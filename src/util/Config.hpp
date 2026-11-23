#pragma once
// Simple INI settings (openTrainSim.cfg): [section] key = value, ';' / '#' comments.
// Unknown keys are kept so the file round-trips.
#include <filesystem>
#include <map>
#include <string>

namespace ots {

class Config {
public:
    bool load(const std::filesystem::path& path);
    bool save(const std::filesystem::path& path) const;

    [[nodiscard]] std::string get(const std::string& section, const std::string& key, const std::string& def = {}) const;
    [[nodiscard]] double getNumber(const std::string& section, const std::string& key, double def) const;
    [[nodiscard]] int getInt(const std::string& section, const std::string& key, int def) const;
    [[nodiscard]] bool getBool(const std::string& section, const std::string& key, bool def) const;
    void set(const std::string& section, const std::string& key, const std::string& value);
    void setNumber(const std::string& section, const std::string& key, double value);

private:
    std::map<std::string, std::map<std::string, std::string>> values_; // lower-case keys
};

} // namespace ots
