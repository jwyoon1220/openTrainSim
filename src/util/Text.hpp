#pragma once
// Text helpers for legacy openBVE content: encoding detection (UTF-8 / UTF-16 BOM / legacy
// code pages such as CP949 or Shift-JIS), lenient VB6-style number parsing and ASCII
// case-insensitive comparisons.
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ots {

// Reads a whole text file as UTF-8. Files that are not valid UTF-8 are decoded with
// `fallbackCodepage` (empty: the default, CP949 for Korean content). nullopt if unreadable.
std::optional<std::string> readTextFile(const std::filesystem::path& path, std::string_view fallbackCodepage = {});
void setDefaultCodepage(std::string codepage);
// Converts a byte string from `codepage` to UTF-8 (invalid sequences become '?').
std::string toUtf8(std::string_view bytes, std::string_view codepage);
bool isValidUtf8(std::string_view s);

std::vector<std::string> splitLines(std::string_view text);
std::vector<std::string_view> split(std::string_view s, char sep);
std::string_view trim(std::string_view s);
std::string lower(std::string_view s); // ASCII
bool iequals(std::string_view a, std::string_view b);
bool istartsWith(std::string_view s, std::string_view prefix);
bool iendsWith(std::string_view s, std::string_view suffix);
std::size_t ifind(std::string_view s, std::string_view needle, std::size_t from = 0);

// Strict: the whole (trimmed) string must be a number.
std::optional<double> parseDouble(std::string_view s);
// openBVE "Vb6" parsing: the longest prefix that is a number (e.g. "12m" → 12).
std::optional<double> parseDoubleVb6(std::string_view s);
std::optional<int> parseIntVb6(std::string_view s);
// Track positions with unit factors: "a:b:c" = a*f0 + b*f1 + c*f2 (factors of the route).
std::optional<double> parseLength(std::string_view s, std::span<const double> unitFactors);

// UTF-8 → ASCII-only rendering (non-ASCII code points dropped); for the built-in debug font.
std::string asciiOnly(std::string_view utf8);

} // namespace ots
