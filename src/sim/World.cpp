#include "sim/World.hpp"

#include "phx/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <functional>
#include <limits>

namespace ots {

Simulation::Simulation(const Route& r, const TrainDef& def) : route(r) {
    train = std::make_unique<TrainSim>(def, r);
    sections = r.sections;
    stations.resize(r.stations.size());
}

void Simulation::start(std::optional<std::size_t> station) {
    if (!station) {
        const auto first = std::ranges::find_if(route.stations, [](const Station& s) { return s.stopsHere() && !s.stops.empty(); });
        if (first != route.stations.end()) station = std::size_t(first - route.stations.begin());
    }
    const std::optional<std::size_t> startStation = station && *station < route.stations.size() ? station : std::nullopt;
    double frontPos = train->length() + 1.0;
    if (startStation) {
        const Station& s = route.stations[*startStation];
        frontPos = s.stopPosition(int(train->def().cars.size()));
        // openBVE: the clock starts at the arrival time (or departure − stop time).
        if (s.arrival) time = *s.arrival;
        else if (s.departure) time = *s.departure - s.stopDuration;
        for (std::size_t i = 0; i < *startStation; ++i) stations[i].passed = true;
        StationProgress& p = stations[*startStation];
        p.arrived = true;
        p.doorsOpened = true;
        p.arrivalTime = time;
    }
    if (route.startTime) time = *route.startTime;
    train->placeAt(std::max(frontPos, train->length() + 1.0));

    // Preceding trains: run the traffic alone (the player is not there yet) from the earliest
    // departure up to the start time, so they are where they would be now.
    double earliest = time;
    for (TrafficTrain& t : traffic) earliest = std::min(earliest, t.schedule(time, startStation));
    if (earliest < time) {
        runTrafficUntil(earliest);
        // still standing where the player starts (interval shorter than the stop): it waits
        // and enters behind the player instead
        for (TrafficTrain& t : traffic)
            if (t.active() && t.train().rear() < train->front + 1.0 && t.train().front > train->rear()) {
                t.postpone(time);
                phx::log::warn("Traffic {}: still at the start position, enters after the player", t.name());
            }
        separateTrains();
    }
    updateStations(0.0);
    updateSections();
    for (const TrafficTrain& t : traffic)
        if (t.active())
            phx::log::info("Traffic {}: {:.0f} m ({} m ahead), {:.0f} km/h", t.name(), t.train().front, int(t.train().front - train->front), t.train().speed * 3.6);
}

void Simulation::runTrafficUntil(double from) {
    const double now = time;
    playerPresent_ = false;
    for (time = from; time < now;) {
        const double h = std::min(0.1, now - time);
        updateTraffic(h);
        time += h;
        updateSections();
    }
    time = now;
    playerPresent_ = true;
}

bool Simulation::spawnAreaClear(const TrafficTrain& t) const {
    const double front = t.spawnFront(), lo = front - t.train().length() - 30.0, hi = front + 30.0;
    bool clear = true;
    forEachTrain([&](const TrainSim& o, auto&&, bool) { clear = clear && !(o.rear() < hi && o.front > lo); });
    return clear;
}

void Simulation::updateTraffic(double dt) {
    for (TrafficTrain& t : traffic)
        if (t.due(time) && spawnAreaClear(t)) t.spawn(time); // waits until its platform is free
    for (TrafficTrain& t : traffic)
        if (t.active()) t.drive(*this);
    // fixed physics sub-steps (stable handles / brakes at any frame rate)
    const int steps = std::max(1, int(std::ceil(dt / (1.0 / 120.0))));
    const double h = dt / steps;
    for (int i = 0; i < steps; ++i) {
        if (playerPresent_) train->update(h);
        for (TrafficTrain& t : traffic)
            if (t.active()) t.train().update(h);
    }
    separateTrains();
}

// Trains cannot pass through each other: whoever is behind stops at the rear of the one ahead.
void Simulation::separateTrains() {
    order_.clear();
    if (playerPresent_) order_.push_back(train.get());
    for (TrafficTrain& t : traffic)
        if (t.active()) order_.push_back(&t.train());
    std::ranges::sort(order_, std::greater{}, &TrainSim::front);
    for (std::size_t k = 1; k < order_.size(); ++k) {
        const TrainSim& ahead = *order_[k - 1];
        TrainSim& behind = *order_[k];
        const double limit = ahead.rear() - 0.3;
        if (behind.front <= limit) continue;
        if (behind.speed - ahead.speed > 1.0)
            phx::log::warn("Collision at {:.0f} m ({:.0f} km/h){}", limit, (behind.speed - ahead.speed) * 3.6, &behind == train.get() ? " by the player" : "");
        behind.setFront(limit);
        behind.speed = std::min(behind.speed, ahead.speed);
    }
}

Expected<void> Simulation::startPlugin(const std::filesystem::path& hostExe) {
    const TrainDef& def = train->def();
    const std::filesystem::path dll = findTrainPlugin(def.folder);
    if (dll.empty()) {
        pluginStatus = "no plugin (ats.cfg)";
        return Unexpected(pluginStatus);
    }
    const TrainSpec& spec = def.spec;
    TrainPlugin::Setup setup{.powerNotches = spec.powerNotches,
                             .brakeType = int(spec.brakeType),
                             .brakeNotches = spec.brakeNotches,
                             .holdBrake = spec.holdBrake,
                             .cars = int(def.cars.size()),
                             .initMode = int(route.safetyStart),
                             .stations = {}};
    for (const Station& s : route.stations) {
        PluginStation p;
        p.name = s.name;
        p.arrival = s.arrival.value_or(-1.0), p.departure = s.departure.value_or(-1.0); // plugin protocol: -1 = none
        if (const StopPoint* sp = s.stopFor(int(def.cars.size()))) p.stop = sp->trackPos, p.forward = sp->forward, p.backward = sp->backward;
        p.left = s.doors <= 0, p.right = s.doors >= 0;
        p.mode = int(s.mode);
        setup.stations.push_back(std::move(p));
    }
    auto pl = std::make_unique<TrainPlugin>();
    if (const auto started = pl->start(hostExe, dll, def.folder, setup); !started) {
        pluginStatus = dll.filename().string() + ": " + started.error();
        return Unexpected(pluginStatus);
    }
    plugin = std::move(pl);
    pluginStatus = dll.filename().string() + " running";
    // openBVE places the train by moving it from the route start, so every beacon before the
    // start position (ATP level, route number, speed profile ...) reaches the plugin first.
    prevFront_ = -1.0e18;
    return {};
}

void Simulation::pluginKey(int key, bool down) {
    if (onboard && key == 0) onboard->acknowledge(down); // S: 확인 (acknowledge)
    if (!plugin || !plugin->running()) return;
    if (down) plugin->keyDown(key);
    else plugin->keyUp(key);
}

void Simulation::hornBlow(int type) {
    if (plugin && plugin->running()) plugin->horn(type);
}

void Simulation::updatePlugin(double dt) {
    TrainSim& t = *train;
    if (!plugin || !plugin->running()) {
        t.safetyActive = false;
        if (plugin && !plugin->lastError.empty() && pluginStatus.find("stopped") == std::string::npos)
            pluginStatus = plugin->name + " stopped: " + plugin->lastError;
        return;
    }
    std::erase_if(messages, [&](const PluginMessage& m) { return m.until < time || m.until > time + 3600.0; });
    // beacons passed since the last frame (forward or backward)
    const double lo = std::min(prevFront_, t.front), hi = std::max(prevFront_, t.front);
    if (hi > lo)
        for (const Beacon& b : route.beacons) {
            if (b.trackPos <= lo) continue;
            if (b.trackPos > hi) break;
            std::size_t idx = b.section.value_or(0);
            if (!b.section) { // next red section ahead
                const auto cur = route.sectionAt(t.front);
                idx = cur ? *cur + 1 : 0;
                while (idx < sections.size() && sections[idx].pluginAspect != 0) ++idx;
            }
            int aspect = 0;
            double dist = 1.0e9;
            if (idx < sections.size()) {
                aspect = sections[idx].pluginAspect;
                dist = sections[idx].trackPos - t.front;
            }
            plugin->beacon(b.type, b.optional, aspect, dist);
            if (std::getenv("OTS_PLUGIN_TRACE")) phx::log::info("beacon {} {} at {:.0f} m (aspect {})", b.type, b.optional, b.trackPos, aspect);
        }
    prevFront_ = t.front;
    // signals ahead: from the current section up to the first red (max 16)
    {
        const std::size_t cur = route.sectionAt(t.front).value_or(0);
        std::vector<std::pair<int, double>> sig;
        for (std::size_t i = cur; i < sections.size() && sig.size() < 16; ++i) {
            sig.push_back({sections[i].pluginAspect, sections[i].trackPos - t.front});
            if (sections[i].pluginAspect == 0) break;
        }
        bool changed = cur != lastSection_ || sig.size() != lastSignals_.size();
        for (std::size_t i = 0; !changed && i < sig.size(); ++i) changed = sig[i].first != lastSignals_[i].first;
        if (changed) plugin->signal(sig);
        lastSection_ = cur;
        lastSignals_ = std::move(sig);
    }
    // driver handle changes
    const int brk = t.emergency ? t.maxBrake() + 1 : t.brakeNotch;
    if (lastRev_ != t.reverser) plugin->setReverser(*(lastRev_ = t.reverser));
    if (lastPow_ != t.powerNotch) plugin->setPower(*(lastPow_ = t.powerNotch));
    if (lastBrk_ != brk) plugin->setBrake(*(lastBrk_ = brk));
    // doors (openBVE DoorStates: 1 left, 2 right)
    const int doors = (t.leftDoors > 0.0 ? 1 : 0) | (t.rightDoors > 0.0 ? 2 : 0);
    if (doors != lastDoors_) plugin->doorChange(lastDoors_, doors), lastDoors_ = doors;

    PluginVehicle v;
    const TrackPose pose = route.poseAt(t.front);
    v.location = t.front;
    v.speedKmh = t.speed * 3.6;
    v.bc = t.brakeCylinder * 1000.0, v.mr = t.mainReservoir * 1000.0, v.er = t.brakePipe * 1000.0, v.bp = t.brakePipe * 1000.0,
    v.sap = t.straightAirPipe * 1000.0; // Pa
    v.radius = pose.curveRadius, v.cant = pose.cant, v.pitch = pose.pitch;
    v.totalMs = time * 1000.0;
    v.elapsedMs = dt * 1000.0;
    v.driver = {t.reverser, t.powerNotch, brk, t.constSpeed, t.holdBrake, 0, t.headlights ? 1 : 0};
    v.camera = cameraMode == 0 ? 2 : 8;
    if (const auto handles = plugin->elapse(v)) {
        const PluginHandles& out = *handles;
        if (lastSafetyBrake_ != out.brake || lastSafetyPower_ != out.power)
            phx::log::info("plugin {:.0f} m {:.1f} km/h: driver P{} B{} -> safety P{} B{}{}", t.front, t.speed * 3.6, t.powerNotch, brk, out.power,
                           out.brake, out.brake > t.maxBrake() ? " (EB)" : "");
        lastSafetyBrake_ = out.brake, lastSafetyPower_ = out.power;
        static const bool trace = std::getenv("OTS_PLUGIN_TRACE") != nullptr;
        if (trace && plugin->panel != tracePanel_) {
            std::string d;
            for (std::size_t i = 0; i < plugin->panel.size(); ++i)
                if (i >= tracePanel_.size() || plugin->panel[i] != tracePanel_[i]) d += std::format(" [{}]={}", i, plugin->panel[i]);
            phx::log::info("plugin panel @{:.0f} m:{}", t.front, d);
            tracePanel_ = plugin->panel;
        }
        t.safetyActive = true;
        t.safetyReverser = std::clamp(out.reverser, -1, 1);
        t.safetyPower = std::clamp(out.power, 0, t.maxPower());
        t.safetyBrake = std::clamp(out.brake, 0, t.maxBrake() + 1);
        t.safetyHoldBrake = out.holdBrake;
        if (plugin->doorRequest) {
            const int r = plugin->doorRequest;
            if (r & 1) t.leftDoorsTarget = true;
            if (r & 2) t.rightDoorsTarget = true;
            if (r & 4) t.leftDoorsTarget = false;
            if (r & 8) t.rightDoorsTarget = false;
            plugin->doorRequest = 0;
        }
        for (PluginMessage& m : plugin->messages) {
            m.until = time + std::max(1.0, m.seconds);
            messages.push_back(std::move(m));
        }
        plugin->messages.clear();
        if (messages.size() > 8) messages.erase(messages.begin(), messages.end() - 8);
    } else {
        t.safetyActive = false;
    }
}

void Simulation::update(double dt) {
    if (paused) return;
    dt *= timeFactor;
    updatePlugin(dt);
    if (onboard) onboard->update(safety::Trackside(route, sections), *train, dt);
    updateTraffic(dt); // AI handles, then every train's physics
    time += dt;
    if (time >= 86400.0) time -= 86400.0;
    updateStations(dt);
    updateSections();
    overspeed = std::abs(train->speed) > speedLimit() + 1.0 / 3.6;
}

double Simulation::speedLimit() const {
    double lim = route.speedLimitAt(train->front);
    // signal speed of the section the train is in (Route.Signal(aspect))
    if (const auto s = route.sectionAt(train->front)) {
        const int aspect = sections[*s].currentAspect;
        if (aspect >= 0 && std::size_t(aspect) < route.signalSpeeds.size()) lim = std::min(lim, route.signalSpeeds[std::size_t(aspect)]);
    }
    return lim;
}

void Simulation::updateSections() {
    const int n = int(sections.size());
    int freeAhead = 1 << 20; // beyond the last section: clear
    int nextAspect = std::numeric_limits<int>::max();
    for (int i = n - 1; i >= 0; --i) {
        Section& s = sections[std::size_t(i)];
        const double start = s.trackPos;
        const double end = i + 1 < n ? sections[std::size_t(i) + 1].trackPos : std::numeric_limits<double>::infinity();
        // occupancy by any train; the departure signal of a station is held at danger until
        // the train approaching it (the nearest one behind) may depart from that station
        bool occupied = false, otherTrain = false;
        const std::vector<StationProgress>* approaching = nullptr;
        double approachingFront = -std::numeric_limits<double>::infinity();
        forEachTrain([&](const TrainSim& t, const std::vector<StationProgress>& progress, bool player) {
            if (t.rear() < end && t.front > start) occupied = true, otherTrain = otherTrain || !player;
            if (t.front <= start + 1.0 && t.front > approachingFront) approachingFront = t.front, approaching = &progress;
        });
        bool held = false;
        if (s.departureStation && *s.departureStation < stations.size() && approaching) {
            const StationProgress& p = (*approaching)[*s.departureStation];
            if (!p.departed && !p.passed) occupied = held = true;
        }
        if (s.aspects.empty()) continue;
        // What the safety plugin sees (openBVE Section.GetPluginSignal): the player train's own
        // occupancy is ignored, so the section it is in shows the aspect it would have if free.
        if (held || otherTrain) {
            s.pluginAspect = s.aspects.front();
        } else if (s.type == SectionType::ValueBased) {
            int a = s.aspects.back();
            if (nextAspect != std::numeric_limits<int>::max())
                for (int x : s.aspects)
                    if (x > nextAspect) {
                        a = x;
                        break;
                    }
            s.pluginAspect = a;
        } else {
            s.pluginAspect = s.aspects[std::size_t(std::min<int>(std::min(freeAhead + 1, 1 << 20), int(s.aspects.size()) - 1))];
        }
        if (occupied) {
            freeAhead = 0;
            s.currentAspect = s.aspects.front();
        } else if (s.type == SectionType::ValueBased) {
            int a = s.aspects.back();
            for (int x : s.aspects)
                if (x > nextAspect) {
                    a = x;
                    break;
                }
            if (nextAspect == std::numeric_limits<int>::max()) a = s.aspects.back();
            s.currentAspect = a;
            freeAhead = std::min(freeAhead + 1, 1 << 20);
        } else {
            freeAhead = std::min(freeAhead + 1, 1 << 20);
            s.currentAspect = s.aspects[std::size_t(std::min<int>(freeAhead, int(s.aspects.size()) - 1))];
        }
        nextAspect = s.currentAspect;
    }
}

double Simulation::distanceToStop(std::size_t station) const {
    if (station >= route.stations.size()) return std::numeric_limits<double>::infinity();
    return route.stations[std::size_t(station)].stopPosition(int(train->def().cars.size())) - train->front;
}

void Simulation::updateStations(double) {
    // next station: first not yet passed/departed station ahead of the rear of the train
    nextStation.reset();
    for (std::size_t i = 0; i < route.stations.size(); ++i) {
        StationProgress& p = stations[i];
        if (p.passed || p.departed) continue;
        const double d = distanceToStop(i);
        const Station& s = route.stations[i];
        const StopPoint* sp = s.stopFor(int(train->def().cars.size()));
        const double fwd = sp ? sp->forward : 5.0, back = sp ? sp->backward : 5.0;
        if (!s.stopsHere()) {
            if (d < -fwd) p.passed = true;
            else if (!nextStation) nextStation = i;
            continue;
        }
        if (d < -fwd - 50.0 && !p.arrived) {
            p.passed = true; // overran / skipped
            continue;
        }
        if (!nextStation) nextStation = i;
        if (i != *nextStation) continue;
        const bool inWindow = d <= back && d >= -fwd;
        if (!p.arrived && inWindow && std::abs(train->speed) < 0.05) {
            p.arrived = true;
            p.arrivalTime = time;
            p.stopError = -d;
        }
        if (p.arrived) {
            if (train->leftDoors > 0.5 || train->rightDoors > 0.5) p.doorsOpened = true;
            const bool timeOk = !s.departure || time >= *s.departure - 1.0;
            const bool doorsOk = p.doorsOpened || s.doors == 0;
            if (timeOk && doorsOk && train->doorsClosed() && !s.terminal) p.departed = true;
            if (!inWindow && d < -fwd) p.departed = true; // left without the formalities
        }
    }
}

std::optional<Simulation::SignalAhead> Simulation::signalAhead() const {
    const auto next = std::ranges::upper_bound(sections, train->front + 0.1, {}, &Section::trackPos);
    if (next == sections.end()) return std::nullopt;
    return SignalAhead{std::size_t(next - sections.begin()), next->trackPos - train->front, next->currentAspect};
}

f32 Simulation::cabBrightness() const {
    // openBVE (BaseRenderer, no nighttime texture): darkness = 1 − Track.Brightness, blended with
    // how bright the route lighting already is, then factor = 1 − 0.7·blend: never below 30 %.
    const float b = std::clamp(route.brightnessAt(train->front), 0.0f, 1.0f);
    const glm::vec3 a = route.ambient * 255.0f, d = route.diffuse * 255.0f;
    const float lightSum = std::max(a.r + a.g + a.b, d.r + d.g + d.b);
    const float resulting = std::min(1.0f, 0.00208333333f * lightSum);
    const float blend = std::min(1.0f, (1.0f - b) + 1.0f - resulting);
    return 1.0f - 0.7f * blend;
}

double ScriptContext::variable(Var v, int index) const {
    const TrainSim& t = train ? *train : *sim_.train;
    const TrainSpec& s = t.def().spec;
    const int carIdx = index >= 0 ? index : car;
    auto pressure = [](double kpa) { return kpa * 1000.0; }; // openBVE scripts use Pa
    switch (v) {
    case Var::Time: return sim_.time;
    case Var::Hour: return std::floor(sim_.time / 3600.0);
    case Var::Minute: return std::floor(std::fmod(sim_.time, 3600.0) / 60.0);
    case Var::Second: return std::floor(std::fmod(sim_.time, 60.0));
    case Var::CameraDistance: return glm::length(position - sim_.cameraPosition);
    case Var::CameraXDistance: return sim_.cameraPosition.x - position.x;
    case Var::CameraYDistance: return sim_.cameraPosition.y - position.y;
    case Var::CameraZDistance: return sim_.cameraPosition.z - position.z;
    case Var::CameraMode: return sim_.cameraMode;
    case Var::Cars: return double(t.cars.size());
    case Var::CarNumber: return car;
    case Var::Speed: return t.speed;
    case Var::Speedometer: return std::abs(t.speed);
    case Var::Acceleration: return t.acceleration;
    case Var::AccelerationMotor: return t.motorAcceleration;
    case Var::TrackDistance: return trainObject ? 0.0 : trackPosition - t.front;
    case Var::Distance: {
        if (trainObject) return 0.0;
        const double d = trackPosition - t.front;
        if (d > 0) return d;
        const double r = trackPosition - t.rear();
        return r < 0 ? -r : 0.0;
    }
    case Var::CurveRadius:
    case Var::FrontAxleCurveRadius:
    case Var::RearAxleCurveRadius: return sim_.route.poseAt(t.front).curveRadius;
    case Var::CurveCant: return sim_.route.poseAt(t.front).cant;
    case Var::Pitch: return sim_.route.poseAt(t.front).pitch;
    case Var::Doors: return std::max(t.leftDoors, t.rightDoors);
    case Var::LeftDoors: return t.leftDoors;
    case Var::RightDoors: return t.rightDoors;
    case Var::LeftDoorsTarget: return t.leftDoorsTarget ? 1.0 : 0.0;
    case Var::RightDoorsTarget: return t.rightDoorsTarget ? 1.0 : 0.0;
    case Var::ReverserNotch: return t.reverser;
    case Var::PowerNotch: return t.powerNotch;
    case Var::PowerNotches: return t.maxPower();
    case Var::BrakeNotch: return t.emergency ? t.maxBrake() + 1 : t.brakeNotch;
    case Var::BrakeNotches: return t.maxBrake();
    case Var::BrakeNotchLinear: return t.emergency ? t.maxBrake() + 1 : t.brakeNotch;
    case Var::BrakeNotchesLinear: return t.maxBrake() + 1;
    case Var::EmergencyBrake: return t.emergency ? 1.0 : 0.0;
    case Var::HasAirBrake: return s.brakeType == BrakeType::AutomaticAir ? 1.0 : 0.0;
    case Var::HoldBrake: return t.holdBrake ? 1.0 : 0.0;
    case Var::HasHoldBrake: return s.holdBrake ? 1.0 : 0.0;
    case Var::ConstSpeed: return t.constSpeed ? 1.0 : 0.0;
    case Var::HasConstSpeed: return s.constSpeed ? 1.0 : 0.0;
    case Var::HasPlugin: return sim_.plugin && sim_.plugin->running() ? 1.0 : 0.0;
    case Var::PluginState:
        if (sim_.plugin && index >= 0 && std::size_t(index) < sim_.plugin->panel.size()) return sim_.plugin->panel[std::size_t(index)];
        return 0.0;
    case Var::Safety: { // built-in train protection, docs/safety.md
        if (!sim_.onboard) return index == 0 ? 0.0 : -1.0;
        const safety::Display& d = sim_.onboard->display();
        const auto kmh = [](const std::optional<double>& v) { return v ? *v * 3.6 : -1.0; };
        switch (index) {
        case 0: return sim_.onboard->active() ? double(int(*sim_.onboard->active()) + 1) : 0.0; // 1 ATS, 2 TVM430, 3 ATP
        case 1: return kmh(d.permitted);
        case 2: return kmh(d.target);
        case 3: return d.targetDistance.value_or(-1.0);
        case 4: return d.warning ? 1.0 : 0.0;
        case 5: return d.braking ? 1.0 : 0.0;
        case 6: return d.emergency ? 1.0 : 0.0;
        case 7: return d.ackRequired ? 1.0 : 0.0;
        case 8: return kmh(d.intervention);
        case 9: { // code: ATS code index, TVM step km/h (RRR −1), ATP mode (0 FS, 1 SR, 2 TR)
            switch (sim_.onboard->active().value_or(safety::System::Ats)) {
            case safety::System::Ats: return double(sim_.onboard->atsCode());
            case safety::System::Tvm430: return d.code == "RRR" ? -1.0 : d.code == "000" ? 0.0 : std::atof(d.code.c_str());
            case safety::System::Atp: return d.code == "FS" ? 0.0 : d.code == "SR" ? 1.0 : 2.0;
            }
            return 0.0;
        }
        default: return 0.0;
        }
    }
    case Var::MainReservoir: return pressure(t.mainReservoir);
    case Var::EmergencyReservoir: return pressure(t.mainReservoir);
    case Var::BrakePipe: return pressure(t.brakePipe);
    case Var::BrakeCylinder: return pressure(t.brakeCylinder);
    case Var::StraightAirPipe: return pressure(t.straightAirPipe);
    case Var::EqualizingReservoir: return pressure(t.brakePipe);
    case Var::Section:
        if (section && *section < sim_.sections.size()) return sim_.sections[*section].currentAspect;
        return 0.0;
    case Var::Klaxon:
    case Var::PrimaryKlaxon: return t.hornPrimary ? 1.0 : 0.0;
    case Var::SecondaryKlaxon: return t.hornSecondary ? 1.0 : 0.0;
    case Var::MusicKlaxon: return 0.0;
    case Var::PassAlarm: return 0.0;
    case Var::PilotLamp: return t.doorsClosed() ? 1.0 : 0.0;
    case Var::StationAdjustAlarm: return 0.0;
    case Var::Headlights: return t.headlights ? 1.0 : 0.0;
    case Var::WiperPosition: return 0.0;
    case Var::RouteLimit: return sim_.route.speedLimitAt(t.front);
    case Var::NextStation:
    case Var::NextStationStop: return sim_.nextStation ? double(*sim_.nextStation) : -1.0;
    case Var::TerminalStation: return double(sim_.route.stations.size()) - 1;
    case Var::DistanceNextStation: return sim_.nextStation ? sim_.distanceToStop(*sim_.nextStation) : 0.0;
    case Var::StopsNextStation: return sim_.nextStation && sim_.route.stations[*sim_.nextStation].stopsHere() ? 1.0 : 0.0;
    case Var::DistanceStation: return index >= 0 && std::size_t(index) < sim_.route.stations.size() ? sim_.distanceToStop(std::size_t(index)) : 0.0;
    case Var::TimetableVisible: return 0.0;
    case Var::Brightness: return sim_.cabBrightness();
    case Var::PantographState: return 1.0;
    case Var::MainBreaker: return 1.0;
    case Var::LocoBrakeNotch:
    case Var::LocoBrakeNotches:
    case Var::LeftDoorButton:
    case Var::RightDoorButton:
    default: (void)carIdx; return 0.0;
    }
}

} // namespace ots
