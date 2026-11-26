#include "util/Text.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <iconv.h>
#endif

#include <array>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>

namespace ots {
namespace {
std::string gDefaultCodepage = "CP949";
std::mutex gCodepageMutex;

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }
char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
} // namespace

void setDefaultCodepage(std::string codepage) {
    std::lock_guard lock(gCodepageMutex);
    gDefaultCodepage = std::move(codepage);
}

bool isValidUtf8(std::string_view s) {
    std::size_t i = 0;
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    while (i < s.size()) {
        const unsigned char c = p[i];
        int n;
        if (c < 0x80) n = 0;
        else if ((c & 0xE0) == 0xC0 && c >= 0xC2) n = 1;
        else if ((c & 0xF0) == 0xE0) n = 2;
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4) n = 3;
        else return false;
        for (int k = 1; k <= n; ++k) {
            if (i + std::size_t(k) >= s.size() || (p[i + std::size_t(k)] & 0xC0) != 0x80) return false;
        }
        i += std::size_t(n) + 1;
    }
    return true;
}

#ifdef _WIN32
namespace {
// iconv-style code page names (as written in the config) to Windows code page numbers.
unsigned windowsCodepage(std::string_view name) {
    std::string n;
    for (char c : name)
        if (c != '-' && c != '_' && c != ' ') n += c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c;
    struct Named { std::string_view name; unsigned cp; };
    static constexpr auto table = std::to_array<Named>({
        {"UTF8", 65001},     {"SHIFTJIS", 932},  {"SJIS", 932},     {"MS932", 932},       {"EUCKR", 949},
        {"UHC", 949},        {"MS949", 949},     {"GBK", 936},      {"GB2312", 936},      {"GB18030", 54936},
        {"BIG5", 950},       {"EUCJP", 20932},   {"LATIN1", 28591}, {"ISO88591", 28591},  {"ASCII", 20127},
        {"KOI8R", 20866},    {"UTF16LE", 1200},  {"UTF16BE", 1201},
    });
    for (const Named& t : table)
        if (n == t.name) return t.cp;
    for (std::string_view prefix : {"WINDOWS", "CP", "IBM"}) {
        if (!n.starts_with(prefix)) continue;
        unsigned cp = 0;
        const char* b = n.data() + prefix.size();
        const auto r = std::from_chars(b, n.data() + n.size(), cp);
        if (r.ec == std::errc() && r.ptr == n.data() + n.size()) return cp;
    }
    return 0;
}
} // namespace

std::string toUtf8(std::string_view bytes, std::string_view codepage) {
    const unsigned cp = windowsCodepage(codepage);
    if (cp == 0 || cp == 65001 || bytes.empty() || bytes.size() > 0x7FFFFFFF) return std::string(bytes);
    const int n = MultiByteToWideChar(cp, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    if (n <= 0) return std::string(bytes);
    std::wstring w(std::size_t(n), L'\0');
    MultiByteToWideChar(cp, 0, bytes.data(), int(bytes.size()), w.data(), n);
    const int m = WideCharToMultiByte(CP_UTF8, 0, w.data(), n, nullptr, 0, nullptr, nullptr);
    std::string out(std::size_t(std::max(m, 0)), '\0');
    if (m > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), n, out.data(), m, nullptr, nullptr);
    return out;
}
#else
std::string toUtf8(std::string_view bytes, std::string_view codepage) {
    iconv_t cd = iconv_open("UTF-8", std::string(codepage).c_str());
    if (cd == iconv_t(-1)) return std::string(bytes);
    std::string out;
    out.resize(bytes.size() * 3 + 16);
    char* in = const_cast<char*>(bytes.data());
    std::size_t inLeft = bytes.size();
    char* o = out.data();
    std::size_t oLeft = out.size();
    while (inLeft > 0) {
        const std::size_t r = iconv(cd, &in, &inLeft, &o, &oLeft);
        if (r != std::size_t(-1)) break;
        if (errno == E2BIG) {
            const std::size_t used = std::size_t(o - out.data());
            out.resize(out.size() * 2);
            o = out.data() + used;
            oLeft = out.size() - used;
        } else { // EILSEQ / EINVAL: skip one byte
            if (oLeft < 1) {
                const std::size_t used = std::size_t(o - out.data());
                out.resize(out.size() * 2);
                o = out.data() + used;
                oLeft = out.size() - used;
            }
            *o++ = '?';
            --oLeft;
            ++in;
            --inLeft;
        }
    }
    out.resize(std::size_t(o - out.data()));
    iconv_close(cd);
    return out;
}
#endif

std::optional<std::string> readTextFile(const std::filesystem::path& path, std::string_view fallbackCodepage) {
    // One sized read instead of a character-at-a-time stream iterator.
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return std::nullopt;
    const std::streamoff size = f.tellg();
    if (size < 0) return std::nullopt;
    std::string bytes(std::size_t(size), '\0');
    f.seekg(0);
    if (size && !f.read(bytes.data(), size)) return std::nullopt;
    const std::string_view v(bytes);
    if (v.starts_with("\xEF\xBB\xBF")) {
        bytes.erase(0, 3);
        return bytes;
    }
    if (v.starts_with("\xFF\xFE")) return toUtf8(v.substr(2), "UTF-16LE");
    if (v.starts_with("\xFE\xFF")) return toUtf8(v.substr(2), "UTF-16BE");
    if (isValidUtf8(bytes)) return bytes;
    if (!fallbackCodepage.empty()) return toUtf8(bytes, fallbackCodepage);
    std::string codepage;
    {
        std::lock_guard lock(gCodepageMutex);
        codepage = gDefaultCodepage;
    }
    return toUtf8(bytes, codepage);
}

std::vector<std::string> splitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n' || text[i] == '\r') {
            lines.emplace_back(text.substr(start, i - start));
            if (i < text.size() && text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            start = i + 1;
        }
    }
    if (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == sep) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = lowerAscii(c);
    return r;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
    return true;
}

bool istartsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && iequals(s.substr(0, p.size()), p); }
bool iendsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && iequals(s.substr(s.size() - p.size()), p); }

std::size_t ifind(std::string_view s, std::string_view needle, std::size_t from) {
    if (needle.size() > s.size()) return std::string_view::npos;
    for (std::size_t i = from; i + needle.size() <= s.size(); ++i)
        if (iequals(s.substr(i, needle.size()), needle)) return i;
    return std::string_view::npos;
}

namespace {
// A decimal number occupying exactly `s` (no spaces), allowing a leading '+' and a leading
// '.' (".5"). Uses from_chars (locale independent).
std::optional<double> parseExact(std::string_view s) {
    if (s.starts_with('+')) s.remove_prefix(1);
    if (s.empty()) return std::nullopt;
    // from_chars rejects ".5" / "-.5" in some libstdc++ versions: normalise.
    std::string normalised;
    if (s.front() == '.' || s.starts_with("-.")) {
        normalised = s.front() == '-' ? "-0" + std::string(s.substr(1)) : "0" + std::string(s);
        s = normalised;
    }
    double v = 0.0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v, std::chars_format::general);
    if (ec != std::errc() || end != s.data() + s.size() || !std::isfinite(v)) return std::nullopt;
    return v;
}
} // namespace

std::optional<double> parseDouble(std::string_view s) { return parseExact(trim(s)); }

std::optional<double> parseDoubleVb6(std::string_view s) {
    // VB6 Val(): embedded whitespace is ignored, then the longest numeric prefix counts
    std::string compact;
    std::ranges::copy_if(trim(s), std::back_inserter(compact), [](char c) { return !isSpace(c); });
    for (std::size_t n = compact.size(); n > 0; --n)
        if (const auto v = parseExact(std::string_view(compact).substr(0, n))) return v;
    return std::nullopt;
}

std::optional<int> parseIntVb6(std::string_view s) {
    const auto d = parseDoubleVb6(s);
    if (!d || *d < -2147483648.0 || *d > 2147483647.0) return std::nullopt;
    return int(std::round(*d));
}

std::optional<double> parseLength(std::string_view s, std::span<const double> unitFactors) {
    s = trim(s);
    if (s.find(':') == std::string_view::npos) {
        const auto v = parseExact(s);
        if (!v) return std::nullopt;
        return *v * (unitFactors.empty() ? 1.0 : unitFactors.back());
    }
    // "a:b:c": the last part uses the last factor, the one before the factor before it ...
    const auto parts = split(s, ':');
    double total = 0.0;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::string_view p = trim(parts[i]);
        if (p.empty()) continue;
        const auto v = parseExact(p);
        if (!v) return std::nullopt;
        total += *v * (unitFactors.size() >= parts.size() ? unitFactors[unitFactors.size() - parts.size() + i] : 1.0);
    }
    return total;
}

std::string asciiOnly(std::string_view utf8) {
    std::string r;
    for (char c : utf8)
        if (static_cast<unsigned char>(c) < 0x80) r.push_back(c);
    return r;
}

} // namespace ots
