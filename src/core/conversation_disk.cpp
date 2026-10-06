#include "strata/core/conversation_disk.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <tuple>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <vector>

#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

#if defined(STRATA_HAVE_ZSTD)
#include <zstd.h>
#endif

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

// ---- checkpoint state codec (format 2): the GDN recurrence and PLE history are fp32 (as the engine holds them)
// whose low mantissa bytes are noise; split into byte planes, the sign/exponent planes compress and zstd -1 takes
// ~14% off at ~1 GB/s.  Lossless.
constexpr uint8_t kRaw = 0, kShuffleZstd = 1;
constexpr size_t kPackMin = 64 * 1024;

#if defined(STRATA_HAVE_ZSTD)
void shuffle4(const uint8_t* in, uint8_t* out, size_t n) {
    const size_t w = n / 4;
    for (size_t i = 0; i < w; ++i)
        for (size_t b = 0; b < 4; ++b) out[b * w + i] = in[4 * i + b];
}
void unshuffle4(const uint8_t* in, uint8_t* out, size_t n) {
    const size_t w = n / 4;
    for (size_t i = 0; i < w; ++i)
        for (size_t b = 0; b < 4; ++b) out[4 * i + b] = in[b * w + i];
}
#endif
// The packed form of v into out, or false when it stays raw (no zstd, too small, not 4-byte rows, no gain).
bool pack(const std::vector<uint8_t>& v, std::vector<uint8_t>& out) {
#if defined(STRATA_HAVE_ZSTD)
    if (v.size() < kPackMin || v.size() % 4) return false;
    std::vector<uint8_t> planes(v.size());
    shuffle4(v.data(), planes.data(), v.size());
    out.resize(ZSTD_compressBound(v.size()));
    const size_t n = ZSTD_compress(out.data(), out.size(), planes.data(), planes.size(), 1);
    if (ZSTD_isError(n) || n >= v.size()) return false;
    out.resize(n);
    return true;
#else
    (void) v; (void) out;
    return false;
#endif
}
bool unpack(const std::vector<uint8_t>& packed, std::vector<uint8_t>& v, size_t n) {
#if defined(STRATA_HAVE_ZSTD)
    if (n % 4 || ZSTD_getFrameContentSize(packed.data(), packed.size()) != n) return false;
    std::vector<uint8_t> planes(n);
    const size_t got = ZSTD_decompress(planes.data(), planes.size(), packed.data(), packed.size());
    if (ZSTD_isError(got) || got != n) return false;
    v.resize(n);
    unshuffle4(planes.data(), v.data(), n);
    return true;
#else
    (void) packed; (void) v; (void) n;
    return false;   // a file packed by a build with zstd: refused, never misread
#endif
}

// ---- the index's byte form
struct Out {
    std::string s;
    void raw(const void* p, size_t n) { if (n) s.append(static_cast<const char*>(p), n); }
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
        if (n) std::memcpy(d, p, n);
        p += n; left -= n;
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
    for (const auto& s : x.slots) {
        o.pod(s.hash); o.pod(s.bytes); o.pod(s.used); o.pod(s.ids_len); o.pod(s.ids_hash);
        o.pod<uint64_t>(s.imgs.size());
        o.raw(s.imgs.data(), s.imgs.size() * sizeof(ConversationImageKey));
    }
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
    x.slots.resize(in.count(48));
    for (auto& s : x.slots) {
        if (!in.ok) break;
        in.pod(s.hash); in.pod(s.bytes); in.pod(s.used); in.pod(s.ids_len); in.pod(s.ids_hash);
        s.imgs.resize(in.count(sizeof(ConversationImageKey)));
        in.raw(s.imgs.data(), s.imgs.size() * sizeof(ConversationImageKey));
    }
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
    bool packed = false;   // format 2: state blobs carry a codec byte
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
    void blob(const std::vector<uint8_t>& v) {
        if (!packed) { vec(v); return; }
        std::vector<uint8_t> z;
        if (!pack(v, z)) { pod(kRaw); vec(v); return; }
        pod(kShuffleZstd);
        pod<uint64_t>(v.size());
        vec(z);
    }
    void checkpoint(const ConversationCheckpoint& c) {
        vec(c.ids); vec(c.imgs); blob(c.gdn); blob(c.ple); vec(c.tails); vec(c.dead); vec(c.block_pos);
        pod<uint64_t>(c.used);
        pod<uint64_t>(c.stage_parts.size());
        for (const auto& p : c.stage_parts) checkpoint(p);
    }
};

struct Reader {
    FILE* f;
    uint64_t left;
    bool ok = true;
    bool packed = false;
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
    void skip(uint64_t n) {
        ok = ok && n <= left && ::fseeko(f, (off_t) n, SEEK_CUR) == 0;
        if (ok) left -= n;
    }
    // a format 1 checkpoint's tokens and pictures; its state is skipped, not read
    void summary(Index::Slot& s, int depth = 0) {
        std::vector<int32_t> ids;
        vec(ids);
        s.ids_len = ids.size();
        s.ids_hash = xxh64(ids.data(), ids.size() * sizeof(int32_t), 0);
        vec(s.imgs);
        for (int i = 0; i < 5 && ok; ++i) skip(count(1));
        pod(s.used);
        const uint64_t parts = count(1);
        if (depth > 0 && parts) ok = false;
        for (uint64_t i = 0; i < parts && ok; ++i) { Index::Slot part; summary(part, depth + 1); }
    }
    void range(ConversationBuffer& b, size_t from) {
        ok = ok && b.visit(from, b.size() - from, [&](uint8_t* p, size_t n, size_t) {
            raw(p, n);
            return ok;
        });
    }
    void buf(ConversationBuffer& b) { b.resize(count(1)); range(b, 0); }
    void blob(std::vector<uint8_t>& v) {
        if (!packed) { vec(v); return; }
        uint8_t codec = 0;
        pod(codec);
        if (!ok || codec == kRaw) { vec(v); return; }
        uint64_t n = 0;
        pod(n);
        std::vector<uint8_t> z;
        vec(z);
        // the raw size is checked against the zstd frame's own before anything that size is allocated
        ok = ok && codec == kShuffleZstd && n <= (16ull << 30) && unpack(z, v, n);
    }
    void checkpoint(ConversationCheckpoint& c, int depth = 0) {
        vec(c.ids); vec(c.imgs); blob(c.gdn); blob(c.ple); vec(c.tails); vec(c.dead); vec(c.block_pos);
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
    // 8 MiB for streaming an image; small for the index reads, which seek (a seek drops the buffer)
    bool open(const std::string& path, const char* mode, size_t buffer = 8u << 20) {
        f = std::fopen(path.c_str(), mode);
        if (!f) return false;
        io.resize(buffer);
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

using Checkpoints = std::vector<const ConversationCheckpoint*>;
Checkpoints all_checkpoints(const SavedConversation& image) {
    Checkpoints out;
    for (const auto& c : image.checkpoints) out.push_back(&c);
    return out;
}

uint64_t key_of(const SavedConversation& image, const Checkpoints& checkpoints) {
    uint64_t h = 1469598103934665603ull;
    h = fnv(h, image.live.ids.data(), image.live.ids.size() * sizeof(int32_t));
    h = fnv(h, image.live.imgs.data(), image.live.imgs.size() * sizeof(ConversationImageKey));
    for (const auto* c : checkpoints) {
        const uint64_t n = c->ids.size();
        h = fnv(h, &n, sizeof n);
    }
    const uint8_t cvec = image.cvec;
    return fnv(h, &cvec, 1);
}

std::string name_of(const SavedConversation& image, const Checkpoints& checkpoints) {
    char name[80];
    std::snprintf(name, sizeof name, "conv-%016llx-%lld.bin",
                  (unsigned long long) key_of(image, checkpoints), (long long) image.live.ids.size());
    return name;
}

Index index_of(const SavedConversation& image, const Checkpoints& checkpoints) {
    Index x;
    x.version = kVersion;
    x.geometry = image.geometry;
    x.layer_lo = image.layer_lo;
    x.layer_hi = image.layer_hi;
    x.cvec = image.cvec;
    x.tokens = image.live.ids.size();
    for (size_t s = 0; s <= checkpoints.size(); ++s) {
        const auto& c = s == 0 ? image.live : *checkpoints[s - 1];
        Index::Slot slot;
        slot.hash = checkpoint_hash(c, 0, slot.bytes);
        slot.used = c.used;
        slot.ids_len = c.ids.size();
        slot.ids_hash = xxh64(c.ids.data(), c.ids.size() * sizeof(int32_t), 0);
        slot.imgs = c.imgs;
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

bool write_image(const SavedConversation& image, const Checkpoints& checkpoints, const Index& self,
                 const std::string& path, const Index* parent, const std::string& parent_name, const Plan* plan,
                 Index& written, std::string& error) {
    const std::string tmp = path + ".tmp";
    File file;
    if (!file.open(tmp, "wb")) { error = "cannot create " + tmp + ": " + std::strerror(errno); return false; }
    Writer w{file.f};
    w.packed = true;
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
    const size_t slots = 1 + checkpoints.size();
    // The slot table first (the parent's slot each one takes, or -1), then the slots written here: a reader
    // knows which parent slots are taken twice before it moves any.
    w.pod<uint64_t>(slots);
    for (size_t s = 0; s < slots; ++s) {
        const auto& c = s == 0 ? image.live : *checkpoints[s - 1];
        w.pod<int64_t>(delta ? plan->slot_ref[s] : -1);
        w.pod<uint64_t>(c.used);
    }
    for (size_t s = 0; s < slots; ++s)
        if (!delta || plan->slot_ref[s] < 0) w.checkpoint(s == 0 ? image.live : *checkpoints[s - 1]);
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
    if (!file.open(path, "rb", 64 * 1024)) { error = "cannot open " + path + ": " + std::strerror(errno); return false; }
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
    if (version == kVersionLegacy) {
        // what matching needs (the slots' tokens and pictures), seeking over the states
        uint8_t cvec = 1;
        r.pod(x.geometry); r.pod(x.layer_lo); r.pod(x.layer_hi); r.pod(cvec);
        x.cvec = cvec != 0;
        x.slots.emplace_back();
        r.summary(x.slots[0]);
        const uint64_t n = r.count(1);
        for (uint64_t i = 0; i < n && r.ok; ++i) { x.slots.emplace_back(); r.summary(x.slots.back()); }
        if (!r.ok) { x = {}; error = path + ": truncated or corrupt"; return false; }
        x.version = version;
        x.tokens = x.slots[0].ids_len;
        x.image_bytes = size;
        return true;
    }
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
    r.packed = true;
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

uint64_t conversation_disk_key(const SavedConversation& image) { return key_of(image, all_checkpoints(image)); }

std::string conversation_disk_name(const SavedConversation& image) { return name_of(image, all_checkpoints(image)); }

ConversationDiskIndex conversation_disk_index(const SavedConversation& image) {
    return index_of(image, all_checkpoints(image));
}

std::vector<size_t> conversation_disk_keep_checkpoints(const SavedConversation& image, uint64_t gap) {
    std::vector<size_t> order(image.checkpoints.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    if (gap == 0 || order.size() <= 3) return order;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return image.checkpoints[a].ids.size() < image.checkpoints[b].ids.size();
    });
    // The root (shortest: the system prompt every chat shares) and the two deepest (resume, or regenerate the
    // last reply) stay.  Between them, oldest first, one is dropped when a request it would have served then
    // resumes from the last one kept instead, reading at most `gap` more tokens.
    std::vector<bool> keep(order.size(), false);
    keep[0] = keep[order.size() - 1] = keep[order.size() - 2] = true;
    uint64_t last = image.checkpoints[order[0]].ids.size();
    for (size_t k = 1; k + 2 < order.size(); ++k) {
        const uint64_t at = image.checkpoints[order[k]].ids.size();
        if (at - last > gap) { keep[k] = true; last = at; }
    }
    std::vector<size_t> out;
    for (size_t k = 0; k < order.size(); ++k) if (keep[k]) out.push_back(order[k]);
    std::sort(out.begin(), out.end());   // the image's own order
    return out;
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
    if (!write_image(image, all_checkpoints(image), self, path, delta ? &pi : nullptr, fs::path(parent).filename().string(),
                     delta ? &plan : nullptr, out, error)) return false;
    if (written) *written = {delta, out.depth, out.payload_bytes, delta ? plan.reused : 0, self.image_bytes};
    return true;
}

bool conversation_disk_read(SavedConversation& image, const std::string& path, std::string& error) {
    return read_image(image, path, 0, nullptr, error);
}

// ---- ConversationStore: the disk tier

namespace {
struct Pending {
    uint64_t id = 0, bytes = 0;
    SavedConversation image;
    bool started = false, done = false, claimed = false;
};
struct DiskEntry {
    Index index;
    uint64_t file_bytes = 0, last_use = 0;
    bool readable = false, conversation = false, foreign = false;   // foreign: not conv-*, never removed
    int pins = 0;                                                    // being read or used as a parent right now
};
uint64_t key_from_name(const std::string& n) {
    if (n.size() < 22 || n.rfind("conv-", 0) != 0) return 0;
    return std::strtoull(n.substr(5, 16).c_str(), nullptr, 16);
}
// what the filesystem holding `dir` has free for an unprivileged writer (UINT64_MAX: unknown)
uint64_t free_bytes(const std::string& dir) {
    struct statvfs v {};
    if (::statvfs(dir.c_str(), &v) != 0) return UINT64_MAX;
    return (uint64_t) v.f_bavail * (uint64_t) v.f_frsize;
}
// The writer runs beside decoding (whose CPU experts want every core) and beside restores from the same disk:
// lowest CPU and I/O priority, where the OS allows it (a container may not).  Returns what was set.
std::string lower_this_thread() {
#if defined(__linux__)
    const long tid = ::syscall(SYS_gettid);
    std::string got;
    got += ::setpriority(PRIO_PROCESS, (id_t) tid, 15) == 0 ? "nice 15" : "nice unchanged (" + std::string(std::strerror(errno)) + ")";
    constexpr int kWhoProcess = 1, kShift = 13, kIdle = 3, kBestEffort = 2;   // linux/ioprio.h
    if (::syscall(SYS_ioprio_set, kWhoProcess, (int) tid, kIdle << kShift) == 0) got += ", I/O idle";
    else if (::syscall(SYS_ioprio_set, kWhoProcess, (int) tid, (kBestEffort << kShift) | 7) == 0) got += ", I/O best-effort 7";
    else got += ", I/O priority unchanged (" + std::string(std::strerror(errno)) + ")";
    return got;
#else
    return "priority unchanged (not Linux)";
#endif
}
double since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}
constexpr const char* kStoreManifest = "conversations.manifest";
} // namespace

struct ConversationStore::Impl {
    std::string dir;
    ConversationStoreOptions opt;
    Log log = [](const std::string&) {};
    mutable std::mutex mu;
    std::condition_variable cv;
    std::map<std::string, DiskEntry> disk;
    std::deque<std::shared_ptr<Pending>> queue;   // the front one may be being written
    uint64_t pending_bytes = 0, next_id = 1, clock = 0;
    ConversationStoreStats st;
    bool stop = false, held = false, open = false;
    std::thread writer;

    std::string path(const std::string& n) const { return (fs::path(dir) / n).string(); }

    // ---- everything below until write() runs with `mu` held
    bool chain_ok(const std::string& n, int depth = 0) const {
        auto it = disk.find(n);
        if (depth > kMaxReadDepth || it == disk.end() || !it->second.readable) return false;
        const Index& x = it->second.index;
        if (x.version == kVersionLegacy || !x.delta) return true;
        auto p = disk.find(x.parent);
        return p != disk.end() && p->second.readable && p->second.index.version == kVersion &&
               p->second.index.fingerprint == x.parent_fingerprint && chain_ok(x.parent, depth + 1);
    }
    std::vector<std::string> chain(const std::string& n) const {   // n, then its parents
        std::vector<std::string> out;
        std::string at = n;
        for (int d = 0; d <= kMaxReadDepth; ++d) {
            auto it = disk.find(at);
            if (it == disk.end()) break;
            out.push_back(at);
            if (!it->second.readable || !it->second.index.delta) break;
            at = it->second.index.parent;
        }
        return out;
    }
    uint64_t chain_bytes(const std::string& n) const {
        uint64_t b = 0;
        for (const auto& c : chain(n)) b += disk.at(c).file_bytes;
        return b;
    }
    void pin(const std::string& n, int d) { for (const auto& c : chain(n)) disk[c].pins += d; }

    // #342 on disk: the conversations whose deepest checkpoint the new file holds are a turn back of it
    void supersede(const std::string& name) {
        const Index& x = disk[name].index;
        auto holds = [&](const Index::Slot& s) {
            for (const auto& t : x.slots)
                if (t.ids_len == s.ids_len && t.ids_hash == s.ids_hash && t.imgs == s.imgs) return true;
            return false;
        };
        for (auto& [n, e] : disk) {
            if (n == name || !e.conversation || e.foreign || !e.readable || e.index.cvec != x.cvec || e.index.slots.size() < 2)
                continue;
            const Index::Slot* deepest = nullptr;
            for (size_t i = 1; i < e.index.slots.size(); ++i)
                if (!deepest || e.index.slots[i].ids_len > deepest->ids_len) deepest = &e.index.slots[i];
            if (deepest->ids_len > 0 && holds(*deepest)) {
                e.conversation = false;
                log("conversation dir: " + n + " superseded by " + name);
            }
        }
    }

    void write_manifest() {
        std::vector<std::pair<uint64_t, std::string>> order;
        for (const auto& [n, e] : disk) if (e.conversation) order.emplace_back(e.last_use, n);
        std::sort(order.begin(), order.end());
        const std::string m = path(kStoreManifest), tmp = m + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "w");
        bool ok = f != nullptr;
        if (f) {
            for (const auto& [use, n] : order) ok = ok && std::fprintf(f, "%s %llu\n", n.c_str(), (unsigned long long) use) > 0;
            ok = std::fflush(f) == 0 && ::fsync(fileno(f)) == 0 && ok;
            ok = std::fclose(f) == 0 && ok;
        }
        std::error_code ec;
        if (ok) fs::rename(tmp, m, ec);
        if (!ok || ec) { log("conversation dir: cannot write " + m); std::remove(tmp.c_str()); }
        sync_dir(dir);
    }

    // The budget (least recently used conversations go, never the last one), then every file no conversation
    // needs.  The manifest is written before anything is removed.
    void collect() {
        auto needed = [&] {
            std::set<std::string> keep;
            uint64_t bytes = 0;
            for (const auto& [n, e] : disk)
                if (e.conversation || e.pins || e.foreign)
                    for (const auto& c : chain(n))
                        if (keep.insert(c).second) bytes += disk.at(c).file_bytes;
            return std::make_pair(keep, bytes);
        };
        auto [keep, bytes] = needed();
        while (bytes > opt.budget_bytes) {
            std::string victim;
            size_t conversations = 0;
            for (const auto& [n, e] : disk) {
                if (!e.conversation || e.foreign) continue;
                ++conversations;
                if (!e.pins && (victim.empty() || e.last_use < disk[victim].last_use)) victim = n;
            }
            if (victim.empty() || conversations <= 1) break;
            disk[victim].conversation = false;
            ++st.evicted;
            log("conversation dir: " + victim + " dropped (over the " + std::to_string(opt.budget_bytes >> 20) +
                " MiB budget)");
            std::tie(keep, bytes) = needed();
        }
        write_manifest();
        for (auto it = disk.begin(); it != disk.end();) {
            if (keep.count(it->first) || it->second.pins || it->second.foreign) { ++it; continue; }
            std::error_code ec;
            fs::remove(path(it->first), ec);
            ++st.removed;
            it = disk.erase(it);
        }
    }

    // ---- one image to disk: the writer thread, or persist() on the request thread while the writer is idle.
    // The catalog is locked to plan and to commit, not while hashing or writing.
    struct Outcome {
        bool written = false, unchanged = false, delta = false, failed = false, skipped = false;
        uint64_t bytes = 0, image_bytes = 0;
        int dropped = 0;
        std::string name;
    };
    Outcome write(const SavedConversation& im) {
        Outcome out;
        Checkpoints cks;
        for (size_t i : conversation_disk_keep_checkpoints(im, opt.policy.checkpoint_gap)) cks.push_back(&im.checkpoints[i]);
        out.dropped = (int) (im.checkpoints.size() - cks.size());
        out.name = name_of(im, cks);
        {
            std::lock_guard<std::mutex> lk(mu);
            auto it = disk.find(out.name);
            if (it != disk.end() && chain_ok(out.name)) {
                it->second.conversation = true;
                it->second.last_use = ++clock;
                std::error_code ec;
                fs::last_write_time(path(out.name), fs::file_time_type::clock::now(), ec);
                out.unchanged = true;
                supersede(out.name);
                collect();
                return out;
            }
        }
        const Index self = index_of(im, cks);
        out.image_bytes = self.image_bytes;
        std::string parent;
        Plan plan;
        Index named;
        bool delta = false;
        {
            std::lock_guard<std::mutex> lk(mu);
            for (const auto& [n, e] : disk) {
                Plan p;
                if (n == out.name || !e.readable || e.index.version != kVersion || !chain_ok(n) ||
                    !plan_delta(self, e.index, p) || p.reused == 0)
                    continue;
                if (parent.empty() || p.reused > plan.reused) { parent = n; plan = std::move(p); }
            }
            if (!parent.empty()) {
                named = disk[parent].index;
                const uint64_t payload = self.image_bytes - plan.reused;
                delta = named.depth + 1 <= opt.policy.max_depth &&
                        (double) (named.chain_bytes + payload) <= opt.policy.max_chain_fraction * (double) named.base_bytes;
                if (delta) pin(parent, +1);   // collect() must not remove it while the delta is written
            }
        }
        // never fill the disk: what the file will take at most (the state codec only shrinks it) must leave the floor
        const uint64_t need = self.image_bytes - (delta ? plan.reused : 0);
        if (const uint64_t free = free_bytes(dir); free != UINT64_MAX && free < opt.min_free_bytes + need) {
            std::lock_guard<std::mutex> lk(mu);
            if (delta) pin(parent, -1);
            ++st.dropped;
            out.skipped = true;
            log("conversation dir: " + out.name + " not written: " + std::to_string(free >> 20) + " MiB free, " +
                std::to_string(need >> 20) + " MiB needed above the " + std::to_string(opt.min_free_bytes >> 20) +
                " MiB floor");
            return out;
        }
        Index written;
        std::string err;
        const bool ok = write_image(im, cks, self, path(out.name), delta ? &named : nullptr, parent,
                                    delta ? &plan : nullptr, written, err);
        std::error_code ec;
        const uint64_t size = ok ? fs::file_size(path(out.name), ec) : 0;
        if (ok) sync_dir(dir);
        std::lock_guard<std::mutex> lk(mu);
        if (delta) pin(parent, -1);
        if (!ok) {
            out.failed = true;
            log("conversation dir: " + err);
            return out;
        }
        auto& e = disk[out.name];
        const int pins = e.pins;
        e = DiskEntry{};
        e.index = std::move(written);
        e.file_bytes = size;
        e.readable = e.conversation = true;
        e.last_use = ++clock;
        e.pins = pins;
        out.written = true;
        out.delta = delta;
        out.bytes = size;
        supersede(out.name);
        collect();
        return out;
    }

    void run() {
        log("conversation dir: writer thread at " + lower_this_thread());
        std::unique_lock<std::mutex> lk(mu);
        for (;;) {
            cv.wait(lk, [&] { return (stop && queue.empty()) || (!held && !queue.empty() && !queue.front()->started); });
            if (queue.empty()) return;
            auto item = queue.front();
            item->started = true;
            lk.unlock();
            if (opt.test_write_delay_ms > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(opt.test_write_delay_ms));
            const auto t0 = std::chrono::steady_clock::now();
            const Outcome o = write(item->image);
            const double secs = since(t0);
            char line[256];
            std::snprintf(line, sizeof line, "conversation dir: spilled %s (%zu tokens): %s%s, %.0f MB in %.1f s",
                          o.name.c_str(), item->image.live.ids.size(),
                          o.failed ? "FAILED" : o.skipped ? "DROPPED (disk full)" : o.unchanged ? "already on disk"
                                   : o.delta ? "delta" : "whole",
                          o.dropped ? (", " + std::to_string(o.dropped) + " checkpoints left out").c_str() : "",
                          o.bytes / 1e6, secs);
            log(line);
            lk.lock();
            ++st.spills;
            st.spill_written += o.written;
            st.spill_unchanged += o.unchanged;
            st.spill_bytes += o.bytes;
            st.spill_image_bytes += o.image_bytes;
            st.spill_seconds += secs;
            item->done = true;
            queue.pop_front();
            pending_bytes -= item->bytes;   // a claimer counts it from here on
            SavedConversation drop;
            if (!item->claimed) drop = std::move(item->image);
            cv.notify_all();
            lk.unlock();
            drop = {};   // freed outside the lock
            lk.lock();
        }
    }
};

ConversationStore::ConversationStore() : impl_(std::make_unique<Impl>()) {}

ConversationStore::~ConversationStore() {
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->stop = true;
        impl_->held = false;
    }
    impl_->cv.notify_all();
    if (impl_->writer.joinable()) impl_->writer.join();
}

bool ConversationStore::open(const std::string& dir, const ConversationStoreOptions& options, Log log) {
    auto& m = *impl_;
    std::lock_guard<std::mutex> lk(m.mu);
    if (m.open) return false;
    m.dir = dir;
    m.opt = options;
    if (log) m.log = std::move(log);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) { m.log("conversation dir: " + dir + " is not a directory"); return false; }
    std::map<std::string, uint64_t> manifest;
    if (FILE* f = std::fopen(m.path(kStoreManifest).c_str(), "r")) {
        char name[512];
        unsigned long long use = 0;
        while (std::fscanf(f, "%511s %llu", name, &use) == 2) manifest[name] = use;
        std::fclose(f);
    }
    std::vector<std::pair<fs::file_time_type, std::string>> unlisted;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        const std::string n = de.path().filename().string();
        const bool conv = n.rfind("conv-", 0) == 0;
        if (!de.is_regular_file(ec)) continue;
        if (conv && de.path().extension() != ".bin") { fs::remove(de.path(), ec); continue; }   // a torn write
        if (de.path().extension() != ".bin") continue;
        DiskEntry e;
        std::string err;
        e.readable = read_index_file(e.index, de.path().string(), err);
        if (!e.readable) m.log("conversation dir: " + n + ": " + err);
        e.file_bytes = de.file_size(ec);
        e.foreign = !conv;
        m.disk[n] = std::move(e);
        if (!manifest.count(n)) unlisted.emplace_back(de.last_write_time(ec), n);
    }
    std::set<std::string> parents;
    for (const auto& [n, e] : m.disk) if (e.readable && e.index.delta) parents.insert(e.index.parent);
    for (auto& [n, e] : m.disk) {
        e.conversation = e.readable && (manifest.count(n) || !parents.count(n));
        if (auto it = manifest.find(n); it != manifest.end()) { e.last_use = it->second; m.clock = std::max(m.clock, it->second); }
    }
    std::sort(unlisted.begin(), unlisted.end());   // files the manifest does not know: after it, oldest first
    for (const auto& [when, n] : unlisted) m.disk[n].last_use = ++m.clock;
    for (auto& [n, e] : m.disk)
        if (e.readable && e.index.delta && !m.chain_ok(n)) {
            m.log("conversation dir: " + n + ": broken chain (parent " + e.index.parent + "), dropped");
            e.readable = e.conversation = false;
        }
    if (m.opt.budget_bytes == 0) {
        uint64_t ours = 0;
        for (const auto& [n, e] : m.disk) if (!e.foreign) ours += e.file_bytes;
        const uint64_t free = free_bytes(dir);
        const uint64_t room = free == UINT64_MAX ? (80ull << 30) : (uint64_t) (0.7 * (double) (free + ours));
        m.opt.budget_bytes = std::min<uint64_t>(80ull << 30, room);
        m.log("conversation dir: budget " + std::to_string(m.opt.budget_bytes >> 20) + " MiB (" +
              (free == UINT64_MAX ? std::string("free space unknown") :
               std::to_string(free >> 20) + " MiB free, " + std::to_string(ours >> 20) + " MiB ours") + ")");
    }
    m.collect();
    m.open = true;
    m.writer = std::thread([this] { impl_->run(); });
    return true;
}

bool ConversationStore::enabled() const { return impl_->open; }

void ConversationStore::spill(SavedConversation&& image) {
    auto& m = *impl_;
    if (!m.open) return;
    // A conversation shorter than min_gain is never worth a restore (worth()), yet its checkpoint state alone is
    // ~200 MB on disk: the integration test spent 31 s writing a 327-token one.  QUIT/PERSIST still write them.
    size_t longest = image.live.ids.size();
    for (const auto& c : image.checkpoints) longest = std::max(longest, c.ids.size());
    if ((int64_t) longest < m.opt.min_gain) {
        {
            std::lock_guard<std::mutex> lk(m.mu);
            ++m.st.too_short;
        }
        m.log("conversation dir: not spilled: " + std::to_string(longest) + " tokens, below the " +
              std::to_string(m.opt.min_gain) + " a restore needs");
        return;
    }
    auto item = std::make_shared<Pending>();
    item->bytes = image.bytes();
    item->image = std::move(image);
    std::unique_lock<std::mutex> lk(m.mu);
    item->id = m.next_id++;
    const auto t0 = std::chrono::steady_clock::now();
    m.cv.wait(lk, [&] { return m.pending_bytes == 0 || m.pending_bytes + item->bytes <= m.opt.pending_bytes; });
    m.st.spill_wait_seconds += since(t0);
    m.queue.push_back(item);
    m.pending_bytes += item->bytes;
    m.cv.notify_all();
    if (!m.opt.background) m.cv.wait(lk, [&] { return item->done; });
}

void ConversationStore::flush() {
    auto& m = *impl_;
    std::unique_lock<std::mutex> lk(m.mu);
    m.cv.wait(lk, [&] { return m.queue.empty(); });
}

ConversationDiskStats ConversationStore::persist(const std::vector<const SavedConversation*>& images) {
    auto& m = *impl_;
    ConversationDiskStats out;
    if (!m.open) return out;
    flush();
    uint64_t removed0;
    {
        std::lock_guard<std::mutex> lk(m.mu);
        removed0 = m.st.removed;
    }
    for (const SavedConversation* im : images) {
        const auto o = m.write(*im);
        out.written += o.written;
        out.deltas += o.delta;
        out.unchanged += o.unchanged;
        out.failed += o.failed;
        out.bytes += o.bytes;
        out.image_bytes += o.written ? o.image_bytes : 0;
        out.dropped_checkpoints += o.written ? o.dropped : 0;
    }
    std::lock_guard<std::mutex> lk(m.mu);
    out.removed = (int) (m.st.removed - removed0);
    return out;
}

ConversationStoreMatch ConversationStore::best(const std::vector<int32_t>& prompt,
                                               const std::vector<ConversationImageKey>& images, bool cvec) const {
    auto& m = *impl_;
    ConversationStoreMatch best;
    std::lock_guard<std::mutex> lk(m.mu);
    // queued images first: they need no read
    for (const auto& item : m.queue) {
        if (item->claimed || item->image.cvec != cvec) continue;
        auto consider = [&](const ConversationCheckpoint& c, bool live) {
            const int64_t n = conversation_prefix(c, prompt, images);
            if (n > best.tokens) {
                best = {};
                best.tokens = n;
                best.live = live;
                best.pending = item->id;
                best.image_bytes = item->bytes;
            }
        };
        consider(item->image.live, true);
        for (const auto& c : item->image.checkpoints) consider(c, false);
    }
    std::map<uint64_t, uint64_t> hashes;   // the prompt's leading tokens, by length
    for (const auto& [n, e] : m.disk) {
        if (!e.conversation || !e.readable || e.index.cvec != cvec) continue;
        for (size_t s = 0; s < e.index.slots.size(); ++s) {
            const auto& slot = e.index.slots[s];
            const uint64_t len = slot.ids_len;
            // as conversation_prefix: the last prompt token always starts the next verify window
            if (len == 0 || len >= prompt.size() || (int64_t) len <= best.tokens) continue;
            size_t j = 0;
            bool same = true;
            for (const auto& image : images) {
                if (image.start >= (int64_t) len) continue;
                if (j == slot.imgs.size() || !(slot.imgs[j++] == image)) { same = false; break; }
            }
            if (!same || j != slot.imgs.size()) continue;
            auto h = hashes.find(len);
            if (h == hashes.end()) h = hashes.emplace(len, xxh64(prompt.data(), len * sizeof(int32_t), 0)).first;
            if (h->second != slot.ids_hash || !m.chain_ok(n)) continue;
            best = {};
            best.tokens = (int64_t) len;
            best.live = s == 0;
            best.name = n;
            best.read_bytes = m.chain_bytes(n);
            best.image_bytes = e.index.image_bytes;
        }
    }
    return best;
}

bool ConversationStore::worth(const ConversationStoreMatch& m, int64_t have) const {
    if (m.tokens <= have) return false;
    if (m.pending) return true;   // in RAM still
    const int64_t gain = m.tokens - have;
    if (gain < impl_->opt.min_gain) return false;
    const double read = (double) m.read_bytes / (impl_->opt.read_mb_s * 1e6);
    const auto& o = impl_->opt;
    const double prefill = (double) gain / (gain >= o.prefill_split ? o.prefill_tok_s : o.prefill_small_tok_s);
    return prefill > read;
}

bool ConversationStore::take(const ConversationStoreMatch& match, SavedConversation& image, std::string& error) {
    auto& m = *impl_;
    if (match.pending) {
        std::unique_lock<std::mutex> lk(m.mu);
        auto it = std::find_if(m.queue.begin(), m.queue.end(), [&](const auto& p) { return p->id == match.pending; });
        if (it == m.queue.end()) { error = "the spilled conversation was written meanwhile"; return false; }
        auto item = *it;
        if (!item->started) {
            m.queue.erase(it);
            m.pending_bytes -= item->bytes;
            image = std::move(item->image);
        } else {
            // Being written: a copy now, not the image after the write (the integration test waited 11 s for a
            // 620 MB write to the USB disk).  The writer only reads the original; claimed, it leaves it to be
            // freed with the item.  Until then the copy and the original are both in RAM.
            item->claimed = true;
            lk.unlock();
            image = item->image;
            lk.lock();
        }
        ++m.st.reclaimed;
        m.st.hit_tokens += (uint64_t) match.tokens;
        m.cv.notify_all();
        return true;
    }
    if (!load(match.name, image, error)) return false;
    std::lock_guard<std::mutex> lk(m.mu);
    m.st.hit_tokens += (uint64_t) match.tokens;
    return true;
}

bool ConversationStore::load(const std::string& name, SavedConversation& image, std::string& error) {
    auto& m = *impl_;
    uint64_t bytes = 0;
    {
        std::lock_guard<std::mutex> lk(m.mu);
        auto it = m.disk.find(name);
        if (it == m.disk.end() || !it->second.readable) { error = name + ": not in the conversation dir"; return false; }
        m.pin(name, +1);
        it->second.last_use = ++m.clock;
        bytes = m.chain_bytes(name);
    }
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = read_image(image, m.path(name), 0, nullptr, error);
    const double secs = since(t0);
    std::lock_guard<std::mutex> lk(m.mu);
    m.pin(name, -1);
    if (ok) {
        ++m.st.hits;
        m.st.read_bytes += bytes;
        m.st.read_seconds += secs;
    } else if (auto it = m.disk.find(name); it != m.disk.end()) {
        it->second.readable = it->second.conversation = false;   // not offered again; collect() removes it
        m.collect();
    }
    return ok;
}

std::vector<std::string> ConversationStore::recent(size_t n) const {
    auto e = entries();
    std::vector<std::string> out;
    for (auto it = e.rbegin(); it != e.rend() && out.size() < n; ++it) out.push_back(it->name);
    return out;
}

std::vector<ConversationStoreEntry> ConversationStore::entries() const {
    auto& m = *impl_;
    std::lock_guard<std::mutex> lk(m.mu);
    std::vector<std::pair<uint64_t, ConversationStoreEntry>> order;
    for (const auto& [n, e] : m.disk)
        if (e.conversation)
            order.push_back({e.last_use, {n, key_from_name(n), (int64_t) e.index.tokens, e.file_bytes}});
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<ConversationStoreEntry> out;
    for (auto& [use, e] : order) out.push_back(std::move(e));
    return out;
}

ConversationStoreStats ConversationStore::stats() const {
    auto& m = *impl_;
    std::lock_guard<std::mutex> lk(m.mu);
    ConversationStoreStats s = m.st;
    s.conversations = s.files = s.disk_bytes = 0;
    for (const auto& [n, e] : m.disk) {
        s.conversations += e.conversation;
        ++s.files;
        s.disk_bytes += e.file_bytes;
    }
    s.pending = m.queue.size();
    s.pending_bytes = m.pending_bytes;
    return s;
}

uint64_t ConversationStore::budget_bytes() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->opt.budget_bytes;
}

uint64_t ConversationStore::pending_bytes() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->pending_bytes;
}

void ConversationStore::hold_writes(bool hold) {
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->held = hold;
    }
    impl_->cv.notify_all();
}

} // namespace strata::core
