#pragma once
// 3D sound output (OpenAL). Every sound has a buffer, a radius, pitch and volume and is attached
// either to a moving frame (a vehicle: its pose is passed to update() each frame) or to a fixed
// world position. Gain is computed per frame from the listener distance (openBVE's model,
// OpenBVE/Audio/Sounds.Update.cs):
//   d < 2r : 1 - d²(4r - d) / (16 r³)      else : r / d
// with the radius halved for sounds outside the frame the listener is inside of. Only the
// loudest `voices` sounds get a real OpenAL source; the rest keep playing virtually (their play
// cursor advances) and come back seamlessly when they become audible.

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace phx {

using SoundBufferId = int;  // -1 = none
using SoundId = unsigned;   // 0 = none

struct SoundAnchor {
    int frame = -1;            // index into update()'s frames, -1 = world position
    glm::dvec3 offset{0.0};    // frame: (right, up, forward) from its origin; world: absolute
};

// A moving frame sounds can be attached to (e.g. one car of a train).
struct AudioFrame {
    glm::dvec3 position{0.0};
    glm::dvec3 X{1, 0, 0}, Y{0, 1, 0}, Z{0, 0, 1}; // right, up, forward
};

struct AudioListener {
    glm::dvec3 position{0.0};
    glm::dvec3 right{1, 0, 0}, up{0, 1, 0}, forward{0, 0, 1};
    int insideFrame = -1; // the frame the listener is inside of (cab), -1 = outside
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

    // Generated sound (synthesis): `fill` writes the next mono 16-bit samples at `sampleRate`.
    // It is called from update() on the calling thread, keeping about `latency` seconds queued,
    // so whatever it reads may be changed between frames without locking. Streams always own a
    // voice of their own (they are never virtual). stop() / setVolume() apply; pitch does not.
    using StreamFill = std::function<void(std::span<int16_t> samples)>;
    SoundId playStream(int sampleRate, StreamFill fill, const SoundAnchor& at, double radius, double volume, double latency = 0.08);
    // Moves a sound or stream to another anchor.
    void setAnchor(SoundId s, const SoundAnchor& at);

    // Once per frame. `frames` resolves frame anchors.
    void update(double dt, const AudioListener& listener, std::span<const AudioFrame> frames);

    float masterVolume = 1.0f;
    bool muted = false;
    [[nodiscard]] int activeSounds() const { return int(sounds_.size() + streams_.size()); }
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
    struct Stream {
        SoundId id = 0;
        SoundAnchor at;
        double radius = 10.0, volume = 1.0;
        bool stopped = false;
        int sampleRate = 44100;
        unsigned source = 0;
        std::vector<unsigned> buffers; // queued round-robin
        std::vector<int16_t> scratch;  // one buffer of samples
        StreamFill fill;
    };
    Sound* find(SoundId s);
    [[nodiscard]] const Sound* find(SoundId s) const;
    Stream* findStream(SoundId s);
    void refill(Stream& st);
    void destroy(Stream& st);

    void* device_ = nullptr;
    void* context_ = nullptr;
    std::vector<Buffer> buffers_;
    std::unordered_map<std::string, SoundBufferId> byPath_;
    std::vector<unsigned> voices_;      // OpenAL sources
    std::vector<SoundId> voiceOwner_;   // sound currently using each voice (0 = free)
    std::vector<Sound> sounds_;
    std::vector<Stream> streams_;
    SoundId nextId_ = 1;
    int audible_ = 0;
};

} // namespace phx
