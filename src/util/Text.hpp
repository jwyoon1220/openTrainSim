#pragma once
// Text helpers for legacy openBVE content: encoding detection (UTF-8 / UTF-16 BOM / legacy
// code pages such as CP949 or Shift-JIS), lenient VB6-style number parsing and ASCII
// case-insensitive comparisons.
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ots {

// Reads a whole text file and returns it as UTF-8. Files that are not valid UTF-8 are decoded
// with `fallbackCodepage` (default CP949: Korean content). Returns false if unreadable.
bool readTextFile(const std::filesystem::path& path, std::string& out, const char* fallbackCodepage = nullptr);
void setDefaultCodepage(std::string codepage);
// Converts a byte string from `codepage` to UTF-8 (invalid sequences become '?').
std::string toUtf8(std::string_view bytes, const char* codepage);
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
bool parseDouble(std::string_view s, double& out);
// openBVE "Vb6" parsing: the longest prefix that is a number (e.g. "12m" → 12).
bool parseDoubleVb6(std::string_view s, double& out);
bool parseIntVb6(std::string_view s, int& out);
// Track positions with unit factors: "a:b:c" = a*f0 + b*f1 + c*f2 (factors of the route).
bool parseLength(std::string_view s, const std::vector<double>& unitFactors, double& out);

// UTF-8 → ASCII-only rendering (non-ASCII code points dropped); for the built-in debug font.
std::string asciiOnly(std::string_view utf8);

} // namespace ots
