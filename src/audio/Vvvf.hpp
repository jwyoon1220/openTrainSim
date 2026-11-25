#pragma once
// VVVF inverter sound synthesis. A preset (vvvf.json, docs/vvvf.md) describes the inverter's
// modulation pattern; the synthesizer runs the inverter's sine-triangle PWM in real time and
// plays the motor line voltage, which is what makes the familiar whine: asynchronous carrier
// (fixed, ramping or randomised), synchronous N-pulse modes and the final one-pulse mode.
#include "util/Expected.hpp"
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace ots {

struct VvvfStage {
    enum class Mode { Async, Sync, OnePulse };
    Mode mode = Mode::Async;
    double fromHz = 0.0;                                     // fundamental range [fromHz, untilHz)
    double untilHz = std::numeric_limits<double>::infinity();
    double carrierFrom = 1000.0, carrierTo = 1000.0;         // Async: Hz, ramped over the range
    double random = 0.0;                                     // Async: ± Hz per carrier period
    int pulses = 9;                                          // Sync: carrier = pulses × fundamental
};

struct VvvfPreset {
    std::string name;
    // fundamental (Hz) = speed / (π × wheel diameter) × gear ratio × pole pairs ± slip
    double poles = 4.0, gearRatio = 6.0, wheelDiameter = 0.86, slipHz = 1.5;
    double baseHz = 50.0;        // V/f: full voltage (modulation 1) from here
    double minAmplitude = 0.08;  // modulation at 0 Hz (voltage boost)
    double maxAmplitude = 1.25;  // over-modulation limit above baseHz
    double volume = 1.0, radius = 10.0;
    double lowpassHz = 6000.0, highpassHz = 40.0;
    double brakeCutoffKmh = 5.0; // regenerative braking (and its sound) ends below this
    bool replaceMotor = true;    // mute train.dat's motor sound tables
    std::vector<VvvfStage> power, brake;

    [[nodiscard]] double hzPerMps() const;
    // Returns false with "line N: ..." / a field error.
    static Expected<VvvfPreset> load(const std::filesystem::path& file);
};

class VvvfSynth {
public:
    VvvfSynth(VvvfPreset preset, int sampleRate);
    // Control, once per frame (before the stream is refilled). direction: 1 power, -1 brake,
    // 0 inverter off (coasting). torque 0..1 sets the loudness.
    void set(double speedMps, int direction, double torque);
    // Stream callback: the next samples (mono, 16 bit).
    void render(std::span<int16_t> out);

    [[nodiscard]] const VvvfPreset& preset() const { return preset_; }
    [[nodiscard]] double fundamentalHz() const { return target_.hz; }
    [[nodiscard]] std::string stageName() const; // "async 1000 Hz", "sync 9P", "1P", "off"

private:
    struct Params {
        double hz = 0.0, gain = 0.0;
        int dir = 0;
    };
    [[nodiscard]] const VvvfStage* stageFor(double hz, int dir) const;

    VvvfPreset preset_;
    int rate_;
    Params from_, target_;
    double theta_ = 0.0;   // fundamental phase (rad)
    double carrier_ = 0.0; // async carrier phase (cycles)
    double carrierHz_ = 0.0;
    double env_ = 0.0;     // smoothed gain
    double lp1_ = 0.0, lp2_ = 0.0, hpIn_ = 0.0, hpOut_ = 0.0;
    unsigned rng_ = 0x12345678u;
};

} // namespace ots
