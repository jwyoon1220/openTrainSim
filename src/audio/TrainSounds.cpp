#include "audio/TrainSounds.hpp"

#include "phx/core/Log.hpp"
#include "sim/World.hpp"

#include <algorithm>
#include <cmath>

namespace ots {

namespace {
// openBVE SoundCfgParser radii
constexpr double kLarge = 30.0, kMedium = 10.0, kSmall = 5.0, kTiny = 2.0;
} // namespace

TrainSounds::TrainSounds(AudioSystem& audio, const Simulation& sim) : audio_(audio) {
    const TrainDef& def = sim.train->def();
    if (!loadTrainSounds(def.folder, files_)) phx::log::info("sound: no sound.cfg / default sounds in {}", def.folder.string());
    for (const std::string& w : files_.warnings) phx::log::warn("sound: {}", w);
    for (auto& [k, p] : files_.run) run_[k] = buf(p);
    for (auto& [k, p] : files_.flange) flange_[k] = buf(p);
    for (auto& [k, p] : files_.motor) motor_[k] = buf(p);
    for (auto& [k, p] : files_.pointSwitch) switch_[k] = buf(p);
    for (auto& [k, p] : files_.ats) ats_[k] = buf(p);
    cars_.resize(def.cars.size());
    motorCar_.resize(def.cars.size());
    for (std::size_t i = 0; i < def.cars.size(); ++i) motorCar_[i] = def.cars[i].motor;
    driverCar_ = std::clamp(def.spec.driverCar, 0, int(def.cars.size()) - 1);
    if (!def.cars.empty()) panel_ = def.cars[std::size_t(driverCar_)].driver + glm::dvec3(0, 0, 1.0);
    maxMotorAcceleration_ = 0.0;
    for (const AccelerationCurve& c : def.spec.power) maxMotorAcceleration_ = std::max(maxMotorAcceleration_, c.at(0.0));
    if (maxMotorAcceleration_ <= 0.0) maxMotorAcceleration_ = 1.0;
    departurePlayed_.assign(sim.route.stations.size(), 0);
    phx::log::info("sound: {} run, {} motor, {} ats sounds", run_.size(), motor_.size(), ats_.size());
}

SoundBufferId TrainSounds::buf(const std::filesystem::path& p) { return p.empty() ? -1 : audio_.load(p); }

SoundId TrainSounds::playAt(SoundBufferId b, int car, glm::dvec3 offset, double radius, double pitch, double volume, bool looped) {
    if (b < 0) return 0;
    return audio_.play(b, {car, offset}, radius, pitch, volume, looped);
}

void TrainSounds::hornMusic() {
    if (musicOn_) {
        audio_.stop(music_);
        music_ = 0;
        musicOn_ = false;
        if (!files_.hornMusicEnd.empty()) playAt(buf(files_.hornMusicEnd), driverCar_, {0, 0, 0}, kMedium);
        return;
    }
    musicOn_ = true;
    const auto& loop = !files_.hornMusicLoop.empty() ? files_.hornMusicLoop : files_.hornMusic;
    if (!files_.hornMusicStart.empty()) playAt(buf(files_.hornMusicStart), driverCar_, {0, 0, 0}, kMedium);
    music_ = playAt(buf(loop), driverCar_, {0, 0, 0}, kMedium, 1.0, 1.0, true);
}

void TrainSounds::update(Simulation& sim, double dt) {
    if (!audio_.available() || cars_.size() != sim.train->cars.size()) return;
    if (first_) { // continuous car noise (openBVE [Others] Noise, played from the start)
        const SoundBufferId n = buf(files_.noise);
        for (std::size_t i = 0; i < cars_.size(); ++i) cars_[i].noise = playAt(n, int(i), {0, 0, 0}, kMedium, 1.0, 1.0, true);
        for (std::size_t i = 0; i < cars_.size(); ++i)
            cars_[i].frontAxle = sim.train->cars[i].frontAxle, cars_[i].rearAxle = sim.train->cars[i].rearAxle;
        lastFront_ = sim.train->front;
        // skip announcements behind the start
        while (nextAnnounce_ < sim.route.announcements.size() && sim.route.announcements[nextAnnounce_].trackPos <= sim.train->front)
            ++nextAnnounce_;
    }
    updateRun(sim, dt);
    updateMotor(sim);
    updateEvents(sim, dt);
    updatePlugin(sim);
    updateRoute(sim);
    first_ = false;
}

// openBVE RunSounds.Update / Flange.Update
void TrainSounds::updateRun(const Simulation& sim, double dt) {
    const TrainSim& t = *sim.train;
    const double spd = std::abs(t.speed);
    const double pitch = spd * 0.04;
    const double baseGain = spd < 2.77777777777778 ? 0.36 * spd : 1.0;
    const double flangeTarget = 0.5 + 0.04 * spd;
    for (std::size_t c = 0; c < cars_.size(); ++c) {
        CarState& cs = cars_[c];
        const CarPose& pose = t.cars[c];
        const int blk = sim.route.blockAt(pose.frontAxle);
        const RouteBlock* rb = blk >= 0 && std::size_t(blk) < sim.route.blocks.size() ? &sim.route.blocks[std::size_t(blk)] : nullptr;
        const int runIndex = rb ? rb->runSound : 0;
        const int flangeIndex = rb ? rb->flangeSound : 0;
        for (auto& [key, b] : run_) {
            Loop& l = cs.run[key];
            l.target = std::clamp(l.target + (key == runIndex ? 3.0 : -3.0) * dt, 0.0, 1.0);
            const double gain = baseGain * l.target;
            if (audio_.playing(l.id)) {
                if (pitch > 0.01 && gain > 0.001) audio_.set(l.id, pitch, gain);
                else audio_.stop(l.id), l.id = 0;
            } else if (pitch > 0.02 && gain > 0.01) {
                l.id = playAt(b, int(c), {0, 0, 0}, kMedium, pitch, gain, true);
            }
        }
        if (flange_.empty()) continue;
        // angle between the axle chord and the rails ≈ wheelbase / radius
        const double radius = std::abs(sim.route.poseAt(0.5 * (pose.frontAxle + pose.rearAxle)).curveRadius);
        const double b2 = radius > 1.0 ? std::abs(pose.frontAxle - pose.rearAxle) / radius : 0.0;
        const double flangeGain = std::clamp(0.5 * b2 * b2 * spd * spd, 0.0, 0.75);
        if (cs.flangePitch < flangeTarget) cs.flangePitch = std::min(flangeTarget, cs.flangePitch + dt);
        else cs.flangePitch = std::max(flangeTarget, cs.flangePitch - dt);
        for (auto& [key, b] : flange_) {
            Loop& l = cs.flange[key];
            l.target = std::clamp(l.target + (key == flangeIndex ? dt : -dt), 0.0, 1.0);
            const double gain = flangeGain * l.target;
            if (audio_.playing(l.id)) {
                if (cs.flangePitch > 0.01 && gain > 0.0001) audio_.set(l.id, cs.flangePitch, gain);
                else audio_.stop(l.id), l.id = 0;
            } else if (cs.flangePitch > 0.02 && gain > 0.01) {
                l.id = playAt(b, int(c), {0, 0, 0}, kMedium, cs.flangePitch, gain, true);
            }
        }
    }
}

// openBVE BVEMotorSound.Update (train.dat motor sound tables, 18 rows per m/s)
void TrainSounds::updateMotor(const Simulation& sim) {
    const TrainSim& t = *sim.train;
    const TrainSpec& spec = t.def().spec;
    const double spd = std::abs(t.speed);
    const int idx = int(std::lround(spd * 18.0));
    int ndir = 0;
    double ratio = 0.0;
    if (t.motorAcceleration > 1e-4) {
        ndir = 1;
        ratio = t.motorAcceleration / maxMotorAcceleration_;
    } else if (t.brakeCylinder > 1.0 && spd > 0.01 && t.powerActual == 0) {
        ndir = -1;
        ratio = t.brakeCylinder / std::max(1.0, spec.bcServiceMax);
    }
    ratio = std::pow(std::clamp(ratio, 0.0, 1.0), 0.25);
    for (std::size_t c = 0; c < cars_.size(); ++c) {
        if (!motorCar_[c]) continue;
        CarState& cs = cars_[c];
        const int odir = cs.motorDir;
        for (int h = 0; h < 2; ++h) {
            MotorTrack& p = cs.motor[h];
            MotorTrack& b = cs.motor[h + 2];
            if (odir > 0 && ndir <= 0) audio_.stop(p.id), p = {};
            else if (odir < 0 && ndir >= 0) audio_.stop(b.id), b = {};
            if (ndir == 0) continue;
            const int table = ndir > 0 ? h : h + 2;
            MotorTrack& tr = cs.motor[table];
            const auto& entries = spec.motorSound[std::size_t(table)];
            if (entries.empty()) continue;
            const MotorSoundEntry& e = entries[std::size_t(std::min(idx, int(entries.size()) - 1))];
            auto it = e.sound >= 0 ? motor_.find(e.sound) : motor_.end();
            const SoundBufferId nb = it != motor_.end() ? it->second : -1;
            const double gain = double(e.gain) * ratio;
            if (nb != tr.buffer) {
                audio_.stop(tr.id);
                tr.buffer = nb;
                tr.id = nb >= 0 ? playAt(nb, int(c), {0, 0, 0}, kMedium, e.pitch, gain, true) : 0;
            } else if (tr.id) {
                audio_.set(tr.id, e.pitch, gain);
            }
        }
        cs.motorDir = ndir;
    }
}

void TrainSounds::updateEvents(Simulation& sim, double dt) {
    (void)dt;
    const TrainSim& t = *sim.train;
    const TrainDef& def = t.def();
    const int nCars = int(cars_.size());
    const glm::dvec3 centre(0.0);
    if (first_) {
        lastPower_ = t.powerNotch, lastBrake_ = t.brakeNotch, lastReverser_ = t.reverser, lastBrakeActual_ = t.brakeActual;
        lastEmergency_ = t.effectiveEmergency(), lastLeft_ = t.leftDoorsTarget, lastRight_ = t.rightDoorsTarget;
        lastClosed_ = t.doorsClosed(), lastBreaker_ = t.powerActual > 0, lastCompressor_ = t.compressor, lastSpeed_ = t.speed;
        return;
    }
    // handles (panel, tiny radius)
    if (t.brakeNotch != lastBrake_) {
        const auto& f = t.brakeNotch > lastBrake_ ? (t.brakeNotch >= t.maxBrake() && !files_.brakeMax.empty() ? files_.brakeMax : files_.brakeApply)
                                                  : (t.brakeNotch == 0 && !files_.brakeMin.empty() ? files_.brakeMin : files_.brakeRelease);
        playAt(buf(f), driverCar_, panel_, kTiny);
        lastBrake_ = t.brakeNotch;
    }
    if (t.powerNotch != lastPower_) {
        const auto& f = t.powerNotch > lastPower_ ? (t.powerNotch >= t.maxPower() && !files_.powerMax.empty() ? files_.powerMax : files_.powerUp)
                                                  : (t.powerNotch == 0 && !files_.powerMin.empty() ? files_.powerMin : files_.powerDown);
        playAt(buf(f), driverCar_, panel_, kTiny);
        lastPower_ = t.powerNotch;
    }
    if (t.reverser != lastReverser_) {
        playAt(buf(t.reverser != 0 ? files_.reverserOn : files_.reverserOff), driverCar_, panel_, kTiny);
        lastReverser_ = t.reverser;
    }
    // breaker (openBVE Breaker.Update: power applied / removed)
    if (const bool on = t.powerActual > 0; on != lastBreaker_) {
        playAt(buf(on ? files_.breakerOn : files_.breakerOff), driverCar_, panel_, kSmall);
        lastBreaker_ = on;
    }
    // emergency brake
    if (const bool eb = t.effectiveEmergency(); eb != lastEmergency_) {
        playAt(buf(eb ? files_.emergency : files_.emergencyRelease), driverCar_, centre, kMedium);
        lastEmergency_ = eb;
    }
    // brake cylinder release: air sounds in every car
    if (t.brakeActual < lastBrakeActual_) {
        const auto& f = t.brakeActual == 0 && !files_.bcReleaseFull.empty() ? files_.bcReleaseFull
                        : lastBrakeActual_ > t.maxBrake() && !files_.bcReleaseHigh.empty() ? files_.bcReleaseHigh
                        : !files_.bcRelease.empty() ? files_.bcRelease : files_.bcReleaseHigh;
        const SoundBufferId b = buf(f);
        for (int c = 0; c < nCars; ++c) playAt(b, c, centre, kSmall);
    }
    lastBrakeActual_ = t.brakeActual;
    // brake shoe rubbing as the train comes to a stop
    if (std::abs(lastSpeed_) >= 1.4 && std::abs(t.speed) < 1.4 && t.brakeCylinder > 1.0) {
        const SoundBufferId b = buf(files_.shoe);
        for (int c = 0; c < nCars; ++c) playAt(b, c, centre, kMedium);
    }
    // doors
    auto doors = [&](bool now, bool& last, double side, const std::filesystem::path& open, const std::filesystem::path& close) {
        if (now == last) return;
        const SoundBufferId b = buf(now ? open : close);
        for (int c = 0; c < nCars; ++c) playAt(b, c, {side, 0, 0}, kSmall);
        last = now;
    };
    doors(t.leftDoorsTarget, lastLeft_, -1.3, files_.doorOpenLeft, files_.doorCloseLeft);
    doors(t.rightDoorsTarget, lastRight_, 1.3, files_.doorOpenRight, files_.doorCloseRight);
    if (const bool closed = t.doorsClosed(); closed != lastClosed_) {
        playAt(buf(closed ? files_.pilotLampOn : files_.pilotLampOff), driverCar_, panel_, kTiny);
        lastClosed_ = closed;
    }
    // horns (front of the driver car)
    const glm::dvec3 front(0.0, 0.0, 0.5 * def.cars[std::size_t(driverCar_)].length);
    auto horn = [&](bool now, bool& last, SoundId& loop, const std::filesystem::path& single, const std::filesystem::path& start,
                    const std::filesystem::path& looped, const std::filesystem::path& end) {
        if (now == last) return;
        last = now;
        if (now) {
            if (!looped.empty()) {
                if (!start.empty()) playAt(buf(start), driverCar_, front, kLarge);
                loop = playAt(buf(looped), driverCar_, front, kLarge, 1.0, 1.0, true);
            } else {
                playAt(buf(!single.empty() ? single : start), driverCar_, front, kLarge);
            }
        } else if (loop) {
            audio_.stop(loop);
            loop = 0;
            if (!end.empty()) playAt(buf(end), driverCar_, front, kLarge);
        }
    };
    horn(t.hornPrimary, lastHornP_, hornLoopP_, files_.hornPrimary, files_.hornPrimaryStart, files_.hornPrimaryLoop, files_.hornPrimaryEnd);
    horn(t.hornSecondary, lastHornS_, hornLoopS_, files_.hornSecondary, files_.hornSecondaryStart, files_.hornSecondaryLoop,
         files_.hornSecondaryEnd);
    // compressor (first motor car, else the driver car)
    if (t.compressor != lastCompressor_) {
        int car = driverCar_;
        for (int c = 0; c < nCars; ++c)
            if (motorCar_[std::size_t(c)]) {
                car = c;
                break;
            }
        if (t.compressor) {
            playAt(buf(files_.compressorAttack), car, centre, kMedium);
            compressorLoop_ = playAt(buf(files_.compressorLoop), car, centre, kMedium, 1.0, 1.0, true);
        } else {
            audio_.stop(compressorLoop_);
            compressorLoop_ = 0;
            playAt(buf(files_.compressorRelease), car, centre, kMedium);
        }
        lastCompressor_ = t.compressor;
    }
    // point (switch) sounds: each axle crossing a block start flagged by the route
    const double spd = std::abs(t.speed);
    const double pitch = spd / 12.5, gain = pitch < 0.5 ? 2.0 * pitch : 1.0;
    const double L = sim.route.blockLength;
    for (int c = 0; c < nCars; ++c) {
        CarState& cs = cars_[std::size_t(c)];
        const CarPose& pose = t.cars[std::size_t(c)];
        const CarDef& cd = def.cars[std::size_t(c)];
        auto axle = [&](double& last, double now, double z) {
            const double lo = std::min(last, now), hi = std::max(last, now);
            last = now;
            if (hi - lo <= 0.0 || hi - lo > 100.0 || switch_.empty() || pitch <= 0.2 || gain <= 0.2) return;
            for (double b = std::ceil(lo / L) * L; b <= hi; b += L) {
                const int bi = int(std::lround(b / L));
                if (bi < 0 || std::size_t(bi) >= sim.route.blocks.size() || !sim.route.blocks[std::size_t(bi)].pointSound) continue;
                auto it = switch_.find(sim.route.blocks[std::size_t(bi)].runSound);
                if (it == switch_.end()) it = switch_.find(0);
                if (it != switch_.end()) playAt(it->second, c, {0, 0, z}, kSmall, pitch, gain);
            }
        };
        axle(cs.frontAxle, pose.frontAxle, cd.frontAxle);
        axle(cs.rearAxle, pose.rearAxle, cd.rearAxle);
    }
    lastSpeed_ = t.speed;
}

// OpenBveApi PlaySound / PlayCarSound handles (NetPlugin semantics: a looped index plays once
// per car and is updated in place; one-shots overlap)
void TrainSounds::updatePlugin(Simulation& sim) {
    if (!sim.plugin) return;
    TrainPlugin& pl = *sim.plugin;
    for (const PluginSoundCommand& cmd : pl.sounds) {
        switch (cmd.kind) {
        case PluginSoundCommand::Kind::Play: {
            auto b = ats_.find(cmd.index);
            if (b == ats_.end() || b->second < 0) {
                pl.soundEnded(cmd.handle);
                break;
            }
            const int car = cmd.car >= 0 && cmd.car < int(cars_.size()) ? cmd.car : driverCar_;
            const glm::dvec3 at = cmd.car < 0 ? panel_ : glm::dvec3(0.0);
            const double radius = cmd.car < 0 ? kTiny : kMedium;
            const auto slot = std::make_pair(cmd.car, cmd.index);
            SoundId id = 0;
            if (cmd.looped) {
                auto s = pluginSlots_.find(slot);
                if (s != pluginSlots_.end() && audio_.playing(s->second)) {
                    id = s->second;
                    audio_.set(id, std::max(0.01, cmd.pitch), std::max(0.0, cmd.volume));
                }
            }
            if (!id) id = playAt(b->second, car, at, radius, std::max(0.01, cmd.pitch), std::max(0.0, cmd.volume), cmd.looped);
            // a plugin re-playing a looped index every frame would pile up handles: the newest
            // handle owns the sound, older ones on the same slot report finished
            if (auto prev = pluginSlotHandle_.find(slot); prev != pluginSlotHandle_.end() && prev->second != cmd.handle) {
                if (pluginHandles_.erase(prev->second)) pl.soundEnded(prev->second);
            }
            pluginSlots_[slot] = id;
            pluginSlotHandle_[slot] = cmd.handle;
            pluginHandles_[cmd.handle] = id;
            break;
        }
        case PluginSoundCommand::Kind::Set:
            if (auto h = pluginHandles_.find(cmd.handle); h != pluginHandles_.end())
                audio_.set(h->second, std::max(0.01, cmd.pitch), std::max(0.0, cmd.volume));
            break;
        case PluginSoundCommand::Kind::Stop:
            if (auto h = pluginHandles_.find(cmd.handle); h != pluginHandles_.end()) {
                audio_.stop(h->second);
                pluginHandles_.erase(h);
            }
            break;
        }
    }
    pl.sounds.clear();
    for (auto it = pluginHandles_.begin(); it != pluginHandles_.end();) {
        if (!audio_.playing(it->second)) {
            pl.soundEnded(it->first);
            it = pluginHandles_.erase(it);
        } else {
            ++it;
        }
    }
}

void TrainSounds::updateRoute(const Simulation& sim) {
    const TrainSim& t = *sim.train;
    // Track.Announce (front axle of the first car, once)
    const auto& ann = sim.route.announcements;
    while (nextAnnounce_ < ann.size() && ann[nextAnnounce_].trackPos <= t.front) {
        const RouteSound& rs = ann[nextAnnounce_++];
        if (rs.trackPos < lastFront_) continue; // jumped / reversed past it
        double pitch = 1.0, gain = 1.0;
        if (rs.speed > 0.0) {
            pitch = std::abs(t.speed) / rs.speed;
            gain = pitch < 0.5 ? 2.0 * pitch : 1.0;
            if (pitch < 0.2 || gain < 0.2) continue;
        }
        playAt(buf(rs.file), 0, {0, 0, 0}, 15.0, pitch, gain);
    }
    // reversing: rewind
    while (nextAnnounce_ > 0 && ann[nextAnnounce_ - 1].trackPos > t.front) --nextAnnounce_;
    lastFront_ = t.front;
    // preload the next few announcements
    for (std::size_t i = nextAnnounce_; i < ann.size() && ann[i].trackPos < t.front + 1500.0; ++i) buf(ann[i].file);
    // stations: arrival sound when the train stops, departure sound timed to end at departure
    std::vector<char>& arrivedSeen = arrivedSeen_;
    if (arrivedSeen.size() != sim.stations.size()) arrivedSeen.assign(sim.stations.size(), 0);
    for (std::size_t i = 0; i < sim.stations.size(); ++i) {
        const Station& st = sim.route.stations[i];
        const StationProgress& pr = sim.stations[i];
        if (pr.arrived && !arrivedSeen[i]) {
            arrivedSeen[i] = 1;
            if (!first_ && !st.arrivalSound.empty()) playAt(buf(st.arrivalSound), driverCar_, {0, 0, 0}, 30.0);
        }
        if (!st.departureSound.empty() && pr.arrived && !pr.departed && !departurePlayed_[i] && st.departure >= 0.0) {
            const SoundBufferId b = buf(st.departureSound);
            if (sim.time >= st.departure - audio_.duration(b)) {
                departurePlayed_[i] = 1;
                playAt(b, driverCar_, {0, 0, 0}, 30.0);
            }
        }
    }
}

} // namespace ots
