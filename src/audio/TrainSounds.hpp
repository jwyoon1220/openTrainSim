#pragma once
// Player train and route sounds with openBVE behaviour (TrainManager RunSounds / Flange /
// BVEMotorSound / CarBrake / Doors / Horn / handle sounds, NetPlugin sound handles,
// RouteManager2 SoundEvent / PointSoundEvent, station arrival / departure sounds).
#include "audio/Audio.hpp"
#include "audio/SoundCfg.hpp"

#include <map>
#include <vector>

namespace ots {

class Simulation;

class TrainSounds {
public:
    TrainSounds(AudioSystem& audio, const Simulation& sim);
    // Once per frame after the simulation step. Consumes the plugin's sound commands.
    void update(Simulation& sim, double dt);
    void hornMusic(); // toggles the music horn (openBVE HORN_MUSIC)

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
        MotorTrack motor[4]; // P1, P2, B1, B2
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

    // route
    std::size_t nextAnnounce_ = 0;
    std::vector<char> departurePlayed_, arrivedSeen_;
};

} // namespace ots
