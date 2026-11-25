#include "audio/Vvvf.hpp"

#include "util/Json.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace ots {

namespace {
constexpr double kTau = 2.0 * std::numbers::pi;

struct Fail {
    std::string msg;
};

// A stage list: [{ "until_kmh" | "until_hz": x, "async": hz | [from, to], "random": hz } |
//                { ..., "sync": pulses } | { ..., "pulse": 1 }, ...]
// km/h limits become fundamental frequencies at that speed (rotor ± slip).
std::vector<VvvfStage> stages(const Json& list, double hzPerKmh, double slipHz, std::string_view what) {
    if (!list.isArray() || list.size() == 0) throw Fail{std::format("\"{}\" must be a non-empty array of stages", what)};
    std::vector<VvvfStage> out;
    double from = 0.0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Json& j = list[i];
        VvvfStage s;
        s.fromHz = from;
        if (j.has("until_hz")) s.untilHz = j.num("until_hz", 0.0);
        else if (j.has("until_kmh")) s.untilHz = j.num("until_kmh", 0.0) * hzPerKmh + slipHz;
        else if (i + 1 < list.size()) throw Fail{std::format("{}[{}]: needs \"until_kmh\" or \"until_hz\" (only the last stage may omit it)", what, i)};
        if (const Json& a = j["async"]; !a.isNull()) {
            s.mode = VvvfStage::Mode::Async;
            if (a.isArray()) s.carrierFrom = a[0].number(1000.0), s.carrierTo = a[1].number(s.carrierFrom);
            else s.carrierFrom = s.carrierTo = a.number(1000.0);
            s.random = j.num("random", 0.0);
            if (s.carrierFrom <= 0.0 || s.carrierTo <= 0.0) throw Fail{std::format("{}[{}]: carrier must be > 0 Hz", what, i)};
        } else if (j.has("sync")) {
            s.mode = VvvfStage::Mode::Sync;
            s.pulses = int(j.num("sync", 9.0));
            if (s.pulses < 1) throw Fail{std::format("{}[{}]: sync pulses must be >= 1", what, i)};
        } else if (j.has("pulse")) {
            s.mode = VvvfStage::Mode::OnePulse;
        } else {
            throw Fail{std::format("{}[{}]: needs \"async\", \"sync\" or \"pulse\"", what, i)};
        }
        if (s.untilHz <= from) throw Fail{std::format("{}[{}]: stages must be in increasing speed order", what, i)};
        from = s.untilHz;
        out.push_back(s);
    }
    return out;
}

double triangle(double phase) { // -1 at 0, +1 at 0.5
    const double f = phase - std::floor(phase);
    return 4.0 * std::abs(f - 0.5) * -1.0 + 1.0;
}
} // namespace

double VvvfPreset::hzPerMps() const { return gearRatio / (std::numbers::pi * wheelDiameter) * poles * 0.5; }

Expected<VvvfPreset> VvvfPreset::load(const std::filesystem::path& file) {
    const auto parsed = Json::parseFile(file);
    if (!parsed) return Unexpected(parsed.error());
    const Json& root = *parsed;
    try {
        VvvfPreset p;
        p.name = root.str("name", file.stem().string());
        const Json& m = root["motor"];
        p.poles = m.num("poles", p.poles);
        p.gearRatio = m.num("gear_ratio", p.gearRatio);
        p.wheelDiameter = m.num("wheel_diameter", p.wheelDiameter);
        p.slipHz = m.num("slip_hz", p.slipHz);
        if (p.poles <= 0 || p.gearRatio <= 0 || p.wheelDiameter <= 0) throw Fail{"motor: poles, gear_ratio and wheel_diameter must be > 0"};
        p.baseHz = root.num("base_hz", p.baseHz);
        p.minAmplitude = root.num("min_amplitude", p.minAmplitude);
        p.maxAmplitude = root.num("max_amplitude", p.maxAmplitude);
        p.volume = root.num("volume", p.volume);
        p.radius = root.num("radius", p.radius);
        p.lowpassHz = root.num("lowpass_hz", p.lowpassHz);
        p.highpassHz = root.num("highpass_hz", p.highpassHz);
        p.brakeCutoffKmh = root.num("brake_cutoff_kmh", p.brakeCutoffKmh);
        p.replaceMotor = root.flag("replace_motor", p.replaceMotor);
        const double hzPerKmh = p.hzPerMps() / 3.6;
        p.power = stages(root["power"], hzPerKmh, p.slipHz, "power");
        p.brake = stages(root.has("brake") ? root["brake"] : root["power"], hzPerKmh, -p.slipHz, "brake");
        return p;
    } catch (const Fail& f) {
        return Unexpected(f.msg);
    }
}

VvvfSynth::VvvfSynth(VvvfPreset preset, int sampleRate) : preset_(std::move(preset)), rate_(sampleRate) {}

void VvvfSynth::set(double speedMps, int direction, double torque) {
    const double rotor = std::abs(speedMps) * preset_.hzPerMps();
    if (direction < 0 && std::abs(speedMps) * 3.6 < preset_.brakeCutoffKmh) direction = 0;
    target_.dir = direction;
    target_.hz = std::max(0.0, rotor + (direction > 0 ? preset_.slipHz : direction < 0 ? -preset_.slipHz : 0.0));
    target_.gain = direction ? 0.3 + 0.7 * std::clamp(torque, 0.0, 1.0) : 0.0;
}

const VvvfStage* VvvfSynth::stageFor(double hz, int dir) const {
    const auto& list = dir < 0 ? preset_.brake : preset_.power;
    for (const VvvfStage& s : list)
        if (hz < s.untilHz) return &s;
    return list.empty() ? nullptr : &list.back();
}

std::string VvvfSynth::stageName() const {
    if (!target_.dir) return "off";
    const VvvfStage* s = stageFor(target_.hz, target_.dir);
    if (!s) return "-";
    switch (s->mode) {
    case VvvfStage::Mode::Async: return std::format("async {:.0f} Hz", carrierHz_);
    case VvvfStage::Mode::Sync: return std::format("sync {}P", s->pulses);
    case VvvfStage::Mode::OnePulse: return "1P";
    }
    return "-";
}

void VvvfSynth::render(std::span<int16_t> out) {
    constexpr int kOversample = 8;
    const double dt = 1.0 / (double(rate_) * kOversample);
    const std::size_t n = out.size();
    // one-pole filters at the output rate
    const double lpA = 1.0 - std::exp(-kTau * std::min(preset_.lowpassHz, 0.45 * rate_) / rate_);
    const double hpA = std::exp(-kTau * std::max(1.0, preset_.highpassHz) / rate_);
    const double envA = 1.0 - std::exp(-1.0 / (0.04 * rate_)); // 40 ms gate / level smoothing
    const double scale = 0.45 * 32767.0 * preset_.volume;
    const int dir = target_.dir ? target_.dir : from_.dir; // keep the last pattern while fading out
    const VvvfStage* stage = nullptr;
    double stageAt = -1.0;

    for (std::size_t i = 0; i < n; ++i) {
        const double t = n > 1 ? double(i) / double(n - 1) : 1.0;
        const double hz = from_.hz + (target_.hz - from_.hz) * t;
        env_ += ((from_.gain + (target_.gain - from_.gain) * t) - env_) * envA;
        if (env_ < 1e-4 && target_.gain == 0.0) { // gate off: silence, keep the phase running
            theta_ = std::fmod(theta_ + kTau * hz / rate_, kTau);
            lp1_ = lp2_ = hpIn_ = hpOut_ = 0.0;
            out[i] = 0;
            continue;
        }
        if (!stage || std::abs(hz - stageAt) > 0.05) stage = stageFor(hz, dir), stageAt = hz;
        if (!stage) {
            out[i] = 0;
            continue;
        }
        const double amp = std::clamp(preset_.minAmplitude + (1.0 - preset_.minAmplitude) * hz / std::max(1.0, preset_.baseHz),
                                      0.0, preset_.maxAmplitude);
        double carrierHz = 0.0;
        if (stage->mode == VvvfStage::Mode::Async) {
            const double span = std::isfinite(stage->untilHz) ? stage->untilHz - stage->fromHz : 0.0;
            const double k = span > 0.0 ? std::clamp((hz - stage->fromHz) / span, 0.0, 1.0) : 0.0;
            carrierHz = stage->carrierFrom + (stage->carrierTo - stage->carrierFrom) * k;
            if (carrierHz_ <= 0.0 || stage->random <= 0.0) carrierHz_ = carrierHz;
        }
        double acc = 0.0;
        for (int k = 0; k < kOversample; ++k) {
            theta_ += kTau * hz * dt;
            if (theta_ >= kTau) theta_ -= kTau;
            double tri = 0.0;
            switch (stage->mode) {
            case VvvfStage::Mode::Async:
                carrier_ += carrierHz_ * dt;
                if (carrier_ >= 1.0) {
                    carrier_ -= std::floor(carrier_);
                    if (stage->random > 0.0) { // new random carrier each period
                        rng_ = rng_ * 1664525u + 1013904223u;
                        carrierHz_ = std::max(50.0, carrierHz + stage->random * (double(rng_ >> 8) / double(1u << 24) * 2.0 - 1.0));
                    } else {
                        carrierHz_ = carrierHz;
                    }
                }
                tri = triangle(carrier_);
                break;
            case VvvfStage::Mode::Sync: tri = triangle(stage->pulses * theta_ / kTau); break;
            case VvvfStage::Mode::OnePulse: tri = 0.0; break;
            }
            const double refU = amp * std::sin(theta_);
            const double refV = amp * std::sin(theta_ - kTau / 3.0);
            const double u = refU > tri ? 1.0 : -1.0;
            const double v = refV > tri ? 1.0 : -1.0;
            acc += 0.5 * (u - v); // line voltage U-V: -1, 0, +1
        }
        double y = acc / kOversample;
        lp1_ += (y - lp1_) * lpA;
        lp2_ += (lp1_ - lp2_) * lpA;
        const double hp = hpA * (hpOut_ + lp2_ - hpIn_);
        hpIn_ = lp2_, hpOut_ = hp;
        y = std::clamp(hp * env_ * scale, -32767.0, 32767.0);
        out[i] = int16_t(std::lround(y));
    }
    from_ = target_;
}

} // namespace ots
