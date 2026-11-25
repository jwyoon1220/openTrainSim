#include "util/Json.hpp"

#include "util/Text.hpp"

#include <cctype>
#include <charconv>
#include <format>

namespace ots {

const Json& Json::null() {
    static const Json n;
    return n;
}

const Json& Json::operator[](std::string_view key) const {
    for (const auto& [k, v] : members_)
        if (k == key) return v;
    return null();
}

class JsonParser {
public:
    explicit JsonParser(std::string_view s) : s_(s) {}

    bool run(Json& out, std::string& error) {
        if (!value(out, 0)) return error = error_, false;
        skip();
        if (pos_ < s_.size()) return error = fail("unexpected text after the end"), false;
        return true;
    }

private:
    std::string_view s_;
    std::size_t pos_ = 0;
    std::string error_;

    std::string fail(std::string_view msg) {
        int line = 1;
        for (std::size_t i = 0; i < pos_ && i < s_.size(); ++i) line += s_[i] == '\n';
        error_ = std::format("line {}: {}", line, msg);
        return error_;
    }
    void skip() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '/') {
                while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
            } else if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '*') {
                const std::size_t e = s_.find("*/", pos_ + 2);
                pos_ = e == std::string_view::npos ? s_.size() : e + 2;
            } else {
                break;
            }
        }
    }
    bool literal(std::string_view w) {
        if (s_.substr(pos_, w.size()) != w) return false;
        pos_ += w.size();
        return true;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) {
            o += char(cp);
        } else if (cp < 0x800) {
            o += char(0xC0 | (cp >> 6)), o += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            o += char(0xE0 | (cp >> 12)), o += char(0x80 | ((cp >> 6) & 0x3F)), o += char(0x80 | (cp & 0x3F));
        } else {
            o += char(0xF0 | (cp >> 18)), o += char(0x80 | ((cp >> 12) & 0x3F)), o += char(0x80 | ((cp >> 6) & 0x3F)),
                o += char(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned& v) {
        if (pos_ + 4 > s_.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& o) {
        ++pos_; // opening quote
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                o += c;
                continue;
            }
            if (pos_ >= s_.size()) break;
            const char e = s_[pos_++];
            switch (e) {
            case 'n': o += '\n'; break;
            case 't': o += '\t'; break;
            case 'r': o += '\r'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'u': {
                unsigned cp;
                if (!hex4(cp)) return fail("bad \\u escape"), false;
                if (cp >= 0xD800 && cp < 0xDC00 && s_.substr(pos_, 2) == "\\u") {
                    pos_ += 2;
                    unsigned lo;
                    if (!hex4(lo)) return fail("bad \\u escape"), false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(o, cp);
                break;
            }
            default: o += e; break;
            }
        }
        return fail("unterminated string"), false;
    }
    bool value(Json& v, int depth) {
        if (depth > 64) return fail("nested too deeply"), false;
        skip();
        if (pos_ >= s_.size()) return fail("unexpected end of file"), false;
        const char c = s_[pos_];
        if (c == '{') {
            v.type_ = Json::Type::Object;
            ++pos_;
            for (;;) {
                skip();
                if (pos_ < s_.size() && s_[pos_] == '}') return ++pos_, true;
                if (pos_ >= s_.size() || s_[pos_] != '"') return fail("expected a \"key\" or }"), false;
                std::string key;
                if (!string(key)) return false;
                skip();
                if (pos_ >= s_.size() || s_[pos_] != ':') return fail("expected : after \"" + key + "\""), false;
                ++pos_;
                Json item;
                if (!value(item, depth + 1)) return false;
                v.members_.emplace_back(std::move(key), std::move(item));
                skip();
                if (pos_ < s_.size() && s_[pos_] == ',') ++pos_;
                else if (pos_ < s_.size() && s_[pos_] == '}') return ++pos_, true;
                else return fail("expected , or }"), false;
            }
        }
        if (c == '[') {
            v.type_ = Json::Type::Array;
            ++pos_;
            for (;;) {
                skip();
                if (pos_ < s_.size() && s_[pos_] == ']') return ++pos_, true;
                Json item;
                if (!value(item, depth + 1)) return false;
                v.items_.push_back(std::move(item));
                skip();
                if (pos_ < s_.size() && s_[pos_] == ',') ++pos_;
                else if (pos_ < s_.size() && s_[pos_] == ']') return ++pos_, true;
                else return fail("expected , or ]"), false;
            }
        }
        if (c == '"') return v.type_ = Json::Type::String, string(v.string_);
        if (literal("true")) return v.type_ = Json::Type::Bool, v.bool_ = true, true;
        if (literal("false")) return v.type_ = Json::Type::Bool, v.bool_ = false, true;
        if (literal("null")) return v.type_ = Json::Type::Null, true;
        std::size_t start = pos_;
        if (s_[pos_] == '+') ++start, ++pos_; // tolerated
        while (pos_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '-' || s_[pos_] == '+' ||
                                    s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E'))
            ++pos_;
        if (pos_ == start) return fail(std::format("unexpected '{}'", c)), false;
        double d = 0.0;
        const auto r = std::from_chars(s_.data() + start, s_.data() + pos_, d);
        if (r.ec != std::errc() || r.ptr != s_.data() + pos_) return fail("bad number"), false;
        v.type_ = Json::Type::Number;
        v.number_ = d;
        return true;
    }
};

Expected<Json> Json::parse(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3); // UTF-8 BOM
    Json out;
    std::string error;
    if (!JsonParser(text).run(out, error)) return Unexpected(std::move(error));
    return out;
}

Expected<Json> Json::parseFile(const std::filesystem::path& file) {
    const auto text = readTextFile(file); // not UTF-8: the content code page (CP949 Notepad files)
    if (!text) return Unexpected("cannot read");
    return parse(*text);
}

} // namespace ots
