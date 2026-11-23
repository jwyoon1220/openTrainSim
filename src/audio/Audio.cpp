#include "audio/Audio.hpp"

#include "phx/core/Log.hpp"
#include "sim/Train.hpp"

#include <AL/al.h>
#include <AL/alc.h>

#include <algorithm>
#include <cmath>

namespace ots {

AudioSystem::~AudioSystem() {
    if (!context_) return;
    for (unsigned v : voices_) {
        alSourceStop(v);
        alDeleteSources(1, &v);
    }
    for (Buffer& b : buffers_)
        if (b.al) alDeleteBuffers(1, &b.al);
    alcMakeContextCurrent(nullptr);
    alcDestroyContext(static_cast<ALCcontext*>(context_));
    alcCloseDevice(static_cast<ALCdevice*>(device_));
}

bool AudioSystem::init(int voices) {
    ALCdevice* dev = alcOpenDevice(nullptr);
    if (!dev) {
        phx::log::warn("audio: no output device, sound disabled");
        return false;
    }
    ALCcontext* ctx = alcCreateContext(dev, nullptr);
    if (!ctx || !alcMakeContextCurrent(ctx)) {
        if (ctx) alcDestroyContext(ctx);
        alcCloseDevice(dev);
        phx::log::warn("audio: cannot create an OpenAL context, sound disabled");
        return false;
    }
    device_ = dev, context_ = ctx;
    alDistanceModel(AL_NONE); // attenuation is computed here (openBVE model)
    alListener3f(AL_POSITION, 0, 0, 0);
    const ALfloat ori[6] = {0, 0, -1, 0, 1, 0};
    alListenerfv(AL_ORIENTATION, ori);
    for (int i = 0; i < voices; ++i) {
        ALuint s = 0;
        alGenSources(1, &s);
        if (alGetError() != AL_NO_ERROR) break;
        alSourcei(s, AL_SOURCE_RELATIVE, AL_TRUE);
        alSourcef(s, AL_MAX_GAIN, 8.0f);
        voices_.push_back(s);
    }
    voiceOwner_.assign(voices_.size(), 0);
    const char* name = alcGetString(dev, ALC_DEVICE_SPECIFIER);
    phx::log::info("audio: OpenAL {} ({} voices)", name ? name : "?", voices_.size());
    return true;
}

SoundBufferId AudioSystem::load(const std::filesystem::path& file) {
    if (!context_ || file.empty()) return -1;
    const std::string key = file.string();
    if (auto it = byPath_.find(key); it != byPath_.end()) return it->second;
    PcmMono16 pcm;
    std::string err;
    SoundBufferId id = -1;
    if (!loadWav(file, pcm, err) || pcm.samples.empty() || pcm.sampleRate <= 0) {
        phx::log::warn("audio: {}: {}", file.string(), err.empty() ? "no samples" : err);
    } else {
        Buffer b;
        alGenBuffers(1, &b.al);
        alBufferData(b.al, AL_FORMAT_MONO16, pcm.samples.data(), ALsizei(pcm.samples.size() * sizeof(int16_t)), pcm.sampleRate);
        if (alGetError() == AL_NO_ERROR) {
            b.duration = pcm.duration();
            id = SoundBufferId(buffers_.size());
            buffers_.push_back(b);
        } else {
            alDeleteBuffers(1, &b.al);
        }
    }
    byPath_[key] = id;
    return id;
}

double AudioSystem::duration(SoundBufferId b) const { return b >= 0 && std::size_t(b) < buffers_.size() ? buffers_[std::size_t(b)].duration : 0.0; }

AudioSystem::Sound* AudioSystem::find(SoundId s) {
    if (!s) return nullptr;
    for (Sound& x : sounds_)
        if (x.id == s) return &x;
    return nullptr;
}
const AudioSystem::Sound* AudioSystem::find(SoundId s) const { return const_cast<AudioSystem*>(this)->find(s); }

SoundId AudioSystem::play(SoundBufferId buffer, const SoundAnchor& at, double radius, double pitch, double volume, bool looped) {
    if (!context_ || buffer < 0 || std::size_t(buffer) >= buffers_.size()) return 0;
    Sound s;
    s.id = nextId_++;
    if (!nextId_) nextId_ = 1;
    s.buffer = buffer, s.at = at, s.radius = radius, s.pitch = pitch, s.volume = volume, s.looped = looped;
    sounds_.push_back(s);
    return s.id;
}

void AudioSystem::set(SoundId id, double pitch, double volume) {
    if (Sound* s = find(id)) s->pitch = pitch, s->volume = volume;
}
void AudioSystem::setVolume(SoundId id, double volume) {
    if (Sound* s = find(id)) s->volume = volume;
}
void AudioSystem::stop(SoundId id) {
    if (Sound* s = find(id)) s->stopped = true;
}
bool AudioSystem::playing(SoundId id) const {
    const Sound* s = find(id);
    return s && !s->stopped;
}
void AudioSystem::stopAll() {
    for (Sound& s : sounds_) s.stopped = true;
}

void AudioSystem::update(double dt, const glm::dvec3& L, const glm::dvec3& R, const glm::dvec3& U, const glm::dvec3& F,
                         std::span<const CarPose> cars, int driverCar, bool interior) {
    if (!context_) return;
    // 1. advance cursors, drop finished sounds, compute gains
    std::vector<glm::vec3> rel(sounds_.size());
    for (std::size_t i = 0; i < sounds_.size(); ++i) {
        Sound& s = sounds_[i];
        const double len = buffers_[std::size_t(s.buffer)].duration;
        if (s.voice >= 0) { // the real source tells where it is
            ALint state = 0;
            alGetSourcei(voices_[std::size_t(s.voice)], AL_SOURCE_STATE, &state);
            if (state == AL_STOPPED && !s.looped) s.stopped = true;
            ALfloat off = 0;
            alGetSourcef(voices_[std::size_t(s.voice)], AL_SEC_OFFSET, &off);
            s.cursor = off;
        } else {
            s.cursor += dt * std::clamp(s.pitch, 0.01, 8.0);
            if (s.cursor >= len) {
                if (s.looped && len > 0) s.cursor = std::fmod(s.cursor, len);
                else s.stopped = true;
            }
        }
        glm::dvec3 p = s.at.offset;
        int carIndex = s.at.car;
        if (carIndex >= 0 && std::size_t(carIndex) < cars.size()) {
            const CarPose& c = cars[std::size_t(carIndex)];
            p = c.position + c.X * s.at.offset.x + c.Y * s.at.offset.y + c.Z * s.at.offset.z;
        }
        const glm::dvec3 d = p - L;
        const double dist = glm::length(d);
        double r = s.radius;
        if (interior && carIndex != driverCar) r *= 0.5;
        double g = dist < 2.0 * r ? 1.0 - dist * dist * (4.0 * r - dist) / (16.0 * r * r * r) : r / dist;
        s.gain = std::max(0.0, g * s.volume);
        rel[i] = glm::vec3(float(glm::dot(d, R)), float(glm::dot(d, U)), float(-glm::dot(d, F)));
    }
    // release voices of stopped sounds, erase them
    for (std::size_t i = 0; i < sounds_.size();) {
        if (sounds_[i].stopped) {
            if (sounds_[i].voice >= 0) {
                alSourceStop(voices_[std::size_t(sounds_[i].voice)]);
                alSourcei(voices_[std::size_t(sounds_[i].voice)], AL_BUFFER, 0);
                voiceOwner_[std::size_t(sounds_[i].voice)] = 0;
            }
            sounds_[i] = sounds_.back();
            rel[i] = rel.back();
            sounds_.pop_back();
            rel.pop_back();
        } else {
            ++i;
        }
    }
    // 2. the loudest sounds get real voices
    std::vector<std::size_t> order(sounds_.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    const std::size_t n = std::min(voices_.size(), order.size());
    std::partial_sort(order.begin(), order.begin() + std::ptrdiff_t(n), order.end(),
                      [&](std::size_t a, std::size_t b) { return sounds_[a].gain > sounds_[b].gain; });
    std::vector<char> keep(sounds_.size(), 0);
    for (std::size_t k = 0; k < n; ++k)
        if (sounds_[order[k]].gain > 1e-4) keep[order[k]] = 1;
    for (std::size_t i = 0; i < sounds_.size(); ++i) { // demote
        Sound& s = sounds_[i];
        if (s.voice >= 0 && !keep[i]) {
            alSourceStop(voices_[std::size_t(s.voice)]);
            alSourcei(voices_[std::size_t(s.voice)], AL_BUFFER, 0);
            voiceOwner_[std::size_t(s.voice)] = 0;
            s.voice = -1;
        }
    }
    audible_ = 0;
    const float master = muted ? 0.0f : masterVolume;
    for (std::size_t i = 0; i < sounds_.size(); ++i) {
        Sound& s = sounds_[i];
        if (!keep[i]) continue;
        ++audible_;
        bool start = false;
        if (s.voice < 0) {
            for (std::size_t v = 0; v < voices_.size(); ++v)
                if (!voiceOwner_[v]) {
                    s.voice = int(v);
                    voiceOwner_[v] = s.id;
                    break;
                }
            if (s.voice < 0) continue;
            const ALuint src = voices_[std::size_t(s.voice)];
            alSourcei(src, AL_BUFFER, ALint(buffers_[std::size_t(s.buffer)].al));
            alSourcei(src, AL_LOOPING, s.looped ? AL_TRUE : AL_FALSE);
            alSourcef(src, AL_SEC_OFFSET, float(s.cursor));
            start = true;
        }
        const ALuint src = voices_[std::size_t(s.voice)];
        alSourcef(src, AL_GAIN, float(s.gain) * master);
        alSourcef(src, AL_PITCH, float(std::clamp(s.pitch, 0.01, 8.0)));
        alSource3f(src, AL_POSITION, rel[i].x, rel[i].y, rel[i].z);
        if (start) alSourcePlay(src);
    }
}

} // namespace ots
