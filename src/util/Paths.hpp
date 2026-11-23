#pragma once
// Windows-authored content on a case-sensitive file system: resolves "Folder\File.CSV"
// against the real directory entries, ignoring case and accepting both separators.
// Directory listings are cached; thread-safe.
#include <filesystem>
#include <string_view>

namespace ots {

namespace fs = std::filesystem;

// Returns the existing path (exact case) or an empty path if nothing matches.
fs::path resolvePath(const fs::path& base, std::string_view relative);
// Same, but returns the naive concatenation when nothing matches (for messages).
fs::path combinePath(const fs::path& base, std::string_view relative);
bool fileExists(const fs::path& p);
// Walks up from `start` looking for a directory that contains a subdirectory named `name`
// (case-insensitive); returns that subdirectory or empty.
fs::path findUpwards(const fs::path& start, std::string_view name);
std::string pathString(const fs::path& p); // UTF-8 for logs

} // namespace ots
