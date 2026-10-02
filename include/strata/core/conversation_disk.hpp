// Disk persistence for --serve's parked conversations (--conversation-dir).
// The file is a plain dump of a SavedConversation; restore still runs the
// usual conversation_snapshot_validate, so a file from another model or
// geometry is rejected there, never applied.
#pragma once

#include "strata/core/conversation_cache.hpp"

#include <cstdint>
#include <string>

namespace strata::core {

// Identity of a conversation image: its live tokens, images, checkpoint ends and steering mode.
uint64_t conversation_disk_key(const SavedConversation& image);
// Writes `path` atomically (temporary file, fsync, rename).
bool conversation_disk_write(const SavedConversation& image, const std::string& path, std::string& error);
bool conversation_disk_read(SavedConversation& image, const std::string& path, std::string& error);

} // namespace strata::core
