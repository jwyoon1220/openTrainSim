#include "audio/SoundCfg.hpp"

#include "util/Paths.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <string_view>
#include <utility>

namespace ots {
namespace {

struct NamedSound {
    std::string_view section, key;
    fs::path TrainSoundFiles::* field;
};

constexpr NamedSound namedSounds[] = {
    {"brake", "bcreleasehigh", &TrainSoundFiles::bcReleaseHigh},
    {"brake", "bcrelease", &TrainSoundFiles::bcRelease},
    {"brake", "bcreleasefull", &TrainSoundFiles::bcReleaseFull},
    {"brake", "emergency", &TrainSoundFiles::emergency},
    {"brake", "emergencyrelease", &TrainSoundFiles::emergencyRelease},
    {"brake", "bpdecomp", &TrainSoundFiles::bpDecomp},
    {"compressor", "attack", &TrainSoundFiles::compressorAttack},
    {"compressor", "loop", &TrainSoundFiles::compressorLoop},
    {"compressor", "release", &TrainSoundFiles::compressorRelease},
    {"suspension", "left", &TrainSoundFiles::springLeft},
    {"suspension", "right", &TrainSoundFiles::springRight},
    {"horn", "primary", &TrainSoundFiles::hornPrimary},
    {"horn", "primarystart", &TrainSoundFiles::hornPrimaryStart},
    {"horn", "primaryloop", &TrainSoundFiles::hornPrimaryLoop},
    {"horn", "primaryend", &TrainSoundFiles::hornPrimaryEnd},
    {"horn", "secondary", &TrainSoundFiles::hornSecondary},
    {"horn", "secondarystart", &TrainSoundFiles::hornSecondaryStart},
    {"horn", "secondaryloop", &TrainSoundFiles::hornSecondaryLoop},
    {"horn", "secondaryend", &TrainSoundFiles::hornSecondaryEnd},
    {"horn", "music", &TrainSoundFiles::hornMusic},
    {"horn", "musicstart", &TrainSoundFiles::hornMusicStart},
    {"horn", "musicloop", &TrainSoundFiles::hornMusicLoop},
    {"horn", "musicend", &TrainSoundFiles::hornMusicEnd},
    {"door", "openleft", &TrainSoundFiles::doorOpenLeft},
    {"door", "openright", &TrainSoundFiles::doorOpenRight},
    {"door", "closeleft", &TrainSoundFiles::doorCloseLeft},
    {"door", "closeright", &TrainSoundFiles::doorCloseRight},
    {"buzzer", "correct", &TrainSoundFiles::buzzerCorrect},
    {"pilotlamp", "on", &TrainSoundFiles::pilotLampOn},
    {"pilotlamp", "off", &TrainSoundFiles::pilotLampOff},
    {"brakehandle", "apply", &TrainSoundFiles::brakeApply},
    {"brakehandle", "applyfast", &TrainSoundFiles::brakeApplyFast},
    {"brakehandle", "release", &TrainSoundFiles::brakeRelease},
    {"brakehandle", "releasefast", &TrainSoundFiles::brakeReleaseFast},
    {"brakehandle", "min", &TrainSoundFiles::brakeMin},
    {"brakehandle", "max", &TrainSoundFiles::brakeMax},
    {"mastercontroller", "up", &TrainSoundFiles::powerUp},
    {"mastercontroller", "upfast", &TrainSoundFiles::powerUpFast},
    {"mastercontroller", "down", &TrainSoundFiles::powerDown},
    {"mastercontroller", "downfast", &TrainSoundFiles::powerDownFast},
    {"mastercontroller", "min", &TrainSoundFiles::powerMin},
    {"mastercontroller", "max", &TrainSoundFiles::powerMax},
    {"reverser", "on", &TrainSoundFiles::reverserOn},
    {"reverser", "off", &TrainSoundFiles::reverserOff},
    {"breaker", "on", &TrainSoundFiles::breakerOn},
    {"breaker", "off", &TrainSoundFiles::breakerOff},
    {"others", "noise", &TrainSoundFiles::noise},
    {"others", "shoe", &TrainSoundFiles::shoe},
    {"others", "halt", &TrainSoundFiles::halt},
};

using SoundMap = std::map<int, fs::path>;

struct IndexedSection {
    std::string_view name;
    SoundMap TrainSoundFiles::* field;
};

constexpr IndexedSection indexedSections[] = {
    {"run", &TrainSoundFiles::run},
    {"flange", &TrainSoundFiles::flange},
    {"motor", &TrainSoundFiles::motor},
    {"switch", &TrainSoundFiles::pointSwitch},
    {"ats", &TrainSoundFiles::ats},
};

std::string compact(std::string_view text) {
    std::string result = lower(text);
    std::erase_if(result, [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; });
    return result;
}

fs::path resolveSound(const fs::path& folder, std::string_view name) {
    auto path = resolvePath(folder, name);
    return fileExists(path) ? path : fs::path{};
}

bool loadBve2(const fs::path& folder, TrainSoundFiles& out) {
    bool found = false;
    const auto load = [&](fs::path& field, std::string_view name) {
        field = resolveSound(folder, name);
        found |= !field.empty();
    };
    constexpr std::pair<std::string_view, fs::path TrainSoundFiles::*> defaults[] = {
        {"Adjust.wav", &TrainSoundFiles::buzzerCorrect},
        {"Brake.wav", &TrainSoundFiles::bpDecomp},
        {"Halt.wav", &TrainSoundFiles::halt},
        {"Klaxon0.wav", &TrainSoundFiles::hornPrimary},
        {"Klaxon1.wav", &TrainSoundFiles::hornSecondary},
        {"Klaxon2.wav", &TrainSoundFiles::hornMusic},
        {"Leave.wav", &TrainSoundFiles::pilotLampOn},
        {"Air.wav", &TrainSoundFiles::bcRelease},
        {"AirHigh.wav", &TrainSoundFiles::bcReleaseHigh},
        {"AirZero.wav", &TrainSoundFiles::bcReleaseFull},
        {"CpStart.wav", &TrainSoundFiles::compressorAttack},
        {"CpLoop.wav", &TrainSoundFiles::compressorLoop},
        {"CpEnd.wav", &TrainSoundFiles::compressorRelease},
        {"DoorClsL.wav", &TrainSoundFiles::doorCloseLeft},
        {"DoorClsR.wav", &TrainSoundFiles::doorCloseRight},
        {"DoorOpnL.wav", &TrainSoundFiles::doorOpenLeft},
        {"DoorOpnR.wav", &TrainSoundFiles::doorOpenRight},
        {"EmrBrake.wav", &TrainSoundFiles::emergency},
        {"Loop.wav", &TrainSoundFiles::noise},
        {"Rub.wav", &TrainSoundFiles::shoe},
        {"SpringL.wav", &TrainSoundFiles::springLeft},
        {"SpringR.wav", &TrainSoundFiles::springRight},
    };
    for (const auto& [name, field] : defaults) load(out.*field, name);
    if (out.hornPrimary.empty()) load(out.hornPrimary, "Klaxon.wav");
    if (out.doorCloseLeft.empty()) load(out.doorCloseLeft, "DoorCls.wav");
    if (out.doorCloseRight.empty()) load(out.doorCloseRight, "DoorCls.wav");
    if (out.doorOpenLeft.empty()) load(out.doorOpenLeft, "DoorOpn.wav");
    if (out.doorOpenRight.empty()) load(out.doorOpenRight, "DoorOpn.wav");

    if (auto point = resolveSound(folder, "Point.wav"); !point.empty()) {
        out.pointSwitch[0] = std::move(point);
        found = true;
    }
    constexpr std::string_view atsNames[] = {"ats.wav", "atscnt.wav", "ding.wav", "toats.wav", "toatc.wav", "eb.wav"};
    for (int index = 0; const auto name : atsNames) {
        if (auto path = resolveSound(folder, name); !path.empty()) {
            out.ats[index] = std::move(path);
            found = true;
        }
        ++index;
    }

    std::error_code ec;
    for (auto it = fs::directory_iterator(folder, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const std::string name = lower(pathString(it->path().filename()));
        if (!iendsWith(name, ".wav") || !fileExists(it->path())) continue;
        for (const auto& [prefix, field] : indexedSections) {
            if (prefix == "switch" || prefix == "ats" || name.size() <= prefix.size() + 4 ||
                !name.starts_with(prefix)) continue;
            auto digits = trim(std::string_view(name).substr(prefix.size(), name.size() - prefix.size() - 4));
            if (digits.starts_with('+')) digits.remove_prefix(1);
            int index = 0;
            const auto end = digits.data() + digits.size();
            const auto result = std::from_chars(digits.data(), end, index);
            if (result.ec == std::errc{} && result.ptr == end && index >= 0) {
                (out.*field)[index] = it->path();
                found = true;
            }
            break;
        }
    }
    return found;
}

} // namespace

bool loadTrainSounds(const fs::path& trainFolder, TrainSoundFiles& out) {
    out = {};
    const auto folder = fs::absolute(trainFolder.empty() ? fs::path(".") : trainFolder);
    const auto cfg = resolveSound(folder, "sound.cfg");
    if (cfg.empty()) return loadBve2(folder, out);

    std::size_t lineNumber = 0;
    const auto warn = [&](std::string_view message) {
        out.warnings.push_back(std::format("{}:{}: {}", pathString(cfg), lineNumber, message));
    };
    std::string contents;
    if (!readTextFile(cfg, contents)) {
        warn("cannot read sound.cfg");
        return true;
    }
    std::string section;
    SoundMap* indexed = nullptr;
    bool headerSeen = false;
    for (const auto& raw : splitLines(contents)) {
        ++lineNumber;
        const auto line = trim(std::string_view(raw).substr(0, raw.find(';')));
        if (line.empty()) continue;
        if (!headerSeen) {
            headerSeen = true;
            if (iequals(line, "Version 1.0")) continue;
            warn("expected Version 1.0");
            if (istartsWith(line, "Version ")) continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = compact(line.substr(1, line.size() - 2));
            const auto it = std::ranges::find(indexedSections, section, &IndexedSection::name);
            indexed = it == std::end(indexedSections) ? nullptr : &(out.*(it->field));
            if (!indexed && std::ranges::find(namedSounds, section, &NamedSound::section) == std::end(namedSounds))
                warn(std::format("unknown section {}", line));
            continue;
        }

        const auto eq = line.find('=');
        const auto key = trim(line.substr(0, eq));
        const bool bareSwitch = eq == std::string_view::npos && section == "switch";
        if (eq == std::string_view::npos && !bareSwitch) {
            warn(std::format("expected key = file, got '{}'", line));
            continue;
        }

        int index = 0;
        fs::path* field = nullptr;
        if (indexed) {
            if (!bareSwitch && (!parseIntVb6(key, index) || index < 0)) {
                warn(std::format("invalid [{}] index '{}'", section, key));
                continue;
            }
        } else {
            const auto normalized = compact(key);
            const auto it = std::ranges::find_if(namedSounds, [&](const NamedSound& sound) {
                return sound.section == section && sound.key == normalized;
            });
            if (it == std::end(namedSounds)) {
                warn(std::format("unknown [{}] key '{}'", section, key));
                continue;
            }
            field = &(out.*(it->field));
        }

        const auto value = bareSwitch ? line : trim(line.substr(eq + 1));
        auto path = resolveSound(folder, value);
        if (path.empty()) warn(std::format("missing sound file '{}'", value));
        if (field) {
            *field = std::move(path);
        } else if (!path.empty()) {
            (*indexed)[index] = std::move(path);
        } else {
            indexed->erase(index);
        }
    }
    if (!headerSeen) warn("expected Version 1.0");
    return true;
}

} // namespace ots
