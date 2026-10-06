// Disk persistence for --serve's parked conversations (--conversation-dir).
// The file is a plain dump of a SavedConversation; restore still runs the
// usual conversation_snapshot_validate, so a file from another model or
// geometry is rejected there, never applied.
//
// Format 2 stores the checkpoints' GDN/PLE state byte-shuffled and zstd-compressed (lossless) when built with zstd.
// Format 2 adds deltas: a file may name a parent file (same directory) and hold only what differs from it, the
// K/V bytes past the longest run of 64 KiB chunks the two share and the checkpoints the parent does not hold.
// Every format 2 file ends with an index (chunk and checkpoint hashes, the chain's sizes) that a writer reads to
// pick a parent without loading the image, and that a reader checks the restored image against: a delta whose
// parent changed or went missing is rejected, never applied.  Format 1 (whole image, no index) is still read.
#pragma once

#include "strata/core/conversation_cache.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace strata::core {

// Identity of a conversation image: its live tokens, images, checkpoint ends and steering mode.
uint64_t conversation_disk_key(const SavedConversation& image);
// conv-<key>-<live tokens>.bin
std::string conversation_disk_name(const SavedConversation& image);

// What a format 2 file says about the image it restores to, read without loading it.
struct ConversationDiskIndex {
    static constexpr size_t chunk_bytes = 64 * 1024;
    uint32_t version = 0;                 // 1: legacy whole image without an index (only the slots' tokens are set)
    bool delta = false;
    std::string parent;                   // the parent's file name, in the same directory (delta only)
    uint64_t parent_fingerprint = 0;
    uint32_t depth = 0;                   // deltas between this file and its whole base (0: a whole image)
    uint64_t payload_bytes = 0;           // this file's bytes before the index
    uint64_t base_bytes = 0;              // the whole base file's payload bytes
    uint64_t chain_bytes = 0;             // the deltas' payload bytes from the base up to and including this one
    uint64_t image_bytes = 0;             // the restored image's payload bytes (RAM admission)
    uint64_t tokens = 0;
    // The image description; the fingerprint hashes it (every byte of the image, through the hashes).
    std::array<int64_t, 18> geometry{};
    int64_t layer_lo = 0, layer_hi = 0;
    bool cvec = true;
    struct Slot {
        uint64_t hash = 0, bytes = 0, used = 0;   // checkpoint payload hash without `used`
        uint64_t ids_len = 0, ids_hash = 0;       // its tokens, for matching a prompt without reading the file
        std::vector<ConversationImageKey> imgs;
    };
    std::vector<Slot> slots;              // the live checkpoint first, then the checkpoints
    struct Buffer { uint64_t size = 0; std::vector<uint64_t> chunks; };
    struct Layer {
        int32_t format = 0;
        int64_t cells = 0, heads = 0, head_dim = 0, page_size = 0, pooled_rows = 0, idx_dim = 0;
        std::array<Buffer, 5> buf;        // k, v, k_scale, v_scale, pooled
    };
    std::vector<Layer> kv;
    uint64_t fingerprint = 0;
};

// Hashes the image (single pass over its bytes).  version 2, whole, no chain.
ConversationDiskIndex conversation_disk_index(const SavedConversation& image);
// Reads only the header/index.  A format 1 file reads as version 1 with image_bytes = its size.
bool conversation_disk_read_index(ConversationDiskIndex& index, const std::string& path, std::string& error);

struct ConversationDiskWritten {
    bool delta = false;
    uint32_t depth = 0;
    uint64_t file_bytes = 0, reused_bytes = 0, image_bytes = 0;
};
// Writes `path` atomically (temporary file, fsync, rename).  With `parent` (a format 2 file in the same directory)
// the file is a delta against it when they share anything; otherwise a whole image.
bool conversation_disk_write(const SavedConversation& image, const std::string& path, std::string& error,
                             const std::string& parent = {}, ConversationDiskWritten* written = nullptr);
// Reads a whole image or a delta with its parent chain; a delta is accepted only when the restored image matches
// its index byte for byte (through the hashes).
bool conversation_disk_read(SavedConversation& image, const std::string& path, std::string& error);

struct ConversationDiskPolicy {
    uint32_t max_depth = 8;               // a longer chain is written whole instead
    double max_chain_fraction = 0.5;      // ... as is one whose deltas would exceed this share of the base
    // Checkpoints written (0: all).  Otherwise the root, the two deepest, and any other whose loss would make a
    // miss there read more than `checkpoint_gap` extra tokens (conversation_disk_keep_checkpoints).
    uint64_t checkpoint_gap = 0;
};
struct ConversationDiskStats {
    int written = 0, deltas = 0, unchanged = 0, removed = 0, failed = 0, dropped_checkpoints = 0;
    uint64_t bytes = 0;                   // written to disk
    uint64_t image_bytes = 0;             // what writing every written image whole would have cost
};
// The image's checkpoints (indices, in its order) a disk image keeps under ConversationDiskPolicy::checkpoint_gap.
std::vector<size_t> conversation_disk_keep_checkpoints(const SavedConversation& image, uint64_t gap);
// ---- the disk tier of --serve's conversation cache (--conversation-dir)
//
// A directory of conversation files the cache spills evicted conversations into and restores them from.  Its
// catalog is built from each file's index (format 2) or its token lists (format 1), never by reading images, and
// matches a prompt by the hash of its leading tokens.  A file is a conversation (listed in conversations.manifest,
// or not a parent of another file) or kept only as a held delta's parent.  A new file supersedes (#342) the
// conversations whose deepest checkpoint it holds.  Files past the byte budget go least recently used first,
// with every parent a kept delta needs.  Writes run on one background thread; the images waiting for it are
// bounded by `pending_bytes` and can be handed back before (or after) they are written.
struct ConversationStoreOptions {
    ConversationDiskPolicy policy;
    uint64_t budget_bytes = 100ull << 30;  // the directory's conv-* files
    uint64_t pending_bytes = 4ull << 30;   // spilled images not written yet (one is always admitted)
    double read_mb_s = 100;                // a disk restore is taken when re-reading the tokens it adds would
    double prefill_tok_s = 1800;           //   take longer than reading its files
    int64_t min_gain = 4096;               //   ... and it adds at least this many tokens
    bool background = true;                // false: spill() writes before it returns
};
struct ConversationStoreMatch {
    int64_t tokens = 0;
    bool live = false;
    std::string name;                      // the file, or empty for a spilled image not written yet
    uint64_t pending = 0;                  // that image's id
    uint64_t read_bytes = 0, image_bytes = 0;
};
struct ConversationStoreStats {
    uint64_t spills = 0, spill_written = 0, spill_unchanged = 0, spill_bytes = 0, spill_image_bytes = 0;
    double spill_seconds = 0, spill_wait_seconds = 0;
    uint64_t hits = 0, hit_tokens = 0, read_bytes = 0, reclaimed = 0, evicted = 0, removed = 0;
    double read_seconds = 0;
    uint64_t conversations = 0, files = 0, disk_bytes = 0, pending = 0, pending_bytes = 0;
};
struct ConversationStoreEntry {
    std::string name;
    uint64_t key = 0;
    int64_t tokens = 0;
    uint64_t bytes = 0;                    // its file (a delta's own)
};

class ConversationStore {
public:
    using Log = std::function<void(const std::string&)>;
    ConversationStore();
    ~ConversationStore();                  // finishes the queued writes
    ConversationStore(const ConversationStore&) = delete;
    ConversationStore& operator=(const ConversationStore&) = delete;

    bool open(const std::string& dir, const ConversationStoreOptions& options, Log log);
    bool enabled() const;
    // An evicted conversation: queued for the writer.  Blocks while the queued images would pass pending_bytes.
    void spill(SavedConversation&& image);
    // QUIT/PERSIST: after the queue, writes each image not on disk yet.
    ConversationDiskStats persist(const std::vector<const SavedConversation*>& images);
    void flush();                          // until every queued write is done
    ConversationStoreMatch best(const std::vector<int32_t>& prompt, const std::vector<ConversationImageKey>& images,
                                bool cvec) const;
    // whether restoring `m` beats reading again what it adds to the `have` tokens a request has already
    bool worth(const ConversationStoreMatch& m, int64_t have) const;
    // The image of a match: a queued one is taken back (after its write, if that has begun), a file is read.
    bool take(const ConversationStoreMatch& m, SavedConversation& image, std::string& error);
    bool load(const std::string& name, SavedConversation& image, std::string& error);
    std::vector<std::string> recent(size_t n) const;   // conversations, most recently used first
    std::vector<ConversationStoreEntry> entries() const;   // conversations, least recently used first
    ConversationStoreStats stats() const;
    uint64_t pending_bytes() const;
    void hold_writes(bool hold);           // tests: the writer starts nothing while held

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::core
