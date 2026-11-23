#include "util/Text.hpp"

#include <iconv.h>

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

std::string toUtf8(std::string_view bytes, const char* codepage) {
    iconv_t cd = iconv_open("UTF-8", codepage);
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

bool readTextFile(const std::filesystem::path& path, std::string& out, const char* fallbackCodepage) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto* b = reinterpret_cast<const unsigned char*>(bytes.data());
    if (bytes.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
        out = bytes.substr(3);
    } else if (bytes.size() >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
        out = toUtf8(std::string_view(bytes).substr(2), "UTF-16LE");
    } else if (bytes.size() >= 2 && b[0] == 0xFE && b[1] == 0xFF) {
        out = toUtf8(std::string_view(bytes).substr(2), "UTF-16BE");
    } else if (isValidUtf8(bytes)) {
        out = std::move(bytes);
    } else {
        std::string cp;
        if (fallbackCodepage) cp = fallbackCodepage;
        else {
            std::lock_guard lock(gCodepageMutex);
            cp = gDefaultCodepage;
        }
        out = toUtf8(bytes, cp.c_str());
    }
    return true;
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
// Parses a decimal number occupying exactly `s` (no spaces), allowing a leading '+' and a
// leading '.' (".5"). Uses from_chars (locale independent).
bool parseExact(std::string_view s, double& out) {
    if (s.empty()) return false;
    std::string tmp;
    if (s.front() == '+') s.remove_prefix(1);
    if (s.empty()) return false;
    // from_chars rejects ".5" / "-.5" in some libstdc++ versions: normalise.
    if (s.front() == '.' || (s.size() > 1 && s[0] == '-' && s[1] == '.')) {
        tmp = s.front() == '-' ? "-0" + std::string(s.substr(1)) : "0" + std::string(s);
        s = tmp;
    }
    const char* end = s.data() + s.size();
    auto [p, ec] = std::from_chars(s.data(), end, out, std::chars_format::general);
    return ec == std::errc() && p == end && std::isfinite(out);
}
} // namespace

bool parseDouble(std::string_view s, double& out) {
    s = trim(s);
    return parseExact(s, out);
}

bool parseDoubleVb6(std::string_view s, double& out) {
    s = trim(s);
    // remove embedded whitespace (VB6 Val() semantics)
    std::string compact;
    compact.reserve(s.size());
    for (char c : s)
        if (!isSpace(c)) compact.push_back(c);
    for (std::size_t n = compact.size(); n > 0; --n)
        if (parseExact(std::string_view(compact).substr(0, n), out)) return true;
    return false;
}

bool parseIntVb6(std::string_view s, int& out) {
    double d;
    if (!parseDoubleVb6(s, d)) return false;
    if (d < -2147483648.0 || d > 2147483647.0) return false;
    out = int(std::round(d));
    return true;
}

bool parseLength(std::string_view s, const std::vector<double>& unitFactors, double& out) {
    s = trim(s);
    if (s.find(':') == std::string_view::npos) {
        double v;
        if (!parseExact(s, v)) return false;
        out = v * (unitFactors.empty() ? 1.0 : unitFactors.back());
        return true;
    }
    const auto parts = split(s, ':');
    double total = 0.0;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::string_view p = trim(parts[i]);
        if (p.empty()) continue;
        double v;
        if (!parseExact(p, v)) return false;
        const std::size_t fi = unitFactors.size() - parts.size() + i;
        const double f = (unitFactors.size() >= parts.size()) ? unitFactors[fi] : 1.0;
        total += v * f;
    }
    out = total;
    return true;
}

std::string asciiOnly(std::string_view utf8) {
    std::string r;
    for (char c : utf8)
        if (static_cast<unsigned char>(c) < 0x80) r.push_back(c);
    return r;
}

} // namespace ots
