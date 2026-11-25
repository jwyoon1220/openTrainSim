#include "phx/audio/AudioSystem.hpp"

#include "phx/core/Log.hpp"
#include "phx/core/Wav.hpp"

#include <AL/al.h>
#include <AL/alc.h>

#include <algorithm>
#include <cmath>

namespace phx {

namespace {
// openBVE distance model; `rel` gets the position in listener space (OpenAL: -z forward).
double anchoredGain(const SoundAnchor& at, double radius, double volume, const AudioListener& l, std::span<const AudioFrame> frames,
                    glm::vec3& rel) {
    glm::dvec3 p = at.offset;
    if (at.frame >= 0 && std::size_t(at.frame) < frames.size()) {
        const AudioFrame& c = frames[std::size_t(at.frame)];
        p = c.position + c.X * at.offset.x + c.Y * at.offset.y + c.Z * at.offset.z;
    }
    const glm::dvec3 d = p - l.position;
    const double dist = glm::length(d);
    double r = radius;
    if (l.insideFrame >= 0 && at.frame != l.insideFrame) r *= 0.5;
    const double g = dist < 2.0 * r ? 1.0 - dist * dist * (4.0 * r - dist) / (16.0 * r * r * r) : r / dist;
    rel = glm::vec3(float(glm::dot(d, l.right)), float(glm::dot(d, l.up)), float(-glm::dot(d, l.forward)));
    return std::max(0.0, g * volume);
}
} // namespace

AudioSystem::~AudioSystem() {
    if (!context_) return;
    for (Stream& st : streams_) destroy(st);
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
        log::warn("audio: no output device, sound disabled");
        return false;
    }
    ALCcontext* ctx = alcCreateContext(dev, nullptr);
    if (!ctx || !alcMakeContextCurrent(ctx)) {
        if (ctx) alcDestroyContext(ctx);
        alcCloseDevice(dev);
        log::warn("audio: cannot create an OpenAL context, sound disabled");
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
    log::info("audio: OpenAL {} ({} voices)", name ? name : "?", voices_.size());
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
        log::warn("audio: {}: {}", file.string(), err.empty() ? "no samples" : err);
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
    else if (Stream* st = findStream(id)) st->volume = volume;
}
void AudioSystem::setAnchor(SoundId id, const SoundAnchor& at) {
    if (Sound* s = find(id)) s->at = at;
    else if (Stream* st = findStream(id)) st->at = at;
}
void AudioSystem::stop(SoundId id) {
    if (Sound* s = find(id)) s->stopped = true;
    else if (Stream* st = findStream(id)) st->stopped = true;
}
bool AudioSystem::playing(SoundId id) const {
    if (const Sound* s = find(id)) return !s->stopped;
    for (const Stream& st : streams_)
        if (st.id == id) return !st.stopped;
    return false;
}
void AudioSystem::stopAll() {
    for (Sound& s : sounds_) s.stopped = true;
    for (Stream& st : streams_) st.stopped = true;
}

AudioSystem::Stream* AudioSystem::findStream(SoundId id) {
    if (!id) return nullptr;
    for (Stream& st : streams_)
        if (st.id == id) return &st;
    return nullptr;
}

SoundId AudioSystem::playStream(int sampleRate, StreamFill fill, const SoundAnchor& at, double radius, double volume, double latency) {
    if (!context_ || !fill || sampleRate <= 0) return 0;
    Stream st;
    alGenSources(1, &st.source);
    if (alGetError() != AL_NO_ERROR) return 0;
    alSourcei(st.source, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourcef(st.source, AL_MAX_GAIN, 8.0f);
    alSourcef(st.source, AL_GAIN, 0.0f);
    // 4 buffers covering `latency`: refilled once a buffer has been played
    constexpr int kBuffers = 4;
    const int perBuffer = std::max(256, int(std::lround(std::clamp(latency, 0.02, 1.0) * sampleRate / kBuffers)));
    st.buffers.resize(kBuffers);
    alGenBuffers(kBuffers, st.buffers.data());
    st.scratch.resize(std::size_t(perBuffer));
    st.sampleRate = sampleRate, st.fill = std::move(fill), st.at = at, st.radius = radius, st.volume = volume;
    st.id = nextId_++;
    if (!nextId_) nextId_ = 1;
    for (ALuint b : st.buffers) {
        st.fill(st.scratch);
        alBufferData(b, AL_FORMAT_MONO16, st.scratch.data(), ALsizei(st.scratch.size() * sizeof(int16_t)), st.sampleRate);
    }
    alSourceQueueBuffers(st.source, ALsizei(st.buffers.size()), st.buffers.data());
    alSourcePlay(st.source);
    streams_.push_back(std::move(st));
    return streams_.back().id;
}

void AudioSystem::refill(Stream& st) {
    ALint done = 0;
    alGetSourcei(st.source, AL_BUFFERS_PROCESSED, &done);
    while (done-- > 0) {
        ALuint b = 0;
        alSourceUnqueueBuffers(st.source, 1, &b);
        st.fill(st.scratch);
        alBufferData(b, AL_FORMAT_MONO16, st.scratch.data(), ALsizei(st.scratch.size() * sizeof(int16_t)), st.sampleRate);
        alSourceQueueBuffers(st.source, 1, &b);
    }
    ALint state = 0;
    alGetSourcei(st.source, AL_SOURCE_STATE, &state);
    if (state != AL_PLAYING) alSourcePlay(st.source); // underrun (a long frame): resume
}

void AudioSystem::destroy(Stream& st) {
    alSourceStop(st.source);
    alSourcei(st.source, AL_BUFFER, 0);
    alDeleteSources(1, &st.source);
    alDeleteBuffers(ALsizei(st.buffers.size()), st.buffers.data());
    st.source = 0;
}

void AudioSystem::update(double dt, const AudioListener& listener, std::span<const AudioFrame> frames) {
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
        s.gain = anchoredGain(s.at, s.radius, s.volume, listener, frames, rel[i]);
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
    // streams: own voices, always running
    for (std::size_t i = 0; i < streams_.size();) {
        Stream& st = streams_[i];
        if (st.stopped) {
            destroy(st);
            streams_[i] = std::move(streams_.back());
            streams_.pop_back();
            continue;
        }
        glm::vec3 r;
        const double g = anchoredGain(st.at, st.radius, st.volume, listener, frames, r);
        alSourcef(st.source, AL_GAIN, float(g) * master);
        alSource3f(st.source, AL_POSITION, r.x, r.y, r.z);
        refill(st);
        if (g > 1e-4) ++audible_;
        ++i;
    }
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

} // namespace phx
