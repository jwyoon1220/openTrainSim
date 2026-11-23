#include "util/Config.hpp"

#include "util/Text.hpp"

#include <format>
#include <fstream>

namespace ots {

bool Config::load(const std::filesystem::path& path) {
    std::string text;
    if (!readTextFile(path, text)) return false;
    std::string section;
    for (const std::string& raw : splitLines(text)) {
        std::string_view line = trim(raw);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        values_[section][lower(trim(line.substr(0, eq)))] = std::string(trim(line.substr(eq + 1)));
    }
    return true;
}

bool Config::save(const std::filesystem::path& path) const {
    std::ofstream f(path);
    if (!f) return false;
    f << "; openTrainSim settings\n";
    for (const auto& [section, kv] : values_) {
        f << "\n[" << section << "]\n";
        for (const auto& [k, v] : kv) f << k << " = " << v << "\n";
    }
    return true;
}

std::string Config::get(const std::string& section, const std::string& key, const std::string& def) const {
    auto s = values_.find(lower(section));
    if (s == values_.end()) return def;
    auto k = s->second.find(lower(key));
    return k == s->second.end() ? def : k->second;
}

double Config::getNumber(const std::string& section, const std::string& key, double def) const {
    double v;
    return parseDouble(get(section, key), v) ? v : def;
}

int Config::getInt(const std::string& section, const std::string& key, int def) const {
    int v;
    return parseIntVb6(get(section, key), v) ? v : def;
}

bool Config::getBool(const std::string& section, const std::string& key, bool def) const {
    const std::string v = lower(get(section, key));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return def;
}

void Config::set(const std::string& section, const std::string& key, const std::string& value) {
    values_[lower(section)][lower(key)] = value;
}

void Config::setNumber(const std::string& section, const std::string& key, double value) {
    set(section, key, std::format("{}", value));
}

} // namespace ots
