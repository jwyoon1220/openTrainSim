#pragma once
// The game's sound: the player train's and route's sounds (TrainSounds, openBVE rules) and
// one-shot requests from other systems (3D cab clicks), played through the engine's audio output
// (phx::AudioSystem). The game only starts it, forwards a few commands and updates it per frame.
#include <glm/vec3.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace phx {
class AudioSystem;
struct AudioFrame;
} // namespace phx

namespace ots {

class TrainSounds;
class Simulation;

struct SoundSettings {
    bool enabled = true;
    int voices = 64;      // 8..256
    double volume = 1.0;  // master, 0..4
    // VVVF preset (audio/Vvvf.hpp): empty = <train folder>/vvvf.json if present, "none" = off.
    std::filesystem::path vvvf;
};

// Listener (camera) in world coordinates.
struct SoundListener {
    glm::dvec3 position{0.0}, right{1, 0, 0}, up{0, 1, 0}, forward{0, 0, 1};
    bool interior = true;
};

class SoundManager {
public:
    SoundManager();
    ~SoundManager();
    SoundManager(const SoundManager&) = delete;
    SoundManager& operator=(const SoundManager&) = delete;

    // Opens the audio device and loads the player train's sounds. False = no sound (the other
    // calls are then no-ops).
    bool start(const SoundSettings& settings, const Simulation& sim);
    [[nodiscard]] bool available() const;

    void hornMusic();  // openBVE HORN_MUSIC (toggle)
    bool toggleMute(); // returns the new muted state
    // A one-shot sound attached to a car (car coordinates), e.g. a cab switch click.
    void playOnCar(const std::filesystem::path& file, int car, const glm::dvec3& position, double pitch = 1.0, double volume = 1.0,
                   double radius = 2.0);

    // Once per frame after the simulation step. `dt` is real time; the train sounds advance by
    // simulation time (paused / time factor).
    void update(Simulation& sim, double dt, const SoundListener& listener);

    // "sound N playing, M audible[, VVVF ...]" for the HUD; empty without sound.
    [[nodiscard]] std::string status() const;

private:
    std::unique_ptr<phx::AudioSystem> audio_;
    std::unique_ptr<TrainSounds> train_;
    int driverCar_ = 0;
    std::vector<phx::AudioFrame> frames_; // car poses, per frame
};

} // namespace ots
