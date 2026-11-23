#pragma once
// Sound output with openBVE's model (OpenBVE/Audio/Sounds.Update.cs): every sound is a "source"
// with a buffer, a radius, pitch and volume, attached either to a car of the player train or to a
// fixed world position. Gain is computed per frame from the listener distance:
//   d < 2r : 1 - d²(4r - d) / (16 r³)      else : r / d
// (radius halved for sounds outside the driver car while in the cab), and only the loudest
// `voices` sources get a real OpenAL source; the rest keep playing virtually (their play cursor
// advances) and come back seamlessly when they become audible.
#include "audio/Wav.hpp"

#include <glm/glm.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ots {

struct CarPose;

using SoundBufferId = int;  // -1 = none
using SoundId = unsigned;   // 0 = none

struct SoundAnchor {
    int car = -1;              // -1 = world position
    glm::dvec3 offset{0.0};    // car: (right, up, forward) from the car centre; world: absolute
};

class AudioSystem {
public:
    AudioSystem() = default;
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // Opens the default output device. Without one, everything below is a no-op.
    bool init(int voices = 64);
    [[nodiscard]] bool available() const { return context_ != nullptr; }

    // Decodes and uploads a file (cached by path). Returns -1 if it cannot be loaded.
    SoundBufferId load(const std::filesystem::path& file);
    [[nodiscard]] double duration(SoundBufferId b) const;

    SoundId play(SoundBufferId buffer, const SoundAnchor& at, double radius, double pitch, double volume, bool looped);
    void set(SoundId s, double pitch, double volume);
    void setVolume(SoundId s, double volume);
    void stop(SoundId s);
    [[nodiscard]] bool playing(SoundId s) const;
    void stopAll();

    // Once per frame. `cars` resolves car anchors, `driverCar` keeps its full radius in the cab.
    void update(double dt, const glm::dvec3& listener, const glm::dvec3& right, const glm::dvec3& up, const glm::dvec3& forward,
                std::span<const CarPose> cars, int driverCar, bool interior);

    float masterVolume = 1.0f;
    bool muted = false;
    [[nodiscard]] int activeSounds() const { return int(sounds_.size()); }
    [[nodiscard]] int audibleVoices() const { return audible_; }

private:
    struct Buffer {
        unsigned al = 0;
        double duration = 0.0;
    };
    struct Sound {
        SoundId id = 0;
        SoundBufferId buffer = -1;
        SoundAnchor at;
        double radius = 10.0, pitch = 1.0, volume = 1.0;
        bool looped = false, stopped = false;
        double cursor = 0.0; // seconds into the buffer (virtual playback)
        int voice = -1;      // index into voices_, -1 = virtual
        double gain = 0.0;   // this frame
    };
    Sound* find(SoundId s);
    [[nodiscard]] const Sound* find(SoundId s) const;

    void* device_ = nullptr;
    void* context_ = nullptr;
    std::vector<Buffer> buffers_;
    std::unordered_map<std::string, SoundBufferId> byPath_;
    std::vector<unsigned> voices_;      // OpenAL sources
    std::vector<SoundId> voiceOwner_;   // sound currently using each voice (0 = free)
    std::vector<Sound> sounds_;
    SoundId nextId_ = 1;
    int audible_ = 0;
};

} // namespace ots
