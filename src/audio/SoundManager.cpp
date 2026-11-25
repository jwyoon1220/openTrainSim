#include "audio/SoundManager.hpp"

#include "audio/TrainSounds.hpp"
#include "phx/audio/AudioSystem.hpp"
#include "phx/core/Log.hpp"
#include "sim/World.hpp"
#include "util/Paths.hpp"

#include <algorithm>
#include <format>

namespace ots {

SoundManager::SoundManager() = default;
SoundManager::~SoundManager() = default; // TrainSounds before AudioSystem (member order)

bool SoundManager::start(const SoundSettings& s, const Simulation& sim) {
    train_.reset();
    audio_.reset();
    if (!s.enabled) return false;
    auto audio = std::make_unique<phx::AudioSystem>();
    if (!audio->init(std::clamp(s.voices, 8, 256))) return false;
    audio->masterVolume = float(std::clamp(s.volume, 0.0, 4.0));
    audio_ = std::move(audio);
    train_ = std::make_unique<TrainSounds>(*audio_, sim);
    const TrainDef& def = sim.train->def();
    driverCar_ = std::clamp(def.spec.driverCar, 0, std::max(0, int(def.cars.size()) - 1));
    std::filesystem::path vvvf = s.vvvf;
    if (vvvf.empty()) vvvf = resolvePath(def.folder, "vvvf.json");
    if (!vvvf.empty() && vvvf != "none") {
        if (auto preset = VvvfPreset::load(vvvf)) train_->setVvvf(std::move(*preset));
        else phx::log::warn("sound: VVVF preset {}: {}", pathString(vvvf), preset.error());
    }
    return true;
}

bool SoundManager::available() const { return audio_ && audio_->available(); }

void SoundManager::hornMusic() {
    if (train_) train_->hornMusic();
}

bool SoundManager::toggleMute() {
    if (!audio_) return false;
    audio_->muted = !audio_->muted;
    return audio_->muted;
}

void SoundManager::playOnCar(const std::filesystem::path& file, int car, const glm::dvec3& position, double pitch, double volume,
                             double radius) {
    if (!available()) return;
    audio_->play(audio_->load(file), phx::SoundAnchor{car, position}, radius, pitch, volume, false);
}

void SoundManager::update(Simulation& sim, double dt, const SoundListener& l) {
    if (!available() || !train_) return;
    train_->update(sim, sim.paused ? 0.0 : dt * sim.timeFactor);
    const auto& cars = sim.train->cars;
    frames_.resize(cars.size());
    for (std::size_t i = 0; i < cars.size(); ++i) frames_[i] = {cars[i].position, cars[i].X, cars[i].Y, cars[i].Z};
    audio_->update(dt, {l.position, l.right, l.up, l.forward, l.interior ? driverCar_ : -1}, frames_);
}

std::string SoundManager::status() const {
    if (!available()) return {};
    std::string s = std::format("sound {} playing, {} audible{}", audio_->activeSounds(), audio_->audibleVoices(), audio_->muted ? " (muted)" : "");
    if (const VvvfSynth* v = train_ ? train_->vvvf() : nullptr) s += std::format(" | VVVF {:.1f} Hz {}", v->fundamentalHz(), v->stageName());
    return s;
}

} // namespace ots
