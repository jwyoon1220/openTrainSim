#include "safety/Safety.hpp"

#include "safety/Ats.hpp"
#include "safety/Atp.hpp"
#include "safety/Tvm430.hpp"
#include "sim/Train.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>

namespace ots::safety {
namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr int kFullService = 1000; // Command::brake: clamped to the train's last service notch

double kmh(const Json& v, double def) { return v.isNumber() ? v.number() / 3.6 : def; }

} // namespace

std::optional<System> parseSystem(std::string_view key) {
    for (std::size_t i = 0; i < kSystemKeys.size(); ++i)
        if (iequals(key, kSystemKeys[i])) return System(i);
    return std::nullopt;
}

// ---------------------------------------------------------------- Trackside

std::pair<std::size_t, std::size_t> Trackside::signalsBetween(double from, double to) const {
    auto above = [&](double pos) {
        return std::size_t(std::ranges::upper_bound(sections_, pos, {}, &Section::trackPos) - sections_.begin());
    };
    return {above(from), above(to)};
}

std::optional<std::size_t> Trackside::nextStop(double pos) const {
    for (std::size_t i = signalsBetween(pos, pos).second; i < sections_.size(); ++i)
        if (isStop(i)) return i;
    return std::nullopt;
}

int Trackside::freeBlocksAfter(std::size_t section, int cap) const {
    int n = 0;
    for (std::size_t i = section + 1; i < sections_.size() && n < cap && !isStop(i); ++i) ++n;
    if (section + 1 + std::size_t(n) >= sections_.size()) n = cap; // runs off the end: clear
    return n;
}

std::optional<System> Trackside::zoneAt(double pos) const {
    const auto& zones = route_.safetyZones;
    const auto it = std::ranges::upper_bound(zones, pos, {}, &SafetyZone::trackPos);
    if (it == zones.begin()) return std::nullopt;
    return parseSystem(std::prev(it)->system);
}

// ---------------------------------------------------------------- configuration

std::size_t AtsConfig::codeFor(int aspect) const {
    for (const auto& [a, code] : aspectCodes)
        if (a == aspect) return code;
    return clearCode;
}

SafetyConfig defaultConfig() {
    SafetyConfig c;
    c.systems = {System::Ats};
    // KORAIL conventional-line ATS codes and the usual 5-aspect numbering of routes
    // (0 R, 1 YY, 2 Y, 3 YG, 4 G). Speeds are the commonly cited supervision values.
    c.ats.codes = {{"R0", 0.0, true}, {"YY", 25 / 3.6, false}, {"Y", 45 / 3.6, false}, {"YG", 65 / 3.6, false}, {"G", kInf, false}};
    c.ats.aspectCodes = {{0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 4}};
    c.ats.clearCode = 4;
    // TVM430 steps of the Korean high-speed lines (KTX)
    for (const double v : {300.0, 270.0, 230.0, 170.0, 80.0, 0.0}) c.tvm.steps.push_back(v / 3.6);
    return c;
}

Expected<SafetyConfig> loadConfig(const std::filesystem::path& file) {
    auto parsed = Json::parseFile(file);
    if (!parsed) return Unexpected(parsed.error());
    const Json& j = *parsed;
    SafetyConfig c = defaultConfig();
    if (const Json& list = j["systems"]; list.isArray()) {
        c.systems.clear();
        for (const Json& s : list.items()) {
            const auto sys = parseSystem(s.string());
            if (!sys) return Unexpected(std::format("unknown system '{}' (ats, tvm430, atp)", s.string()));
            c.systems.push_back(*sys);
        }
    }
    if (j.has("default")) {
        const auto sys = parseSystem(j.str("default"));
        if (!sys) return Unexpected(std::format("unknown default system '{}'", j.str("default")));
        c.defaultSystem = *sys;
    } else if (!c.systems.empty()) {
        c.defaultSystem = c.systems.front();
    }
    c.transitionAckSeconds = j.num("transitionAckSeconds", c.transitionAckSeconds);

    if (const Json& a = j["ats"]; a.isObject()) {
        AtsConfig& ats = c.ats;
        ats.ackSeconds = a.num("ackSeconds", ats.ackSeconds);
        ats.tolerance = kmh(a["toleranceKmh"], ats.tolerance);
        ats.minBrake = int(a.num("minBrake", ats.minBrake));
        ats.afterStop = kmh(a["afterStopKmh"], ats.afterStop);
        if (const Json& codes = a["codes"]; codes.isArray()) {
            ats.codes.clear();
            for (const Json& k : codes.items())
                ats.codes.push_back({k.str("name"), kmh(k["kmh"], kInf), k.flag("stop", false)});
            if (ats.codes.empty()) return Unexpected("ats.codes is empty");
            ats.aspectCodes.clear();
            ats.clearCode = ats.codes.size() - 1;
        }
        auto codeIndex = [&](const std::string& name) -> std::optional<std::size_t> {
            for (std::size_t i = 0; i < ats.codes.size(); ++i)
                if (iequals(ats.codes[i].name, name)) return i;
            return std::nullopt;
        };
        if (const Json& map = a["aspects"]; map.isObject()) {
            ats.aspectCodes.clear();
            for (const auto& [key, value] : map.members()) {
                int aspect = 0;
                const auto [end, ec] = std::from_chars(key.data(), key.data() + key.size(), aspect);
                const auto code = codeIndex(value.string());
                if (ec != std::errc() || end != key.data() + key.size()) return Unexpected(std::format("ats.aspects: '{}' is not an aspect number", key));
                if (!code) return Unexpected(std::format("ats.aspects: unknown code '{}'", value.string()));
                ats.aspectCodes.emplace_back(aspect, *code);
            }
        }
        if (a.has("clear")) {
            const auto code = codeIndex(a.str("clear"));
            if (!code) return Unexpected(std::format("ats.clear: unknown code '{}'", a.str("clear")));
            ats.clearCode = *code;
        }
    }
    if (const Json& t = j["tvm430"]; t.isObject()) {
        TvmConfig& tvm = c.tvm;
        if (const Json& steps = t["steps"]; steps.isArray()) {
            tvm.steps.clear();
            for (const Json& v : steps.items()) tvm.steps.push_back(v.number() / 3.6);
            if (tvm.steps.size() < 2 || tvm.steps.back() != 0.0) return Unexpected("tvm430.steps: at least two, the last 0 (000)");
        }
        tvm.warningMargin = kmh(t["warningMarginKmh"], tvm.warningMargin);
        tvm.emergencyMargin = kmh(t["emergencyMarginKmh"], tvm.emergencyMargin);
        tvm.finalDecel = t.num("finalDecel", tvm.finalDecel);
    }
    if (const Json& p = j["atp"]; p.isObject()) {
        AtpConfig& atp = c.atp;
        atp.serviceDecel = p.num("serviceDecel", atp.serviceDecel);
        atp.emergencyDecel = p.num("emergencyDecel", atp.emergencyDecel);
        atp.brakeDelay = p.num("brakeDelay", atp.brakeDelay);
        atp.indicationTime = p.num("indicationTime", atp.indicationTime);
        atp.releaseSpeed = kmh(p["releaseSpeedKmh"], atp.releaseSpeed);
        atp.ceilingWarning = kmh(p["ceilingWarningKmh"], atp.ceilingWarning);
        atp.ceilingService = kmh(p["ceilingServiceKmh"], atp.ceilingService);
        atp.ceilingEmergency = kmh(p["ceilingEmergencyKmh"], atp.ceilingEmergency);
        atp.staffResponsible = kmh(p["staffResponsibleKmh"], atp.staffResponsible);
        atp.lookahead = p.num("lookahead", atp.lookahead);
    }
    return c;
}

// ---------------------------------------------------------------- Onboard

Onboard::Onboard(const SafetyConfig& config) : config_(config) {
    for (System s : config_.systems) {
        switch (s) {
        case System::Ats: devices_.push_back(std::make_unique<Ats>(config_.ats)); break;
        case System::Tvm430: devices_.push_back(std::make_unique<Tvm430>(config_.tvm)); break;
        case System::Atp: devices_.push_back(std::make_unique<Atp>(config_.atp)); break;
        }
    }
}

Onboard::~Onboard() = default;

Device* Onboard::find(System s) {
    for (auto& d : devices_)
        if (d->system == s) return d.get();
    return nullptr;
}

const Device* Onboard::device(System s) const {
    for (const auto& d : devices_)
        if (d->system == s) return d.get();
    return nullptr;
}

std::size_t Onboard::atsCode() const {
    const Device* d = device(System::Ats);
    return d ? static_cast<const Ats*>(d)->code() : 0;
}

std::optional<System> Onboard::active() const { return active_ ? std::optional(active_->system) : std::nullopt; }

void Onboard::acknowledge(bool down) {
    if (down && !ackHeld_ && transition_) transition_.reset();
    ackHeld_ = down;
    if (active_) active_->acknowledge(down);
}

void Onboard::update(const Trackside& track, TrainSim& t, double dt) {
    const double previous = previousFront_.value_or(t.front);
    previousFront_ = t.front;
    const Context ctx{track, t, dt, previous};

    // the device for the zone the train is in
    const System want = track.zoneAt(t.front).value_or(config_.defaultSystem);
    Device* wanted = find(want);
    if (!active_) {
        active_ = wanted ? wanted : devices_.empty() ? nullptr : devices_.front().get();
        if (active_) active_->activate(ctx);
    } else if (wanted && wanted != active_) {
        active_ = wanted; // the new system supervises at once; the driver confirms the change
        active_->activate(ctx);
        transition_ = Transition{want};
    }

    Command cmd;
    if (active_) {
        active_->update(ctx);
        cmd = active_->command;
        display_ = active_->display;
    } else {
        display_ = {};
    }
    if (!wanted) display_.message = std::format("{} 미장착 구간", label(want));
    if (transition_) {
        transition_->elapsed += dt;
        display_.ackRequired = true;
        display_.message = std::format("{} 구간 진입: 확인", label(transition_->to));
        if (transition_->elapsed > config_.transitionAckSeconds) {
            cmd.merge({.brake = kFullService});
            display_.braking = true;
        } else {
            display_.warning = true;
        }
    }

    // override the driver's handles with the strongest demand
    const bool braking = cmd.brake > 0 || cmd.emergency;
    if (cmd.any()) {
        const int driverBrake = t.emergency ? t.maxBrake() + 1 : t.brakeNotch;
        t.safetyActive = true;
        t.safetyReverser = t.reverser;
        t.safetyPower = cmd.powerCut || braking ? 0 : t.powerNotch;
        t.safetyBrake = cmd.emergency ? t.maxBrake() + 1 : std::max(driverBrake, std::min(cmd.brake, t.maxBrake()));
        t.safetyHoldBrake = t.holdBrake;
    } else {
        t.safetyActive = false;
    }
    if (braking && !wasBraking_) ++interventions;
    display_.emergency = display_.emergency || cmd.emergency;
    if (cmd.emergency && !wasEmergency_) ++emergencies;
    wasBraking_ = braking;
    wasEmergency_ = cmd.emergency;
}

} // namespace ots::safety
