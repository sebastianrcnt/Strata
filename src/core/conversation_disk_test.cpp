// CPU-only tests of --conversation-dir's file format: whole images, delta chains, broken chains, compaction and
// the persist/GC/load selection.  `conversation_disk_test real <parent.bin> <child.bin> <dir>` instead replays two
// real conversation files (format 1 or 2): the child written as a delta of the parent, read back and compared.
// `conversation_disk_test prefix <child.bin> <checkpoint> <dir>` does the same against the child cut back to one
// of its checkpoints.
#include "strata/core/conversation_disk.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace strata::core;
namespace fs = std::filesystem;

namespace {
int checks = 0;
void check(bool value, const char* description) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", description); std::exit(1); }
}

uint64_t splitmix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
void fill(uint8_t* p, size_t n, uint64_t seed) {
    uint64_t s = splitmix(seed);
    for (size_t i = 0; i < n; i += 8) {
        s = splitmix(s);
        std::memcpy(p + i, &s, std::min<size_t>(8, n - i));
    }
}
void fill(std::vector<uint8_t>& v, size_t n, uint64_t seed) { v.resize(n); fill(v.data(), n, seed); }
uint64_t ids_hash(const std::vector<int32_t>& ids, size_t n) {
    uint64_t h = 77;
    for (size_t i = 0; i < n; ++i) h = splitmix(h ^ (uint32_t) ids[i]);
    return h;
}

// Shapes as the engine lays them out (src/core/conversation_snapshot.cpp, INT8 K/V): cells rounded up to the
// 4-cell page, heads x head_dim bytes of K and of V per cell, 2 bytes of scale per 64 dims, and one pooled indexer
// row (idx_dim floats) per 4 cells plus the moving spare row.  A cell's bytes depend on its token and position;
// the cells past the last token (the partial page) and the spare row change as the conversation grows, as in the
// engine.  Checkpoints carry GDN/PLE/tail state determined by their token prefix.
constexpr int kLayers = 4, kHeads = 2, kHeadDim = 128, kPage = 4, kIdxDim = 128, kBlock = 4;
constexpr size_t kGdn = 512 * 1024;

ConversationCheckpoint checkpoint(const std::vector<int32_t>& ids, size_t n, uint64_t used) {
    ConversationCheckpoint c;
    c.ids.assign(ids.begin(), ids.begin() + (std::ptrdiff_t) n);
    const uint64_t h = ids_hash(ids, n);
    // fp32 state as the engine holds it: small values (rms ~0.016) whose low mantissa bytes are noise
    c.gdn.resize(kGdn);
    for (size_t i = 0; i < kGdn / 4; ++i) {
        const float x = (float) ((int64_t) (splitmix(h ^ (i * 2654435761ull)) % 2001) - 1000) * 1.6e-5f;
        std::memcpy(c.gdn.data() + 4 * i, &x, 4);
    }
    fill(c.ple, 4096, h ^ 2);
    fill(c.tails, 1536, h ^ 3);
    fill(c.dead, 64, h ^ 4);
    fill(c.block_pos, 48, h ^ 5);
    c.used = used;
    return c;
}

SavedConversation image(const std::vector<int32_t>& ids, const std::vector<size_t>& checkpoints, uint64_t used = 1) {
    SavedConversation s;
    for (size_t i = 0; i < s.geometry.size(); ++i) s.geometry[i] = (int64_t) (i * 7 + 3);
    s.layer_lo = 0;
    s.layer_hi = kLayers;
    s.cvec = true;
    const int64_t n = (int64_t) ids.size();
    s.live = checkpoint(ids, ids.size(), 0);
    for (size_t k : checkpoints) s.checkpoints.push_back(checkpoint(ids, k, used++));
    const int64_t cells = (n + kPage - 1) / kPage * kPage;
    for (int l = 0; l <= kLayers; ++l) {   // the last one is the draft layer: no indexer rows
        ConversationKv kv;
        kv.format = 1; kv.cells = cells; kv.heads = kHeads; kv.head_dim = kHeadDim; kv.page_size = kPage;
        kv.idx_dim = kIdxDim;
        kv.pooled_rows = l < kLayers && n > 0 ? n / kBlock + 1 : 0;
        const size_t per_cell[4] = {kHeads * kHeadDim, kHeads * kHeadDim, kHeads * kHeadDim / 64 * 2, kHeads * kHeadDim / 64 * 2};
        ConversationBuffer* bufs[4] = {&kv.k, &kv.v, &kv.k_scale, &kv.v_scale};
        for (int b = 0; b < 4; ++b) {
            bufs[b]->resize((size_t) cells * per_cell[b]);
            std::vector<uint8_t> cell(per_cell[b]);
            for (int64_t c = 0; c < cells; ++c) {
                const uint64_t seed = c < n ? ((uint64_t) l << 56) ^ ((uint64_t) b << 52) ^ ((uint64_t) c << 20) ^ (uint32_t) ids[(size_t) c]
                                            : ((uint64_t) l << 56) ^ ((uint64_t) b << 52) ^ 0xdead00000ull ^ (uint64_t) (c * 131 + n);
                fill(cell.data(), cell.size(), seed);
                bufs[b]->visit((size_t) c * per_cell[b], cell.size(), [&](uint8_t* p, size_t m, size_t at) {
                    std::memcpy(p, cell.data() + (at - (size_t) c * per_cell[b]), m);
                    return true;
                });
            }
        }
        const size_t row = kIdxDim * sizeof(float);
        kv.pooled.resize((size_t) kv.pooled_rows * row);
        std::vector<uint8_t> r(row);
        for (int64_t i = 0; i < kv.pooled_rows; ++i) {
            const bool complete = (i + 1) * kBlock <= n;
            fill(r.data(), row, ((uint64_t) l << 56) ^ (complete ? ids_hash(ids, (size_t) (i + 1) * kBlock) : 0xabcull ^ (uint64_t) n));
            kv.pooled.visit((size_t) i * row, row, [&](uint8_t* p, size_t m, size_t at) {
                std::memcpy(p, r.data() + (at - (size_t) i * row), m);
                return true;
            });
        }
        s.kv.push_back(std::move(kv));
    }
    return s;
}

bool same(const ConversationCheckpoint& a, const ConversationCheckpoint& b) {
    if (a.stage_parts.size() != b.stage_parts.size()) return false;
    for (size_t i = 0; i < a.stage_parts.size(); ++i) if (!same(a.stage_parts[i], b.stage_parts[i])) return false;
    return a.ids == b.ids && a.imgs == b.imgs && a.gdn == b.gdn && a.ple == b.ple && a.tails == b.tails &&
           a.dead == b.dead && a.block_pos == b.block_pos && a.used == b.used;
}
bool same(const SavedConversation& a, const SavedConversation& b) {
    if (a.geometry != b.geometry || a.layer_lo != b.layer_lo || a.layer_hi != b.layer_hi || a.cvec != b.cvec ||
        !same(a.live, b.live) || a.checkpoints.size() != b.checkpoints.size() || a.kv.size() != b.kv.size())
        return false;
    for (size_t i = 0; i < a.checkpoints.size(); ++i) if (!same(a.checkpoints[i], b.checkpoints[i])) return false;
    for (size_t i = 0; i < a.kv.size(); ++i) {
        const auto& x = a.kv[i];
        const auto& y = b.kv[i];
        if (x.format != y.format || x.cells != y.cells || x.heads != y.heads || x.head_dim != y.head_dim ||
            x.page_size != y.page_size || x.pooled_rows != y.pooled_rows || x.idx_dim != y.idx_dim ||
            !(x.k == y.k) || !(x.v == y.v) || !(x.k_scale == y.k_scale) || !(x.v_scale == y.v_scale) ||
            !(x.pooled == y.pooled))
            return false;
    }
    return true;
}

// The format 1 writer as it shipped, to prove those files still read.
void write_v1(const SavedConversation& im, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    auto raw = [&](const void* p, size_t n) { if (n) std::fwrite(p, 1, n, f); };
    auto u64 = [&](uint64_t v) { raw(&v, 8); };
    auto buf = [&](const ConversationBuffer& b) {
        u64(b.size());
        b.visit(0, b.size(), [&](const uint8_t* p, size_t n, size_t) { raw(p, n); return true; });
    };
    std::function<void(const ConversationCheckpoint&)> ck = [&](const ConversationCheckpoint& c) {
        u64(c.ids.size()); raw(c.ids.data(), c.ids.size() * 4);
        u64(c.imgs.size()); raw(c.imgs.data(), c.imgs.size() * sizeof(ConversationImageKey));
        for (const auto* v : {&c.gdn, &c.ple, &c.tails, &c.dead, &c.block_pos}) { u64(v->size()); raw(v->data(), v->size()); }
        u64(c.used);
        u64(c.stage_parts.size());
        for (const auto& p : c.stage_parts) ck(p);
    };
    const char magic[8] = {'S', 'T', 'R', 'C', 'O', 'N', 'V', '1'};
    const uint32_t version = 1;
    raw(magic, 8); raw(&version, 4); raw(&im.geometry, sizeof im.geometry); raw(&im.layer_lo, 8); raw(&im.layer_hi, 8);
    const uint8_t cvec = im.cvec;
    raw(&cvec, 1);
    ck(im.live);
    u64(im.checkpoints.size());
    for (const auto& c : im.checkpoints) ck(c);
    u64(im.kv.size());
    for (const auto& k : im.kv) {
        raw(&k.format, sizeof k.format);
        for (const int64_t* v : {&k.cells, &k.heads, &k.head_dim, &k.page_size, &k.pooled_rows, &k.idx_dim}) raw(v, 8);
        buf(k.k); buf(k.v); buf(k.k_scale); buf(k.v_scale); buf(k.pooled);
    }
    raw(magic, 8);
    std::fclose(f);
}

std::vector<int32_t> tokens(size_t n, uint64_t seed) {
    std::vector<int32_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (int32_t) (splitmix(seed * 1000003 + i) % 248000);
    return v;
}
std::vector<int32_t> extend(std::vector<int32_t> v, size_t n, uint64_t seed) {
    const auto more = tokens(n, seed);
    v.insert(v.end(), more.begin(), more.end());
    return v;
}

std::string tmpdir() {
    std::string t = (fs::temp_directory_path() / "conversation_disk_test-XXXXXX").string();
    check(::mkdtemp(t.data()) != nullptr, "temporary directory");
    return t;
}
std::string P(const std::string& dir, const SavedConversation& im) { return (fs::path(dir) / conversation_disk_name(im)).string(); }
std::vector<std::string> conv_files(const std::string& dir) {
    std::vector<std::string> out;
    for (const auto& de : fs::directory_iterator(dir))
        if (de.path().filename().string().rfind("conv-", 0) == 0) out.push_back(de.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}
bool reads_as(const std::string& path, const SavedConversation& want) {
    SavedConversation got;
    std::string e;
    if (!conversation_disk_read(got, path, e)) { std::fprintf(stderr, "  read %s: %s\n", path.c_str(), e.c_str()); return false; }
    return same(got, want);
}
bool rejected(const std::string& path, const char* why_part) {
    SavedConversation got;
    got.layer_hi = 12345;   // a refused read leaves the target alone
    std::string e;
    const bool ok = conversation_disk_read(got, path, e);
    if (!ok) std::printf("  rejected as expected: %s\n", e.c_str());
    return !ok && got.layer_hi == 12345 && e.find(why_part) != std::string::npos;
}
void corrupt(const std::string& path, uint64_t at) {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekg((std::streamoff) at);
    char c = 0;
    f.read(&c, 1);
    c ^= 0x40;
    f.seekp((std::streamoff) at);
    f.write(&c, 1);
}

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

int real(const std::string& parent_in, const std::string& child_in, const std::string& dir) {
    // Both images are read whole first, then the parent is written as format 2 and the child as its delta.
    std::string e;
    fs::create_directories(dir);
    SavedConversation parent, child, back;
    double t = now();
    check(conversation_disk_read(parent, parent_in, e), e.c_str());
    std::printf("read parent %s: %llu tokens, %zu checkpoints in %.1f s\n", parent_in.c_str(),
                (unsigned long long) parent.live.ids.size(), parent.checkpoints.size(), now() - t);
    const std::string base = P(dir, parent);
    ConversationDiskWritten w;
    t = now();
    check(conversation_disk_write(parent, base, e, {}, &w), e.c_str());
    std::printf("parent written whole: %.1f MB in %.1f s\n", w.file_bytes / 1e6, now() - t);
    parent = {};
    t = now();
    check(conversation_disk_read(child, child_in, e), e.c_str());
    std::printf("read child %s: %llu tokens, %zu checkpoints in %.1f s\n", child_in.c_str(),
                (unsigned long long) child.live.ids.size(), child.checkpoints.size(), now() - t);
    t = now();
    const ConversationDiskIndex ix = conversation_disk_index(child);
    std::printf("hashing the child: %.2f s for %.1f MB\n", now() - t, ix.image_bytes / 1e6);
    const std::string child_out = P(dir, child);
    t = now();
    check(conversation_disk_write(child, child_out, e, base, &w), e.c_str());
    std::printf("child written %s: %.1f MB (image %.1f MB, reused %.1f MB, %.1f%% of whole) in %.1f s\n",
                w.delta ? "as a delta" : "WHOLE", w.file_bytes / 1e6, w.image_bytes / 1e6, w.reused_bytes / 1e6,
                100.0 * w.file_bytes / w.image_bytes, now() - t);
    t = now();
    check(conversation_disk_read(back, child_out, e), e.c_str());
    std::printf("delta read back with its parent in %.1f s\n", now() - t);
    check(same(back, child), "real child restored from the delta equals the original byte for byte");
    std::printf("OK: byte-for-byte equal\n");
    return 0;
}
// The child cut back to its checkpoint `k` (live = that checkpoint, K/V pages and indexer rows up to it): the image
// a conversation had a few turns earlier, as far as the file format is concerned.
int prefix(const std::string& child_in, size_t k, const std::string& dir) {
    std::string e;
    SavedConversation child;
    check(conversation_disk_read(child, child_in, e), e.c_str());
    check(k < child.checkpoints.size(), "checkpoint index");
    SavedConversation cut;
    cut.geometry = child.geometry; cut.layer_lo = child.layer_lo; cut.layer_hi = child.layer_hi; cut.cvec = child.cvec;
    cut.live = child.checkpoints[k];
    cut.live.used = 0;
    const int64_t n = (int64_t) cut.live.ids.size();
    for (const auto& c : child.checkpoints) if ((int64_t) c.ids.size() < n) cut.checkpoints.push_back(c);
    for (const auto& kv : child.kv) {
        ConversationKv x = kv;
        x.cells = (n + kv.page_size - 1) / kv.page_size * kv.page_size;
        for (auto* b : {&x.k, &x.v, &x.k_scale, &x.v_scale}) b->resize(b->size() / (size_t) kv.cells * (size_t) x.cells);
        if (kv.pooled_rows) {
            x.pooled_rows = n / 4 + 1;
            x.pooled.resize((size_t) x.pooled_rows * (size_t) kv.idx_dim * sizeof(float));
        }
        cut.kv.push_back(std::move(x));
    }
    std::printf("cut %s back to checkpoint %zu: %lld of %zu tokens, %zu checkpoints\n", child_in.c_str(), k,
                (long long) n, child.live.ids.size(), cut.checkpoints.size());
    fs::create_directories(dir);
    const std::string base = (fs::path(dir) / ("cut-" + conversation_disk_name(cut))).string();
    ConversationDiskWritten w;
    double t = now();
    check(conversation_disk_write(cut, base, e, {}, &w), e.c_str());
    std::printf("cut image written whole: %.1f MB in %.1f s\n", w.file_bytes / 1e6, now() - t);
    cut = {};
    const std::string out = P(dir, child);
    t = now();
    check(conversation_disk_write(child, out, e, base, &w), e.c_str());
    std::printf("child written %s: %.1f MB (image %.1f MB, reused %.1f MB, %.1f%% of whole) in %.1f s\n",
                w.delta ? "as a delta" : "WHOLE", w.file_bytes / 1e6, w.image_bytes / 1e6, w.reused_bytes / 1e6,
                100.0 * w.file_bytes / w.image_bytes, now() - t);
    SavedConversation back;
    t = now();
    check(conversation_disk_read(back, out, e), e.c_str());
    std::printf("delta read back with its parent in %.1f s\n", now() - t);
    check(same(back, child), "real child restored from the delta equals the original byte for byte");
    std::printf("OK: byte-for-byte equal\n");
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "real") return real(argv[2], argv[3], argv[4]);
    if (argc == 5 && std::string(argv[1]) == "prefix") return prefix(argv[2], (size_t) std::atoi(argv[3]), argv[4]);
    const std::string dir = tmpdir();
    std::string e, e0;
    // A conversation over four turns: the third rewrites the reply's tail (#342: the client re-renders it).
    const auto ids0 = tokens(10000, 1);
    const auto ids1 = extend(ids0, 700, 2);
    const auto ids2 = extend(ids1, 900, 3);
    auto ids3 = std::vector<int32_t>(ids2.begin(), ids2.end() - 50);
    ids3 = extend(ids3, 1200, 4);
    const SavedConversation a0 = image(ids0, {1000, 9000}, 10);
    const SavedConversation a1 = image(ids1, {1000, 9000, 10000}, 10);
    const SavedConversation a2 = image(ids2, {1000, 9000, 10000, 10700}, 10);
    const SavedConversation a3 = image(ids3, {1000, 9000, 10000, 10700, 11550}, 10);
    const SavedConversation other = image(tokens(6000, 9), {1000}, 3);

    {   // format 1 still reads
        const std::string p = (fs::path(dir) / "legacy.bin").string();
        write_v1(a0, p);
        check(reads_as(p, a0), "format 1 file reads back equal");
        ConversationDiskIndex x;
        check(conversation_disk_read_index(x, p, e) && x.version == 1, "format 1 index reads as legacy");
        ConversationDiskWritten w;
        check(conversation_disk_write(a1, (fs::path(dir) / "child-of-legacy.bin").string(), e, p, &w) && !w.delta,
              "a format 1 file is never a parent: the child is written whole");
        fs::remove(p);
        fs::remove(fs::path(dir) / "child-of-legacy.bin");
    }
    {   // whole format 2
        ConversationDiskWritten w;
        check(conversation_disk_write(a0, P(dir, a0), e, {}, &w) && !w.delta, "whole write");
        check(reads_as(P(dir, a0), a0), "whole format 2 reads back equal");
        ConversationDiskIndex x;
        check(conversation_disk_read_index(x, P(dir, a0), e) && x.version == 2 && !x.delta &&
              x.fingerprint == conversation_disk_index(a0).fingerprint && x.image_bytes == w.image_bytes,
              "the index on disk is the image's index");
        std::printf("whole a0: %.2f MB\n", w.file_bytes / 1e6);
    }
    {   // a chain of three deltas, the last over a rewritten tail
        const SavedConversation* chain[] = {&a0, &a1, &a2, &a3};
        for (int i = 1; i < 4; ++i) {
            ConversationDiskWritten w;
            check(conversation_disk_write(*chain[i], P(dir, *chain[i]), e, P(dir, *chain[i - 1]), &w), e.c_str());
            check(w.delta && w.depth == (uint32_t) i, "written as a delta of the previous turn");
            check(w.file_bytes < w.image_bytes / 4, "a turn's delta is a small part of the image");
            std::printf("delta a%d: %.2f MB of %.2f MB (reused %.2f MB)\n", i, w.file_bytes / 1e6, w.image_bytes / 1e6,
                        w.reused_bytes / 1e6);
        }
        check(reads_as(P(dir, a1), a1), "a1 restored from a0 + delta");
        check(reads_as(P(dir, a2), a2), "a2 restored from a0 + 2 deltas");
        check(reads_as(P(dir, a3), a3), "a3 (rewritten tail) restored from a0 + 3 deltas");
        // the delta equals a whole write of the same image, byte for byte after restore
        const std::string whole = (fs::path(dir) / "whole-a3.bin").string();
        check(conversation_disk_write(a3, whole, e), "whole a3");
        SavedConversation x, y;
        check(conversation_disk_read(x, whole, e) && conversation_disk_read(y, P(dir, a3), e) && same(x, y),
              "delta chain restore equals the whole file's restore");
        fs::remove(whole);
    }
    {   // a slot taken twice from one parent slot (copy, then move)
        SavedConversation twice = a1;
        twice.checkpoints.push_back(twice.checkpoints[0]);
        twice.checkpoints.back().used = 99;
        const std::string p = (fs::path(dir) / "twice.bin").string();
        ConversationDiskWritten w;
        check(conversation_disk_write(twice, p, e, P(dir, a0), &w) && w.delta, "delta with a doubly used parent slot");
        check(reads_as(p, twice), "doubly used parent slot restored twice");
        fs::remove(p);
    }
    {   // broken chains are rejected, never applied
        const std::string a1p = P(dir, a1), a2p = P(dir, a2), a3p = P(dir, a3), save = a1p + ".save";
        fs::copy_file(a1p, save);
        // the parent's name now holds another image
        check(conversation_disk_write(other, a1p, e), "overwrite a1's file with another image");
        check(rejected(a2p, "holds another image"), "a2 rejected: its parent changed");
        check(rejected(a3p, "broken chain"), "a3 rejected: its grandparent changed");
        // the parent is missing
        fs::remove(a1p);
        check(rejected(a2p, "broken chain"), "a2 rejected: its parent is missing");
        fs::rename(save, a1p);
        check(reads_as(a3p, a3), "chain whole again once the parent is back");
        // a flipped byte in a delta's payload or in its index
        const std::string c = a3p + ".corrupt.bin";
        fs::copy_file(a3p, c);
        corrupt(c, fs::file_size(c) / 2);
        check(rejected(c, ""), "a flipped payload byte is rejected");
        fs::remove(c);
        fs::copy_file(a3p, c);
        corrupt(c, fs::file_size(c) - 40);
        check(rejected(c, "corrupt"), "a flipped index byte is rejected");
        fs::remove(c);
        fs::copy_file(a3p, c);
        fs::resize_file(c, fs::file_size(c) - 1000);
        check(rejected(c, ""), "a truncated delta is rejected");
        fs::remove(c);
        // a corrupt parent (payload) makes the child fail its index check
        const std::string a0p = P(dir, a0);
        fs::copy_file(a0p, save);
        corrupt(a0p, 4096);
        check(rejected(a1p, "does not match"), "a corrupted base is caught at the child's index check");
        fs::remove(a0p);
        fs::rename(save, a0p);
        check(reads_as(a1p, a1), "a1 whole again");
    }
    for (const auto& n : conv_files(dir)) fs::remove(fs::path(dir) / n);

    std::vector<std::string> log;
    auto logger = [&](const std::string& m) {
        std::printf("  log: %s\n", m.c_str());
        if (m.find("FAILED") != std::string::npos || m.find("cannot") != std::string::npos) log.push_back(m);
    };
    auto clean = [&] { for (const auto& de : fs::directory_iterator(dir)) fs::remove(de.path()); };
    auto options = [](uint64_t budget = 1ull << 40) {
        ConversationStoreOptions o;
        o.budget_bytes = budget;
        return o;
    };
    auto prompt = [](const std::vector<int32_t>& ids, size_t keep, size_t more) {
        std::vector<int32_t> p(ids.begin(), ids.begin() + (std::ptrdiff_t) keep);
        for (size_t i = 0; i < more; ++i) p.push_back(5);
        return p;
    };
    auto fsize = [&](const SavedConversation& im) { return (uint64_t) fs::file_size(P(dir, im)); };
    auto show = [](const char* what, const ConversationDiskStats& s) {
        std::printf("  %s: %d written (%d deltas, %.2f MB of %.2f MB), %d unchanged, %d removed\n", what, s.written,
                    s.deltas, s.bytes / 1e6, s.image_bytes / 1e6, s.unchanged, s.removed);
        return s;
    };
    clean();
    {   // the catalog: deltas, supersession, matching by the index alone
        ConversationStore st;
        check(st.open(dir, options(), logger), "open");
        auto s = show("a0", st.persist({&a0}));
        check(s.written == 1 && s.deltas == 0, "first file is whole");
        s = show("a1", st.persist({&a1}));
        check(s.written == 1 && s.deltas == 1 && s.removed == 0, "next turn is a delta; its parent stays");
        auto e = st.entries();
        check(e.size() == 1 && e[0].name == conversation_disk_name(a1) && e[0].tokens == (int64_t) ids1.size(),
              "a0 is superseded by a1: one conversation, two files");
        check(conv_files(dir).size() == 2, "a0 kept as a1's parent");
        auto m = st.best(prompt(ids1, ids1.size(), 10), {}, true);
        check(m.tokens == (int64_t) ids1.size() && m.live && m.name == conversation_disk_name(a1) &&
              m.read_bytes == fsize(a0) + fsize(a1), "a continuation of a1 matches its live state; the read is the chain");
        auto c = st.best(prompt(ids1, 9500, 3), {}, true);
        check(c.tokens == 9000 && !c.live, "an edit after token 9500 matches the 9000 checkpoint");
        check(st.best(prompt(ids1, ids1.size(), 10), {}, false).tokens == 0, "the steering mode must match");
        check(st.best(prompt(ids1, ids1.size(), 10), {{5, 42}}, true).tokens == 0, "a picture in the prefix must match");
        check(st.best(prompt(ids1, 900, 10), {}, true).tokens == 0, "nothing shorter than the first checkpoint");
        check(st.worth(m, 0), "10700 tokens to read again (~6 s) beat reading ~40 MB");
        check(!st.worth(m, m.tokens - 100), "a 100-token gain is not worth a read");
        check(!st.worth(c, 0) || c.read_bytes > 0, "worth weighs the chain's bytes");
        SavedConversation back;
        check(st.take(m, back, e0) && same(back, a1), "a disk hit restores a1 byte for byte");
        check(st.stats().hits == 1, "counted as a disk hit");
    }
    {   // a new process: the catalog comes back from the manifest and the indexes
        ConversationStore st;
        check(st.open(dir, options(), logger), "reopen");
        check(st.entries().size() == 1 && st.recent(2) == std::vector<std::string>{conversation_disk_name(a1)},
              "reopened: the same conversation, a0 still a parent only");
        check(st.best(prompt(ids1, ids1.size(), 1), {}, true).tokens == (int64_t) ids1.size(), "and it matches");
    }
    clean();
    {   // a format 1 file is matched from its token lists, then superseded by a whole format 2 file
        write_v1(a0, P(dir, a0));
        ConversationStore st;
        check(st.open(dir, options(), logger), "open with a format 1 file");
        auto m = st.best(prompt(ids0, ids0.size(), 4), {}, true);
        check(m.tokens == (int64_t) ids0.size() && m.live, "format 1 live state matched without reading its states");
        SavedConversation back;
        check(st.take(m, back, e0) && same(back, a0), "format 1 restore");
        auto s = show("a1 over format 1", st.persist({&a1}));
        check(s.written == 1 && s.deltas == 0 && s.removed == 1 && conv_files(dir).size() == 1,
              "a1 is whole (format 1 is never a parent); a0 superseded and removed");
    }
    clean();
    {   // spill: queued, matched and taken back before its write
        ConversationStore st;
        check(st.open(dir, options(), logger), "open");
        st.hold_writes(true);
        SavedConversation c = a1;
        const uint64_t bytes = c.bytes();
        st.spill(std::move(c));
        check(st.pending_bytes() == bytes && st.stats().pending == 1, "queued bytes counted");
        auto m = st.best(prompt(ids1, ids1.size(), 5), {}, true);
        check(m.pending != 0 && m.tokens == (int64_t) ids1.size() && st.worth(m, 0), "a queued image matches, no read");
        SavedConversation back;
        check(st.take(m, back, e0) && same(back, a1), "taken back whole");
        check(st.pending_bytes() == 0 && conv_files(dir).empty(), "nothing written, nothing pending");
        st.hold_writes(false);
        // written between best() and take(): take refuses, best() then finds the file
        c = a1;
        st.hold_writes(true);
        st.spill(std::move(c));
        m = st.best(prompt(ids1, ids1.size(), 5), {}, true);
        st.hold_writes(false);
        st.flush();
        check(!st.take(m, back, e0), "a queued match written meanwhile is refused");
        m = st.best(prompt(ids1, ids1.size(), 5), {}, true);
        check(m.pending == 0 && m.name == conversation_disk_name(a1) && st.take(m, back, e0) && same(back, a1),
              "and found on disk");
        // the same image spilled again is not written again
        c = a1;
        st.spill(std::move(c));
        st.flush();
        check(st.stats().spill_unchanged == 1, "a spill of a conversation already on disk writes nothing");
    }
    clean();
    {   // queued bytes are bounded: the second spill waits for the first write
        ConversationStoreOptions o = options();
        o.pending_bytes = a1.bytes() + 1;
        ConversationStore st;
        check(st.open(dir, o, logger), "open");
        st.hold_writes(true);
        SavedConversation c1 = a1, c2 = a2;
        st.spill(std::move(c1));   // one image is always admitted
        std::atomic<bool> done{false};
        std::thread t([&] { st.spill(std::move(c2)); done = true; });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        check(!done.load(), "a spill past the pending bound blocks");
        st.hold_writes(false);
        t.join();
        st.flush();
        const auto s = st.stats();
        check(done.load() && s.spills == 2 && s.spill_wait_seconds > 0.2 && s.pending_bytes == 0,
              "it went on once the first was written");
        check(st.entries().size() == 1 && conv_files(dir).size() == 2, "a2 (a delta of a1) supersedes a1, which stays its parent");
        std::printf("  spills: %llu, %.2f MB written for %.2f MB of images in %.2f s (waited %.2f s)\n",
                    (unsigned long long) s.spills, s.spill_bytes / 1e6, s.spill_image_bytes / 1e6, s.spill_seconds,
                    s.spill_wait_seconds);
    }
    clean();
    {   // the byte budget: least recently used conversations go, never a parent a kept delta needs
        uint64_t other_b, a0_b, a1_b;
        {
            ConversationStore st;
            check(st.open(dir, options(), logger), "open");
            st.persist({&other});
            st.persist({&a0});
            st.persist({&a1});
            other_b = fsize(other); a0_b = fsize(a0); a1_b = fsize(a1);
        }
        {
            ConversationStore st;
            check(st.open(dir, options(a0_b + a1_b + other_b / 2), logger), "reopen under a smaller budget");
            check(conv_files(dir).size() == 2 && !fs::exists(P(dir, other)) && fs::exists(P(dir, a0)),
                  "the least recently used conversation went; a1's parent stayed");
            check(st.stats().evicted == 1, "one budget eviction");
        }
        {
            ConversationStore st;
            check(st.open(dir, options(a1_b), logger), "a budget below the last conversation's chain");
            check(conv_files(dir).size() == 2 && st.entries().size() == 1, "the last conversation is never dropped");
            const SavedConversation b = image(tokens(5000, 77), {1000}, 1);
            auto s = show("b", st.persist({&b}));
            check(s.written == 1 && s.removed == 2 && conv_files(dir).size() == 1 && fs::exists(P(dir, b)),
                  "a newer one replaces it: the delta and its parent go together");
        }
    }
    clean();
    {   // a broken chain found at start: dropped, never offered
        {
            ConversationStore st;
            check(st.open(dir, options(), logger), "open");
            st.persist({&a0});
            st.persist({&a1});
        }
        fs::remove(P(dir, a0));
        ConversationStore st;
        check(st.open(dir, options(), logger), "reopen without a1's parent");
        check(st.entries().empty() && conv_files(dir).empty() &&
              st.best(prompt(ids1, ids1.size(), 1), {}, true).tokens == 0, "a1 dropped and removed");
    }
    clean();
    {   // compaction by depth: the chain restarts whole after max_depth deltas
        std::vector<SavedConversation> turns;
        std::vector<int32_t> ids = ids0;
        std::vector<size_t> cks = {1000};
        for (int i = 0; i < 11; ++i) {
            turns.push_back(image(ids, cks, 5));
            cks.push_back(ids.size());
            ids = extend(ids, 150, 100 + i);
        }
        ConversationStoreOptions o = options();
        o.policy.max_depth = 4;
        o.policy.max_chain_fraction = 10;
        ConversationStore st;
        check(st.open(dir, o, logger), "open");
        int deltas = 0, wholes = 0;
        for (auto& t : turns) {
            auto s = st.persist({&t});
            deltas += s.deltas;
            wholes += s.written - s.deltas;
            ConversationDiskIndex x;
            check(conversation_disk_read_index(x, P(dir, t), e0) && x.depth <= 4, "depth bounded by the policy");
            check(reads_as(P(dir, t), t), "every turn restores");
        }
        check(wholes == 3 && deltas == 8, "11 turns at max_depth 4: whole, 4 deltas, whole, 4 deltas, whole");
        check(conv_files(dir).size() == 1 && st.entries().size() == 1, "after compaction the old chain is removed");
    }
    clean();
    {   // compaction by size
        ConversationStoreOptions o = options();
        o.policy.max_chain_fraction = 0.15;
        ConversationStore st;
        check(st.open(dir, o, logger), "open");
        st.persist({&a0});
        check(st.persist({&a1}).deltas == 1, "a small turn fits 15%");
        auto s = st.persist({&a2});
        check(s.deltas == 0 && s.written == 1 && conv_files(dir).size() == 1, "past 15% of the base: whole, old chain gone");
    }
    clean();
    {   // the state codec: lossless, and it takes something off
        SavedConversation small = image(tokens(64, 5), {16, 32, 48}, 1);
        ConversationDiskWritten w;
        check(conversation_disk_write(small, P(dir, small), e0, {}, &w), "write state-heavy image");
        std::printf("state-heavy image: %.2f MB on disk for %.2f MB\n", w.file_bytes / 1e6, w.image_bytes / 1e6);
#if defined(STRATA_HAVE_ZSTD)
        check(w.file_bytes < w.image_bytes * 0.97, "checkpoint state is stored compressed");
#endif
        check(reads_as(P(dir, small), small), "compressed state restores byte for byte");
        const std::string c = P(dir, small) + ".corrupt.bin";
        fs::copy_file(P(dir, small), c);
        corrupt(c, 400000);   // inside the first checkpoint's packed GDN
        check(rejected(c, ""), "a flipped byte in packed state is rejected");
        clean();
    }
    {   // checkpoint thinning: the positions of a real file (conv-3593...-70768)
        SavedConversation t;
        for (size_t n : {69444, 1414, 67547, 67791, 68172, 68901}) {   // not in length order, as LRU leaves them
            ConversationCheckpoint c;
            c.ids.resize(n);
            t.checkpoints.push_back(c);
        }
        check(conversation_disk_keep_checkpoints(t, 0).size() == 6, "gap 0 keeps every checkpoint");
        check(conversation_disk_keep_checkpoints(t, 4096) == std::vector<size_t>{0, 1, 2, 5},
              "gap 4096: root, 67547 and the two deepest");
        check(conversation_disk_keep_checkpoints(t, 100000) == std::vector<size_t>{0, 1, 5},
              "a gap past everything keeps the root and the two deepest");
        const auto ids = tokens(12000, 21);
        const SavedConversation full = image(ids, {1000, 9000, 9300, 9600, 11000, 11500}, 1);
        ConversationStoreOptions o = options();
        o.policy.checkpoint_gap = 1024;
        ConversationStore st;
        check(st.open(dir, o, logger), "open");
        auto s = st.persist({&full});
        check(s.written == 1 && s.dropped_checkpoints == 2, "two middle checkpoints dropped");
        SavedConversation back, want = full;
        want.checkpoints = {full.checkpoints[0], full.checkpoints[1], full.checkpoints[4], full.checkpoints[5]};
        check(st.load(st.recent(1).at(0), back, e0) && same(back, want), "thinned image restores");
        check(st.persist({&back}).unchanged == 1, "the reloaded thinned image is the same file");
        check(st.persist({&full}).unchanged == 1, "the in-RAM image maps to that file too");
    }
    check(log.empty(), "no write errors");
    fs::remove_all(dir);
    std::printf("conversation_disk_test: %d checks passed\n", checks);
    return 0;
}
