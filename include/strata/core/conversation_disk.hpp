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
    uint32_t version = 0;                 // 1: legacy whole image without an index (no other field is set)
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
    struct Slot { uint64_t hash = 0, bytes = 0, used = 0; };   // checkpoint payload hash without `used`
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
// --conversation-dir's QUIT/PERSIST: `images` least recently active first.  Writes each image not already on disk
// (a delta where a held or kept file shares enough with it), then the manifest of held files, then removes the
// conv-* files that are neither held nor an ancestor of a held one.  `log` gets one line per problem.
ConversationDiskStats conversation_disk_persist(const std::string& dir, const std::vector<const SavedConversation*>& images,
                                                const ConversationDiskPolicy& policy,
                                                const std::function<void(const std::string&)>& log);
// The .bin files load_all should load, oldest first: all but those kept only as a held delta's ancestor.
std::vector<std::string> conversation_disk_loadable(const std::string& dir);

} // namespace strata::core
