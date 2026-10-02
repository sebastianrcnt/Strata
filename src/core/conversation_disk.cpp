#include "strata/core/conversation_disk.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace strata::core {
namespace {

constexpr char kMagic[8] = {'S', 'T', 'R', 'C', 'O', 'N', 'V', '1'};
constexpr uint32_t kVersion = 1;

uint64_t fnv(uint64_t h, const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

struct Writer {
    FILE* f;
    bool ok = true;
    void raw(const void* p, size_t n) { ok = ok && (n == 0 || std::fwrite(p, 1, n, f) == n); }
    template<class T> void pod(const T& v) { raw(&v, sizeof v); }
    template<class T> void vec(const std::vector<T>& v) {
        pod<uint64_t>(v.size());
        raw(v.data(), v.size() * sizeof(T));
    }
    void buf(const ConversationBuffer& b) {
        pod<uint64_t>(b.size());
        ok = ok && b.visit(0, b.size(), [&](const uint8_t* p, size_t n, size_t) {
            raw(p, n);
            return ok;
        });
    }
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
    void buf(ConversationBuffer& b) {
        b.resize(count(1));
        ok = ok && b.visit(0, b.size(), [&](uint8_t* p, size_t n, size_t) {
            raw(p, n);
            return ok;
        });
    }
    void checkpoint(ConversationCheckpoint& c, int depth = 0) {
        vec(c.ids); vec(c.imgs); vec(c.gdn); vec(c.ple); vec(c.tails); vec(c.dead); vec(c.block_pos);
        pod(c.used);
        const uint64_t parts = count(1);
        if (depth > 0 && parts) ok = false;
        c.stage_parts.resize(parts);
        for (auto& p : c.stage_parts) if (ok) checkpoint(p, depth + 1);
    }
};

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

bool conversation_disk_write(const SavedConversation& image, const std::string& path, std::string& error) {
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) { error = "cannot create " + tmp + ": " + std::strerror(errno); return false; }
    std::vector<char> io(8u << 20);
    std::setvbuf(f, io.data(), _IOFBF, io.size());
    Writer w{f};
    w.raw(kMagic, sizeof kMagic);
    w.pod(kVersion);
    w.pod(image.geometry);
    w.pod(image.layer_lo);
    w.pod(image.layer_hi);
    w.pod<uint8_t>(image.cvec);
    w.checkpoint(image.live);
    w.pod<uint64_t>(image.checkpoints.size());
    for (const auto& c : image.checkpoints) w.checkpoint(c);
    w.pod<uint64_t>(image.kv.size());
    for (const auto& k : image.kv) {
        w.pod(k.format); w.pod(k.cells); w.pod(k.heads); w.pod(k.head_dim); w.pod(k.page_size);
        w.pod(k.pooled_rows); w.pod(k.idx_dim);
        w.buf(k.k); w.buf(k.v); w.buf(k.k_scale); w.buf(k.v_scale); w.buf(k.pooled);
    }
    w.raw(kMagic, sizeof kMagic);   // trailer: a torn write never reads back as whole
    bool ok = w.ok && std::fflush(f) == 0 && ::fsync(fileno(f)) == 0;
    ok = std::fclose(f) == 0 && ok;
    if (!ok) {
        error = "writing " + tmp + " failed: " + std::strerror(errno);
        std::remove(tmp.c_str());
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) { error = "rename to " + path + ": " + ec.message(); std::remove(tmp.c_str()); return false; }
    return true;
}

bool conversation_disk_read(SavedConversation& image, const std::string& path, std::string& error) {
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(path, ec);
    if (ec) { error = path + ": " + ec.message(); return false; }
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { error = "cannot open " + path + ": " + std::strerror(errno); return false; }
    std::vector<char> io(8u << 20);
    std::setvbuf(f, io.data(), _IOFBF, io.size());
    Reader r{f, size};
    char magic[8] = {};
    uint32_t version = 0;
    r.raw(magic, sizeof magic);
    r.pod(version);
    if (!r.ok || std::memcmp(magic, kMagic, sizeof magic) != 0 || version != kVersion) {
        std::fclose(f);
        error = path + ": not a Strata conversation file (or another version)";
        return false;
    }
    SavedConversation out;
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
    r.raw(magic, sizeof magic);
    const bool ok = r.ok && r.left == 0 && std::memcmp(magic, kMagic, sizeof magic) == 0;
    std::fclose(f);
    if (!ok) { error = path + ": truncated or corrupt"; return false; }
    image = std::move(out);
    return true;
}

} // namespace strata::core
