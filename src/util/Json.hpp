#pragma once
// Small JSON reader for content files (cab.json). Accepts // and /* */ comments and trailing
// commas, since the files are written by hand.
#include "util/Expected.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ots {

class Json {
public:
    enum class Type : unsigned char { Null, Bool, Number, String, Array, Object };

    // Syntax errors read "line N: message".
    static Expected<Json> parse(std::string_view text);
    // Reads a file (UTF-8 / UTF-16 with BOM, else the content code page) and parses it.
    static Expected<Json> parseFile(const std::filesystem::path& file);

    [[nodiscard]] Type type() const { return type_; }
    [[nodiscard]] bool isNull() const { return type_ == Type::Null; }
    [[nodiscard]] bool isBool() const { return type_ == Type::Bool; }
    [[nodiscard]] bool isNumber() const { return type_ == Type::Number; }
    [[nodiscard]] bool isString() const { return type_ == Type::String; }
    [[nodiscard]] bool isArray() const { return type_ == Type::Array; }
    [[nodiscard]] bool isObject() const { return type_ == Type::Object; }

    [[nodiscard]] double number(double def = 0.0) const { return type_ == Type::Number ? number_ : type_ == Type::Bool ? (bool_ ? 1.0 : 0.0) : def; }
    [[nodiscard]] bool boolean(bool def = false) const { return type_ == Type::Bool ? bool_ : type_ == Type::Number ? number_ != 0.0 : def; }
    [[nodiscard]] const std::string& string() const { return string_; }

    // Arrays
    [[nodiscard]] std::size_t size() const { return items_.size(); }
    [[nodiscard]] const Json& operator[](std::size_t i) const { return i < items_.size() ? items_[i] : null(); }
    [[nodiscard]] const std::vector<Json>& items() const { return items_; }
    // Objects (member order is kept)
    [[nodiscard]] const Json& operator[](std::string_view key) const;
    [[nodiscard]] bool has(std::string_view key) const { return !(*this)[key].isNull(); }
    [[nodiscard]] const std::vector<std::pair<std::string, Json>>& members() const { return members_; }

    // Convenience lookups with defaults.
    [[nodiscard]] double num(std::string_view key, double def) const { return (*this)[key].number(def); }
    [[nodiscard]] bool flag(std::string_view key, bool def) const { return (*this)[key].boolean(def); }
    [[nodiscard]] std::string str(std::string_view key, std::string def = {}) const {
        const Json& v = (*this)[key];
        return v.isString() ? v.string_ : def;
    }

    static const Json& null();

private:
    friend class JsonParser;
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Json> items_;
    std::vector<std::pair<std::string, Json>> members_;
};

} // namespace ots
