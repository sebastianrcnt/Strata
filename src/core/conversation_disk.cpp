#include "strata/core/conversation_disk.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace strata::core {
namespace {

namespace fs = std::filesystem;
using Index = ConversationDiskIndex;

constexpr char kMagic[8] = {'S', 'T', 'R', 'C', 'O', 'N', 'V', '1'};
constexpr uint32_t kVersionLegacy = 1;   // whole image, no index
constexpr uint32_t kVersion = 2;         // whole image or delta, then the index
constexpr uint8_t kWhole = 0, kDelta = 1;
constexpr size_t kChunk = Index::chunk_bytes;
constexpr int kMaxReadDepth = 64;        // far above any policy; stops a cycle of hand-renamed files
constexpr uint64_t kMaxIndexBytes = 256ull << 20;
constexpr const char* kManifest = "conversations.manifest";

uint64_t fnv(uint64_t h, const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

// XXH64 (the reference algorithm): ~10 GB/s here, so hashing a 2 GB image costs a fraction of writing it.
constexpr uint64_t P1 = 11400714785074694791ull, P2 = 14029467366897019727ull, P3 = 1609587929392839161ull,
                   P4 = 9650029242287828579ull, P5 = 2870177450012600261ull;
inline uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline uint64_t load64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline uint32_t load32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t xround(uint64_t acc, uint64_t in) { acc += in * P2; acc = rotl(acc, 31); return acc * P1; }
inline uint64_t xmerge(uint64_t acc, uint64_t v) { acc ^= xround(0, v); return acc * P1 + P4; }
uint64_t xxh64(const void* data, size_t len, uint64_t seed) {
    const auto* p = static_cast<const uint8_t*>(data);
    const uint8_t* end = p + len;
    uint64_t h;
    if (len >= 32) {
        const uint8_t* limit = end - 32;
        uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        do {
            v1 = xround(v1, load64(p)); v2 = xround(v2, load64(p + 8));
            v3 = xround(v3, load64(p + 16)); v4 = xround(v4, load64(p + 24));
            p += 32;
        } while (p <= limit);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = xmerge(h, v1); h = xmerge(h, v2); h = xmerge(h, v3); h = xmerge(h, v4);
    } else {
        h = seed + P5;
    }
    h += len;
    for (; p + 8 <= end; p += 8) h = rotl(h ^ xround(0, load64(p)), 27) * P1 + P4;
    if (p + 4 <= end) { h = rotl(h ^ (uint64_t) load32(p) * P1, 23) * P2 + P3; p += 4; }
    for (; p < end; ++p) h = rotl(h ^ (*p * P5), 11) * P1;
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
    return h;
}

std::vector<uint64_t> chunk_hashes(const ConversationBuffer& b) {
    std::vector<uint64_t> out;
    out.reserve((b.size() + kChunk - 1) / kChunk);
    std::vector<uint8_t> scratch;
    for (size_t at = 0; at < b.size(); at += kChunk) {
        const size_t n = std::min(kChunk, b.size() - at);
        const uint8_t* direct = nullptr;
        b.visit(at, n, [&](const uint8_t* p, size_t m, size_t) { if (m == n) direct = p; return false; });
        if (!direct) {   // the chunk straddles two segments
            scratch.resize(n);
            b.read(scratch.data(), at, n);
            direct = scratch.data();
        }
        out.push_back(xxh64(direct, n, 0x5354524b56ull));
    }
    return out;
}

template<class T> void mix(uint64_t& h, const std::vector<T>& v) { h = xxh64(v.data(), v.size() * sizeof(T), h); }
// Everything restore reads back but the top level `used` (an LRU stamp the index keeps beside it).
uint64_t checkpoint_hash(const ConversationCheckpoint& c, uint64_t h, uint64_t& bytes) {
    mix(h, c.ids); mix(h, c.imgs); mix(h, c.gdn); mix(h, c.ple); mix(h, c.tails); mix(h, c.dead); mix(h, c.block_pos);
    bytes += c.ids.size() * sizeof(int32_t) + c.imgs.size() * sizeof(ConversationImageKey) + c.gdn.size() +
             c.ple.size() + c.tails.size() + c.dead.size() + c.block_pos.size();
    const uint64_t parts = c.stage_parts.size();
    h = xxh64(&parts, sizeof parts, h);
    for (const auto& p : c.stage_parts) {
        h = xxh64(&p.used, sizeof p.used, h);
        h = checkpoint_hash(p, h, bytes);
    }
    return h;
}

// ---- the index's byte form
struct Out {
    std::string s;
    void raw(const void* p, size_t n) { s.append(static_cast<const char*>(p), n); }
    template<class T> void pod(const T& v) { raw(&v, sizeof v); }
    void str(const std::string& v) { pod<uint64_t>(v.size()); raw(v.data(), v.size()); }
};
struct In {
    const uint8_t* p;
    size_t left;
    bool ok = true;
    void raw(void* d, size_t n) {
        ok = ok && n <= left;
        if (!ok) return;
        std::memcpy(d, p, n); p += n; left -= n;
    }
    template<class T> void pod(T& v) { raw(&v, sizeof v); }
    uint64_t count(size_t elem) {
        uint64_t n = 0;
        pod(n);
        if (ok && elem && n > left / elem) ok = false;
        return ok ? n : 0;
    }
    void str(std::string& v) { v.resize(count(1)); raw(v.data(), v.size()); }
};

std::string describe(const Index& x) {
    Out o;
    o.pod(x.geometry); o.pod(x.layer_lo); o.pod(x.layer_hi); o.pod<uint8_t>(x.cvec);
    o.pod<uint64_t>(x.slots.size());
    for (const auto& s : x.slots) { o.pod(s.hash); o.pod(s.bytes); o.pod(s.used); }
    o.pod<uint64_t>(x.kv.size());
    for (const auto& l : x.kv) {
        o.pod(l.format); o.pod(l.cells); o.pod(l.heads); o.pod(l.head_dim); o.pod(l.page_size);
        o.pod(l.pooled_rows); o.pod(l.idx_dim);
        for (const auto& b : l.buf) {
            o.pod(b.size);
            o.pod<uint64_t>(b.chunks.size());
            o.raw(b.chunks.data(), b.chunks.size() * sizeof(uint64_t));
        }
    }
    return o.s;
}

std::string serialize(const Index& x) {
    Out o;
    o.pod(x.depth); o.pod<uint8_t>(x.delta); o.str(x.parent); o.pod(x.parent_fingerprint);
    o.pod(x.payload_bytes); o.pod(x.base_bytes); o.pod(x.chain_bytes); o.pod(x.image_bytes); o.pod(x.tokens);
    o.s += describe(x);
    o.pod(x.fingerprint);
    return o.s;
}

bool parse(Index& x, const std::string& bytes) {
    In in{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};
    uint8_t delta = 0, cvec = 1;
    in.pod(x.depth); in.pod(delta); in.str(x.parent); in.pod(x.parent_fingerprint);
    in.pod(x.payload_bytes); in.pod(x.base_bytes); in.pod(x.chain_bytes); in.pod(x.image_bytes); in.pod(x.tokens);
    const size_t described = bytes.size() - in.left;
    in.pod(x.geometry); in.pod(x.layer_lo); in.pod(x.layer_hi); in.pod(cvec);
    x.delta = delta != 0;
    x.cvec = cvec != 0;
    x.slots.resize(in.count(24));
    for (auto& s : x.slots) { in.pod(s.hash); in.pod(s.bytes); in.pod(s.used); }
    x.kv.resize(in.count(60));
    for (auto& l : x.kv) {
        if (!in.ok) break;
        in.pod(l.format); in.pod(l.cells); in.pod(l.heads); in.pod(l.head_dim); in.pod(l.page_size);
        in.pod(l.pooled_rows); in.pod(l.idx_dim);
        for (auto& b : l.buf) {
            in.pod(b.size);
            b.chunks.resize(in.count(sizeof(uint64_t)));
            in.raw(b.chunks.data(), b.chunks.size() * sizeof(uint64_t));
            if (b.chunks.size() != (b.size + kChunk - 1) / kChunk) in.ok = false;
        }
    }
    const size_t described_end = bytes.size() - in.left;
    in.pod(x.fingerprint);
    if (!in.ok || in.left != 0 || x.depth > (uint32_t) kMaxReadDepth || x.delta != (x.depth > 0)) return false;
    const uint64_t h = xxh64(bytes.data() + described, described_end - described, 0);
    return h == x.fingerprint;
}

// ---- the file's byte form
struct Writer {
    FILE* f;
    bool ok = true;
    uint64_t n = 0;
    void raw(const void* p, size_t c) {
        ok = ok && (c == 0 || std::fwrite(p, 1, c, f) == c);
        n += c;
    }
    template<class T> void pod(const T& v) { raw(&v, sizeof v); }
    template<class T> void vec(const std::vector<T>& v) {
        pod<uint64_t>(v.size());
        raw(v.data(), v.size() * sizeof(T));
    }
    void str(const std::string& v) { pod<uint64_t>(v.size()); raw(v.data(), v.size()); }
    void range(const ConversationBuffer& b, size_t from) {
        ok = ok && b.visit(from, b.size() - from, [&](const uint8_t* p, size_t c, size_t) {
            raw(p, c);
            return ok;
        });
    }
    void buf(const ConversationBuffer& b) { pod<uint64_t>(b.size()); range(b, 0); }
    void checkpoint(const ConversationCheckpoint& c) {
        vec(c.ids); vec(c.imgs); vec(c.gdn); vec(c.ple); vec(c.tails); vec(c.dead); vec(c.block_pos);
        pod<uint64_t>(c.used);
        pod<uint64_t>(c.stage_parts.size());
        for (const auto& p : c.stage_parts) checkpoint(p);
    }
};

struct Reader {
    FILE* f;
    uint64_t left;
    bool ok = true;
    void raw(void* p, size_t n) {
        ok = ok && n <= left && (n == 0 || std::fread(p, 1, n, f) == n);
        if (ok) left -= n;
    }
    template<class T> void pod(T& v) { raw(&v, sizeof v); }
    uint64_t count(size_t elem) {
        uint64_t n = 0;
        pod(n);
        if (ok && elem && n > left / elem) ok = false;   // a corrupt length must not allocate the moon
        return ok ? n : 0;
    }
    template<class T> void vec(std::vector<T>& v) {
        v.resize(count(sizeof(T)));
        raw(v.data(), v.size() * sizeof(T));
    }
    void str(std::string& v) { v.resize(count(1)); raw(v.data(), v.size()); }
    void range(ConversationBuffer& b, size_t from) {
        ok = ok && b.visit(from, b.size() - from, [&](uint8_t* p, size_t n, size_t) {
            raw(p, n);
            return ok;
        });
    }
    void buf(ConversationBuffer& b) { b.resize(count(1)); range(b, 0); }
    void checkpoint(ConversationCheckpoint& c, int depth = 0) {
        vec(c.ids); vec(c.imgs); vec(c.gdn); vec(c.ple); vec(c.tails); vec(c.dead); vec(c.block_pos);
        pod(c.used);
        const uint64_t parts = count(1);
        if (depth > 0 && parts) ok = false;
        c.stage_parts.resize(parts);
        for (auto& p : c.stage_parts) if (ok) checkpoint(p, depth + 1);
    }
};

struct File {
    FILE* f = nullptr;
    std::vector<char> io;
    ~File() { if (f) std::fclose(f); }
    bool open(const std::string& path, const char* mode) {
        f = std::fopen(path.c_str(), mode);
        if (!f) return false;
        io.resize(8u << 20);
        std::setvbuf(f, io.data(), _IOFBF, io.size());
        return true;
    }
};

std::array<const ConversationBuffer*, 5> buffers(const ConversationKv& k) { return {&k.k, &k.v, &k.k_scale, &k.v_scale, &k.pooled}; }
std::array<ConversationBuffer*, 5> buffers(ConversationKv& k) { return {&k.k, &k.v, &k.k_scale, &k.v_scale, &k.pooled}; }

// What a delta against `parent` keeps from it.
struct Plan {
    std::vector<int64_t> slot_ref;                 // per image slot: the parent's slot, or -1 (written)
    std::vector<std::array<uint64_t, 5>> keep;     // per layer and buffer: leading bytes taken from the parent
    uint64_t reused = 0;
};

bool same_shape(const Index::Layer& a, const Index::Layer& b) {
    return a.format == b.format && a.heads == b.heads && a.head_dim == b.head_dim && a.page_size == b.page_size &&
           a.idx_dim == b.idx_dim;
}

bool plan_delta(const Index& self, const Index& parent, Plan& plan) {
    plan = {};
    if (parent.version != kVersion || parent.geometry != self.geometry || parent.layer_lo != self.layer_lo ||
        parent.layer_hi != self.layer_hi || parent.cvec != self.cvec || parent.kv.size() != self.kv.size())
        return false;
    plan.keep.resize(self.kv.size());
    for (size_t l = 0; l < self.kv.size(); ++l) {
        const auto& a = self.kv[l];
        const auto& b = parent.kv[l];
        if (!same_shape(a, b)) return false;
        for (size_t i = 0; i < 5; ++i) {
            const auto& x = a.buf[i].chunks;
            const auto& y = b.buf[i].chunks;
            size_t same = 0;
            while (same < x.size() && same < y.size() && x[same] == y[same]) ++same;
            const uint64_t keep = std::min<uint64_t>({(uint64_t) same * kChunk, a.buf[i].size, b.buf[i].size});
            plan.keep[l][i] = keep;
            plan.reused += keep;
        }
    }
    plan.slot_ref.assign(self.slots.size(), -1);
    for (size_t s = 0; s < self.slots.size(); ++s)
        for (size_t p = 0; p < parent.slots.size(); ++p)
            if (parent.slots[p].hash == self.slots[s].hash && parent.slots[p].bytes == self.slots[s].bytes) {
                plan.slot_ref[s] = (int64_t) p;
                plan.reused += self.slots[s].bytes;
                break;
            }
    return true;
}

bool sync_dir(const std::string& dir) {
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

// The file's name must stay inside its directory.
bool plain_name(const std::string& n) {
    return !n.empty() && n.size() < 256 && n.find('/') == std::string::npos && n != "." && n != "..";
}

bool write_image(const SavedConversation& image, const Index& self, const std::string& path,
                 const Index* parent, const std::string& parent_name, const Plan* plan, Index& written,
                 std::string& error) {
    const std::string tmp = path + ".tmp";
    File file;
    if (!file.open(tmp, "wb")) { error = "cannot create " + tmp + ": " + std::strerror(errno); return false; }
    Writer w{file.f};
    const bool delta = parent && plan;
    written = self;
    written.delta = delta;
    written.parent = delta ? parent_name : std::string();
    written.parent_fingerprint = delta ? parent->fingerprint : 0;
    w.raw(kMagic, sizeof kMagic);
    w.pod(kVersion);
    w.pod<uint8_t>(delta ? kDelta : kWhole);
    if (delta) { w.str(written.parent); w.pod(written.parent_fingerprint); }
    w.pod(image.geometry);
    w.pod(image.layer_lo);
    w.pod(image.layer_hi);
    w.pod<uint8_t>(image.cvec);
    const size_t slots = 1 + image.checkpoints.size();
    // The slot table first (the parent's slot each one takes, or -1), then the slots written here: a reader
    // knows which parent slots are taken twice before it moves any.
    w.pod<uint64_t>(slots);
    for (size_t s = 0; s < slots; ++s) {
        const auto& c = s == 0 ? image.live : image.checkpoints[s - 1];
        w.pod<int64_t>(delta ? plan->slot_ref[s] : -1);
        w.pod<uint64_t>(c.used);
    }
    for (size_t s = 0; s < slots; ++s)
        if (!delta || plan->slot_ref[s] < 0) w.checkpoint(s == 0 ? image.live : image.checkpoints[s - 1]);
    w.pod<uint64_t>(image.kv.size());
    for (size_t l = 0; l < image.kv.size(); ++l) {
        const auto& k = image.kv[l];
        w.pod(k.format); w.pod(k.cells); w.pod(k.heads); w.pod(k.head_dim); w.pod(k.page_size);
        w.pod(k.pooled_rows); w.pod(k.idx_dim);
        const auto b = buffers(k);
        for (size_t i = 0; i < 5; ++i) {
            const uint64_t keep = delta ? plan->keep[l][i] : 0;
            w.pod<uint64_t>(b[i]->size());
            w.pod(keep);
            w.range(*b[i], keep);
        }
    }
    written.payload_bytes = w.n;
    written.depth = delta ? parent->depth + 1 : 0;
    written.base_bytes = delta ? parent->base_bytes : w.n;
    written.chain_bytes = delta ? parent->chain_bytes + w.n : 0;
    const uint64_t at = w.n;
    const std::string index = serialize(written);
    w.raw(index.data(), index.size());
    w.pod(at);
    w.raw(kMagic, sizeof kMagic);   // trailer: a torn write never reads back as whole
    bool ok = w.ok && std::fflush(file.f) == 0 && ::fsync(fileno(file.f)) == 0;
    ok = std::fclose(file.f) == 0 && ok;
    file.f = nullptr;
    if (!ok) {
        error = "writing " + tmp + " failed: " + std::strerror(errno);
        std::remove(tmp.c_str());
        return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) { error = "rename to " + path + ": " + ec.message(); std::remove(tmp.c_str()); return false; }
    return true;
}

bool read_index_file(Index& x, const std::string& path, std::string& error) {
    x = {};
    std::error_code ec;
    const uint64_t size = fs::file_size(path, ec);
    if (ec) { error = path + ": " + ec.message(); return false; }
    File file;
    if (!file.open(path, "rb")) { error = "cannot open " + path + ": " + std::strerror(errno); return false; }
    Reader r{file.f, size};
    char magic[8] = {};
    uint32_t version = 0;
    r.raw(magic, sizeof magic);
    r.pod(version);
    if (!r.ok || std::memcmp(magic, kMagic, sizeof magic) != 0 || (version != kVersion && version != kVersionLegacy)) {
        error = path + ": not a Strata conversation file (or another version)";
        return false;
    }
    x.version = version;
    if (version == kVersionLegacy) { x.image_bytes = size; return true; }
    uint64_t at = 0;
    if (size < 28 + 16 || ::fseeko(file.f, (off_t) (size - 16), SEEK_SET) != 0) { error = path + ": truncated"; return false; }
    r.left = 16;
    r.pod(at);
    r.raw(magic, sizeof magic);
    if (!r.ok || std::memcmp(magic, kMagic, sizeof magic) != 0 || at < 13 || at > size - 16 ||
        size - 16 - at > kMaxIndexBytes || ::fseeko(file.f, (off_t) at, SEEK_SET) != 0) {
        error = path + ": truncated or corrupt (index)";
        return false;
    }
    std::string bytes(size - 16 - at, '\0');
    r.left = bytes.size();
    r.raw(bytes.data(), bytes.size());
    if (!r.ok || !parse(x, bytes) || x.payload_bytes != at) {
        x = {};
        error = path + ": corrupt index";
        return false;
    }
    x.version = kVersion;
    return true;
}

bool read_legacy(Reader& r, SavedConversation& out) {
    uint8_t cvec = 1;
    r.pod(out.geometry);
    r.pod(out.layer_lo);
    r.pod(out.layer_hi);
    r.pod(cvec);
    out.cvec = cvec != 0;
    r.checkpoint(out.live);
    out.checkpoints.resize(r.count(1));
    for (auto& c : out.checkpoints) if (r.ok) r.checkpoint(c);
    out.kv.resize(r.count(1));
    for (auto& k : out.kv) {
        if (!r.ok) break;
        r.pod(k.format); r.pod(k.cells); r.pod(k.heads); r.pod(k.head_dim); r.pod(k.page_size);
        r.pod(k.pooled_rows); r.pod(k.idx_dim);
        r.buf(k.k); r.buf(k.v); r.buf(k.k_scale); r.buf(k.v_scale); r.buf(k.pooled);
    }
    char magic[8] = {};
    r.raw(magic, sizeof magic);
    return r.ok && r.left == 0 && std::memcmp(magic, kMagic, sizeof magic) == 0;
}

// depth 0 is the file asked for; its parents are read with depth > 0 and only the top is checked against its index.
bool read_image(SavedConversation& image, const std::string& path, int depth, Index* stored, std::string& error) {
    std::error_code ec;
    const uint64_t size = fs::file_size(path, ec);
    if (ec) { error = path + ": " + ec.message(); return false; }
    File file;
    if (!file.open(path, "rb")) { error = "cannot open " + path + ": " + std::strerror(errno); return false; }
    Reader r{file.f, size};
    char magic[8] = {};
    uint32_t version = 0;
    r.raw(magic, sizeof magic);
    r.pod(version);
    if (!r.ok || std::memcmp(magic, kMagic, sizeof magic) != 0 || (version != kVersion && version != kVersionLegacy)) {
        error = path + ": not a Strata conversation file (or another version)";
        return false;
    }
    SavedConversation out;
    if (version == kVersionLegacy) {
        if (depth > 0) { error = path + ": a format 1 file cannot be a delta's parent"; return false; }
        if (!read_legacy(r, out)) { error = path + ": truncated or corrupt"; return false; }
        image = std::move(out);
        return true;
    }
    uint8_t kind = 0, cvec = 1;
    r.pod(kind);
    SavedConversation base;
    std::vector<ConversationCheckpoint> pool;
    const bool delta = kind == kDelta;
    if (!r.ok || (kind != kWhole && kind != kDelta)) { error = path + ": truncated or corrupt"; return false; }
    if (delta) {
        std::string parent;
        uint64_t fingerprint = 0;
        r.str(parent);
        r.pod(fingerprint);
        if (!r.ok || !plain_name(parent)) { error = path + ": corrupt parent reference"; return false; }
        if (depth >= kMaxReadDepth) { error = path + ": delta chain too long (a cycle?)"; return false; }
        const std::string parent_path = (fs::path(path).parent_path() / parent).string();
        Index pi;
        std::string e;
        if (!read_index_file(pi, parent_path, e) || pi.version != kVersion) {
            error = path + ": broken chain: parent " + parent + ": " + (e.empty() ? "format 1" : e);
            return false;
        }
        if (pi.fingerprint != fingerprint) {
            error = path + ": broken chain: parent " + parent + " holds another image now";
            return false;
        }
        if (!read_image(base, parent_path, depth + 1, nullptr, e)) { error = path + ": broken chain: " + e; return false; }
        pool.reserve(1 + base.checkpoints.size());
        pool.push_back(std::move(base.live));
        for (auto& c : base.checkpoints) pool.push_back(std::move(c));
        base.checkpoints.clear();
    }
    r.pod(out.geometry);
    r.pod(out.layer_lo);
    r.pod(out.layer_hi);
    r.pod(cvec);
    out.cvec = cvec != 0;
    const uint64_t slots = r.count(16);
    if (r.ok && slots == 0) r.ok = false;
    std::vector<int64_t> refs(slots);
    std::vector<uint64_t> used(slots);
    std::vector<size_t> uses(pool.size());
    for (size_t s = 0; s < slots && r.ok; ++s) {
        r.pod(refs[s]);
        r.pod(used[s]);
        if (refs[s] >= 0 && (!delta || (uint64_t) refs[s] >= pool.size())) r.ok = false;
        else if (refs[s] >= 0) ++uses[(size_t) refs[s]];
    }
    std::vector<ConversationCheckpoint> got(r.ok ? slots : 0);
    for (size_t s = 0; s < got.size() && r.ok; ++s) {
        auto& c = got[s];
        if (refs[s] < 0) r.checkpoint(c);
        else if (--uses[(size_t) refs[s]] == 0) c = std::move(pool[(size_t) refs[s]]);   // its last taker moves it
        else c = pool[(size_t) refs[s]];
        c.used = used[s];
    }
    pool.clear();
    if (r.ok) {
        out.live = std::move(got[0]);
        out.checkpoints.assign(std::make_move_iterator(got.begin() + 1), std::make_move_iterator(got.end()));
    }
    out.kv.resize(r.count(60));
    if (r.ok && delta && out.kv.size() != base.kv.size()) r.ok = false;
    for (size_t l = 0; l < out.kv.size() && r.ok; ++l) {
        auto& k = out.kv[l];
        r.pod(k.format); r.pod(k.cells); r.pod(k.heads); r.pod(k.head_dim); r.pod(k.page_size);
        r.pod(k.pooled_rows); r.pod(k.idx_dim);
        const auto dst = buffers(k);
        const auto src = delta ? buffers(base.kv[l]) : std::array<ConversationBuffer*, 5>{};
        for (size_t i = 0; i < 5 && r.ok; ++i) {
            uint64_t total = 0, keep = 0;
            r.pod(total);
            r.pod(keep);
            if (!r.ok || keep > total || total - keep > r.left || (!delta && keep) || (delta && keep > src[i]->size())) {
                r.ok = false;
                break;
            }
            // A fresh buffer, not the parent's truncated and regrown: regrowth reserves for appends (up to 16 MiB a
            // buffer), which the cache's byte budget would count.  Costs one copy of the kept bytes.
            dst[i]->resize(total);
            if (keep) r.ok = src[i]->visit(0, keep, [&](const uint8_t* p, size_t n, size_t at) {
                return dst[i]->visit(at, n, [&](uint8_t* q, size_t m, size_t to) {
                    std::memcpy(q, p + (to - at), m);
                    return true;
                });
            });
            if (delta) *src[i] = {};
            r.range(*dst[i], keep);
        }
    }
    base = {};
    // the index, its offset and the trailer
    Index x;
    if (r.ok) {
        const uint64_t at = size - r.left;
        if (r.left < 16 || r.left - 16 > kMaxIndexBytes) r.ok = false;
        std::string bytes(r.ok ? r.left - 16 : 0, '\0');
        r.raw(bytes.data(), bytes.size());
        uint64_t recorded = 0;
        r.pod(recorded);
        r.raw(magic, sizeof magic);
        r.ok = r.ok && r.left == 0 && recorded == at && std::memcmp(magic, kMagic, sizeof magic) == 0 && parse(x, bytes) &&
               x.delta == delta;
    }
    if (!r.ok) { error = path + ": truncated or corrupt"; return false; }
    if (depth == 0) {
        const Index got_index = conversation_disk_index(out);
        if (got_index.fingerprint != x.fingerprint) {
            error = path + ": restored image does not match its index (corrupt file or chain)";
            return false;
        }
    }
    if (stored) *stored = std::move(x);
    image = std::move(out);
    return true;
}

} // namespace

uint64_t conversation_disk_key(const SavedConversation& image) {
    uint64_t h = 1469598103934665603ull;
    h = fnv(h, image.live.ids.data(), image.live.ids.size() * sizeof(int32_t));
    h = fnv(h, image.live.imgs.data(), image.live.imgs.size() * sizeof(ConversationImageKey));
    for (const auto& c : image.checkpoints) {
        const uint64_t n = c.ids.size();
        h = fnv(h, &n, sizeof n);
    }
    const uint8_t cvec = image.cvec;
    return fnv(h, &cvec, 1);
}

std::string conversation_disk_name(const SavedConversation& image) {
    char name[80];
    std::snprintf(name, sizeof name, "conv-%016llx-%lld.bin",
                  (unsigned long long) conversation_disk_key(image), (long long) image.live.ids.size());
    return name;
}

ConversationDiskIndex conversation_disk_index(const SavedConversation& image) {
    Index x;
    x.version = kVersion;
    x.geometry = image.geometry;
    x.layer_lo = image.layer_lo;
    x.layer_hi = image.layer_hi;
    x.cvec = image.cvec;
    x.tokens = image.live.ids.size();
    for (size_t s = 0; s <= image.checkpoints.size(); ++s) {
        const auto& c = s == 0 ? image.live : image.checkpoints[s - 1];
        Index::Slot slot;
        slot.hash = checkpoint_hash(c, 0, slot.bytes);
        slot.used = c.used;
        x.image_bytes += slot.bytes;
        x.slots.push_back(slot);
    }
    for (const auto& k : image.kv) {
        Index::Layer l;
        l.format = k.format; l.cells = k.cells; l.heads = k.heads; l.head_dim = k.head_dim; l.page_size = k.page_size;
        l.pooled_rows = k.pooled_rows; l.idx_dim = k.idx_dim;
        const auto b = buffers(k);
        for (size_t i = 0; i < 5; ++i) {
            l.buf[i].size = b[i]->size();
            l.buf[i].chunks = chunk_hashes(*b[i]);
            x.image_bytes += b[i]->size();
        }
        x.kv.push_back(std::move(l));
    }
    const std::string d = describe(x);
    x.fingerprint = xxh64(d.data(), d.size(), 0);
    return x;
}

bool conversation_disk_read_index(ConversationDiskIndex& index, const std::string& path, std::string& error) {
    return read_index_file(index, path, error);
}

bool conversation_disk_write(const SavedConversation& image, const std::string& path, std::string& error,
                             const std::string& parent, ConversationDiskWritten* written) {
    const Index self = conversation_disk_index(image);
    Index pi, out;
    Plan plan;
    bool delta = false;
    if (!parent.empty()) {
        if (fs::path(parent).parent_path() != fs::path(path).parent_path()) {
            error = "a delta's parent must be in the same directory";
            return false;
        }
        std::string e;
        delta = read_index_file(pi, parent, e) && plan_delta(self, pi, plan) && plan.reused > 0;
    }
    if (!write_image(image, self, path, delta ? &pi : nullptr, fs::path(parent).filename().string(),
                     delta ? &plan : nullptr, out, error)) return false;
    if (written) *written = {delta, out.depth, out.payload_bytes, delta ? plan.reused : 0, self.image_bytes};
    return true;
}

bool conversation_disk_read(SavedConversation& image, const std::string& path, std::string& error) {
    return read_image(image, path, 0, nullptr, error);
}

ConversationDiskStats conversation_disk_persist(const std::string& dir, const std::vector<const SavedConversation*>& images,
                                                const ConversationDiskPolicy& policy,
                                                const std::function<void(const std::string&)>& log) {
    ConversationDiskStats st;
    std::error_code ec;
    fs::create_directories(dir, ec);
    // What is on disk now: every conv-*.bin with its index (format 1 and unreadable files have none).
    struct Entry { Index index; bool readable = false; };
    std::map<std::string, Entry> disk;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        const std::string n = de.path().filename().string();
        if (n.rfind("conv-", 0) != 0 || de.path().extension() != ".bin" || !de.is_regular_file(ec)) continue;
        Entry e;
        std::string err;
        e.readable = read_index_file(e.index, de.path().string(), err);
        disk[n] = std::move(e);
    }
    // A chain is whole when every parent is there and still holds the image the delta was made against.
    std::map<std::string, bool> whole;
    std::function<bool(const std::string&, int)> chain_ok = [&](const std::string& n, int depth) -> bool {
        if (auto it = whole.find(n); it != whole.end()) return it->second;
        bool ok = false;
        auto it = disk.find(n);
        if (depth <= kMaxReadDepth && it != disk.end() && it->second.readable) {
            const Index& x = it->second.index;
            if (x.version == kVersionLegacy || !x.delta) ok = true;
            else if (auto p = disk.find(x.parent); p != disk.end() && p->second.readable &&
                     p->second.index.version == kVersion && p->second.index.fingerprint == x.parent_fingerprint)
                ok = chain_ok(x.parent, depth + 1);
        }
        return whole[n] = ok;
    };
    std::vector<std::string> held;
    for (const SavedConversation* im : images) {
        const std::string name = conversation_disk_name(*im);
        const fs::path path = fs::path(dir) / name;
        held.push_back(name);
        if (disk.count(name) && chain_ok(name, 0)) {
            ++st.unchanged;
            fs::last_write_time(path, fs::file_time_type::clock::now(), ec);   // load order = recency
            continue;
        }
        const Index self = conversation_disk_index(*im);
        // The parent sharing the most bytes with it; a chain grown too long or too heavy is written whole again.
        const std::string* best_name = nullptr;
        Plan best;
        for (const auto& [n, e] : disk) {
            Plan p;
            if (n == name || !e.readable || e.index.version != kVersion || !chain_ok(n, 0) ||
                !plan_delta(self, e.index, p) || p.reused == 0)
                continue;
            if (!best_name || p.reused > best.reused) { best_name = &n; best = std::move(p); }
        }
        const Index* parent = nullptr;
        Index named;
        if (best_name) {
            named = disk[*best_name].index;
            const uint64_t payload = self.image_bytes - best.reused;   // about what the delta will hold
            if (named.depth + 1 <= policy.max_depth &&
                (double) (named.chain_bytes + payload) <= policy.max_chain_fraction * (double) named.base_bytes)
                parent = &named;
        }
        Index out;
        std::string err;
        if (!write_image(*im, self, path.string(), parent, best_name ? *best_name : std::string(),
                         parent ? &best : nullptr, out, err)) {
            ++st.failed;
            log(err);
            disk.erase(name);
            whole.erase(name);
            continue;
        }
        ++st.written;
        st.deltas += parent != nullptr;
        st.bytes += out.payload_bytes;
        st.image_bytes += self.image_bytes;
        disk[name] = {std::move(out), true};
        whole.clear();   // a rewritten file can repair (or, never, break) another's chain
    }
    // Durable files before the manifest names them, and the manifest before anything is removed.
    sync_dir(dir);
    {
        const std::string m = (fs::path(dir) / kManifest).string(), tmp = m + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "w");
        bool ok = f != nullptr;
        if (f) {
            for (const auto& n : held) ok = ok && std::fprintf(f, "%s\n", n.c_str()) > 0;
            ok = std::fflush(f) == 0 && ::fsync(fileno(f)) == 0 && ok;
            ok = std::fclose(f) == 0 && ok;
        }
        if (ok) fs::rename(tmp, m, ec);
        if (!ok || ec) { log("cannot write " + m); std::remove(tmp.c_str()); }
        sync_dir(dir);
    }
    // Keep what is held and every ancestor a held file needs; everything else named conv-* goes.
    std::set<std::string> keep;
    for (const auto& n : held) {
        std::string at = n;
        for (int d = 0; d <= kMaxReadDepth && keep.insert(at).second; ++d) {
            auto it = disk.find(at);
            if (it == disk.end() || !it->second.readable || !it->second.index.delta) break;
            at = it->second.index.parent;
        }
    }
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        const std::string n = de.path().filename().string();
        if (n.rfind("conv-", 0) != 0 || keep.count(n)) continue;
        if (fs::remove(de.path(), ec)) ++st.removed;
    }
    return st;
}

std::vector<std::string> conversation_disk_loadable(const std::string& dir) {
    std::error_code ec;
    std::set<std::string> manifest, ancestors;
    bool have_manifest = false;
    if (FILE* f = std::fopen((fs::path(dir) / kManifest).c_str(), "r")) {
        have_manifest = true;
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            std::string n = line;
            while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
            if (!n.empty()) manifest.insert(n);
        }
        std::fclose(f);
    }
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (!de.is_regular_file(ec) || de.path().extension() != ".bin") continue;
        files.emplace_back(de.last_write_time(ec), de.path());
        Index x;
        std::string e;
        if (read_index_file(x, de.path().string(), e) && x.delta) ancestors.insert(x.parent);
    }
    std::sort(files.begin(), files.end());   // oldest first: the newest ends most recent in the LRU
    std::vector<std::string> out;
    for (const auto& [when, path] : files) {
        const std::string n = path.filename().string();
        // A parent kept only for a held delta is not a conversation of its own (#342 dropped it from the cache);
        // without a manifest every parent is taken for one of those.
        if (ancestors.count(n) && (!have_manifest || !manifest.count(n))) continue;
        out.push_back(path.string());
    }
    return out;
}

} // namespace strata::core
