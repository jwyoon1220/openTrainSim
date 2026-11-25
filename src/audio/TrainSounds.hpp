#pragma once
// Player train and route sounds with openBVE behaviour (TrainManager RunSounds / Flange /
// BVEMotorSound / CarBrake / Doors / Horn / handle sounds, NetPlugin sound handles,
// RouteManager2 SoundEvent / PointSoundEvent, station arrival / departure sounds).
#include "audio/SoundCfg.hpp"
#include "audio/Vvvf.hpp"
#include "phx/audio/AudioSystem.hpp"

#include <array>
#include <map>
#include <memory>
#include <vector>

namespace ots {

class Simulation;
using phx::AudioSystem;
using phx::SoundBufferId;
using phx::SoundId;

class TrainSounds {
public:
    TrainSounds(AudioSystem& audio, const Simulation& sim);
    // Once per frame after the simulation step. Consumes the plugin's sound commands.
    void update(Simulation& sim, double dt);
    void hornMusic(); // toggles the music horn (openBVE HORN_MUSIC)
    // Synthesised inverter sound from a preset (audio/Vvvf.hpp), played from the motor car
    // nearest the cab; replaces train.dat's motor sound tables unless the preset says otherwise.
    void setVvvf(const VvvfPreset& preset);
    [[nodiscard]] const VvvfSynth* vvvf() const { return vvvf_.get(); }

private:
    struct Loop {
        SoundId id = 0;
        double target = 0.0;
    };
    struct MotorTrack {
        SoundBufferId buffer = -1;
        SoundId id = 0;
    };
    struct CarState {
        std::map<int, Loop> run, flange;
        double flangePitch = 0.0;
        std::array<MotorTrack, 4> motor; // P1, P2, B1, B2
        int motorDir = 0;
        SoundId noise = 0;
        double frontAxle = 0.0, rearAxle = 0.0; // last track positions (point sounds)
    };

    SoundBufferId buf(const std::filesystem::path& p);
    SoundId playAt(SoundBufferId b, int car, glm::dvec3 offset, double radius, double pitch = 1.0, double volume = 1.0,
                   bool looped = false);
    void updateRun(const Simulation& sim, double dt);
    void updateMotor(const Simulation& sim);
    void updateEvents(Simulation& sim, double dt);
    void updatePlugin(Simulation& sim);
    void updateRoute(const Simulation& sim);

    AudioSystem& audio_;
    TrainSoundFiles files_;
    std::map<int, SoundBufferId> run_, flange_, motor_, switch_, ats_;
    std::vector<CarState> cars_;
    std::vector<char> motorCar_;
    glm::dvec3 panel_{0.0};
    int driverCar_ = 0;
    double maxMotorAcceleration_ = 1.0;
    bool first_ = true;

    // previous frame state for edge-triggered sounds
    int lastPower_ = 0, lastBrake_ = 0, lastReverser_ = 0, lastBrakeActual_ = 0;
    bool lastEmergency_ = false, lastLeft_ = false, lastRight_ = false, lastClosed_ = true, lastBreaker_ = false;
    bool lastHornP_ = false, lastHornS_ = false, lastCompressor_ = false;
    double lastSpeed_ = 0.0, lastFront_ = 0.0;
    SoundId hornLoopP_ = 0, hornLoopS_ = 0, music_ = 0, compressorLoop_ = 0;
    bool musicOn_ = false;

    // plugin sound handles: host id → sound, and the sound currently owning each (car, index)
    std::map<int, SoundId> pluginHandles_;
    std::map<std::pair<int, int>, SoundId> pluginSlots_;
    std::map<std::pair<int, int>, int> pluginSlotHandle_;

    // VVVF synthesis (the stream owns a reference too)
    std::shared_ptr<VvvfSynth> vvvf_;
    SoundId vvvfStream_ = 0;
    // built-in train protection alarm (synthesised in the cab)
    struct AlarmSynth;
    void updateSafety(const Simulation& sim);
    std::shared_ptr<AlarmSynth> alarm_;
    SoundId alarmStream_ = 0;

    // route
    std::size_t nextAnnounce_ = 0;
    std::vector<char> departurePlayed_, arrivedSeen_;
};

} // namespace ots
