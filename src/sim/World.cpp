#include "sim/World.hpp"

#include "phx/core/Log.hpp"

#include <cstdlib>
#include <format>

#include <algorithm>
#include <cmath>
#include <limits>

namespace ots {

Simulation::Simulation(const Route& r, const TrainDef& def) : route(r) {
    train = std::make_unique<TrainSim>(def, r);
    sections = r.sections;
    stations.resize(r.stations.size());
}

void Simulation::start(int station) {
    if (station < 0) {
        for (std::size_t i = 0; i < route.stations.size(); ++i)
            if (route.stations[i].stopsHere() && !route.stations[i].stops.empty()) {
                station = int(i);
                break;
            }
    }
    double frontPos = train->length() + 1.0;
    if (station >= 0 && std::size_t(station) < route.stations.size()) {
        const Station& s = route.stations[std::size_t(station)];
        if (const StopPoint* sp = s.stopFor(int(train->def().cars.size()))) frontPos = sp->trackPos;
        else frontPos = s.trackPos;
        // openBVE: the clock starts at the arrival time (or departure − stop time).
        if (s.arrival >= 0) time = s.arrival;
        else if (s.departure >= 0) time = s.departure - s.stopDuration;
        for (int i = 0; i <= station; ++i) stations[std::size_t(i)].passed = true;
        StationProgress& p = stations[std::size_t(station)];
        p.passed = false;
        p.arrived = true;
        p.doorsOpened = true;
        p.arrivalTime = time;
    }
    if (route.startTime >= 0) time = route.startTime;
    frontPos = std::max(frontPos, train->length() + 1.0);
    train->placeAt(frontPos);
    updateStations(0.0);
    updateSections();
}

bool Simulation::startPlugin(const std::filesystem::path& hostExe, std::string& status) {
    const TrainDef& def = train->def();
    const std::filesystem::path dll = findTrainPlugin(def.folder);
    if (dll.empty()) {
        status = "no plugin (ats.cfg)";
        return false;
    }
    std::vector<PluginStation> st;
    for (const Station& s : route.stations) {
        PluginStation p;
        p.name = s.name;
        p.arrival = s.arrival, p.departure = s.departure;
        if (const StopPoint* sp = s.stopFor(int(def.cars.size()))) p.stop = sp->trackPos, p.forward = sp->forward, p.backward = sp->backward;
        p.left = s.doors <= 0, p.right = s.doors >= 0;
        p.mode = int(s.mode);
        st.push_back(p);
    }
    auto pl = std::make_unique<TrainPlugin>();
    std::string err;
    const TrainSpec& spec = def.spec;
    if (!pl->start(hostExe, dll, def.folder, spec.powerNotches, int(spec.brakeType), spec.brakeNotches, spec.holdBrake, int(def.cars.size()),
                   route.initMode, st, err)) {
        status = dll.filename().string() + ": " + err;
        return false;
    }
    plugin = std::move(pl);
    status = dll.filename().string() + " running";
    // openBVE places the train by moving it from the route start, so every beacon before the
    // start position (ATP level, route number, speed profile ...) reaches the plugin first.
    prevFront_ = -1.0e18;
    return true;
}

void Simulation::pluginKey(int key, bool down) {
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
            int idx = b.section;
            if (idx == -1) { // next red section ahead
                idx = route.sectionAt(t.front) + 1;
                while (idx >= 0 && std::size_t(idx) < sections.size() && sections[std::size_t(idx)].pluginAspect != 0) ++idx;
            }
            int aspect = 0;
            double dist = 1.0e9;
            if (idx >= 0 && std::size_t(idx) < sections.size()) {
                aspect = sections[std::size_t(idx)].pluginAspect;
                dist = sections[std::size_t(idx)].trackPos - t.front;
            }
            plugin->beacon(b.type, b.optional, aspect, dist);
            if (std::getenv("OTS_PLUGIN_TRACE")) phx::log::info("beacon {} {} at {:.0f} m (aspect {})", b.type, b.optional, b.trackPos, aspect);
        }
    prevFront_ = t.front;
    // signals ahead: from the current section up to the first red (max 16)
    {
        const int cur = std::max(0, route.sectionAt(t.front));
        std::vector<std::pair<int, double>> sig;
        for (std::size_t i = std::size_t(cur); i < sections.size() && sig.size() < 16; ++i) {
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
    if (t.reverser != lastRev_) plugin->setReverser(lastRev_ = t.reverser);
    if (t.powerNotch != lastPow_) plugin->setPower(lastPow_ = t.powerNotch);
    if (brk != lastBrk_) plugin->setBrake(lastBrk_ = brk);
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
    PluginHandles out;
    if (plugin->elapse(v, out)) {
        if (out.brake != lastSafetyBrake_ || out.power != lastSafetyPower_)
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
    // fixed physics sub-steps (stable handles / brakes at any frame rate)
    const int steps = std::max(1, int(std::ceil(dt / (1.0 / 120.0))));
    const double h = dt / steps;
    for (int i = 0; i < steps; ++i) train->update(h);
    time += dt;
    if (time >= 86400.0) time -= 86400.0;
    updateStations(dt);
    updateSections();
    overspeed = std::abs(train->speed) > speedLimit() + 1.0 / 3.6;
}

double Simulation::speedLimit() const {
    double lim = route.speedLimitAt(train->front);
    // signal speed of the section the train is in (Route.Signal(aspect))
    const int s = route.sectionAt(train->front);
    if (s >= 0) {
        const int aspect = sections[std::size_t(s)].currentAspect;
        if (aspect >= 0 && std::size_t(aspect) < route.signalSpeeds.size()) lim = std::min(lim, route.signalSpeeds[std::size_t(aspect)]);
    }
    return lim;
}

void Simulation::updateSections() {
    const double front = train->front, rear = train->rear();
    const int n = int(sections.size());
    int freeAhead = 1 << 20; // beyond the last section: clear
    int nextAspect = std::numeric_limits<int>::max();
    for (int i = n - 1; i >= 0; --i) {
        Section& s = sections[std::size_t(i)];
        const double start = s.trackPos;
        const double end = i + 1 < n ? sections[std::size_t(i) + 1].trackPos : std::numeric_limits<double>::infinity();
        bool occupied = rear < end && front > start;
        bool held = false;
        if (s.departureStation >= 0 && std::size_t(s.departureStation) < stations.size()) {
            const StationProgress& p = stations[std::size_t(s.departureStation)];
            // held at danger until the train may depart from that station
            if (!p.departed && !p.passed) occupied = held = true;
        }
        if (s.aspects.empty()) continue;
        // What the safety plugin sees (openBVE Section.GetPluginSignal): the player train's own
        // occupancy is ignored, so the section it is in shows the aspect it would have if free.
        if (held) {
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

double Simulation::distanceToStop(int station) const {
    if (station < 0 || std::size_t(station) >= route.stations.size()) return std::numeric_limits<double>::infinity();
    const Station& s = route.stations[std::size_t(station)];
    const StopPoint* sp = s.stopFor(int(train->def().cars.size()));
    return (sp ? sp->trackPos : s.trackPos) - train->front;
}

void Simulation::updateStations(double) {
    // next station: first not yet passed/departed station ahead of the rear of the train
    nextStation = -1;
    for (std::size_t i = 0; i < route.stations.size(); ++i) {
        StationProgress& p = stations[i];
        if (p.passed || p.departed) continue;
        const double d = distanceToStop(int(i));
        const Station& s = route.stations[i];
        const StopPoint* sp = s.stopFor(int(train->def().cars.size()));
        const double fwd = sp ? sp->forward : 5.0, back = sp ? sp->backward : 5.0;
        if (!s.stopsHere()) {
            if (d < -fwd) p.passed = true;
            else if (nextStation < 0) nextStation = int(i);
            continue;
        }
        if (d < -fwd - 50.0 && !p.arrived) {
            p.passed = true; // overran / skipped
            continue;
        }
        if (nextStation < 0) nextStation = int(i);
        if (int(i) != nextStation) continue;
        const bool inWindow = d <= back && d >= -fwd;
        if (!p.arrived && inWindow && std::abs(train->speed) < 0.05) {
            p.arrived = true;
            p.arrivalTime = time;
            p.stopError = -d;
        }
        if (p.arrived) {
            if (train->leftDoors > 0.5 || train->rightDoors > 0.5) p.doorsOpened = true;
            const bool timeOk = s.departure < 0 || time >= s.departure - 1.0;
            const bool doorsOk = p.doorsOpened || s.doors == 0;
            if (timeOk && doorsOk && train->doorsClosed() && !s.terminal) p.departed = true;
            if (!inWindow && d < -fwd) p.departed = true; // left without the formalities
        }
    }
}

int Simulation::signalAhead(double& distance, int& aspect) const {
    for (std::size_t i = 0; i < sections.size(); ++i)
        if (sections[i].trackPos > train->front + 0.1) {
            distance = sections[i].trackPos - train->front;
            aspect = sections[i].currentAspect;
            return int(i);
        }
    distance = std::numeric_limits<double>::infinity();
    aspect = -1;
    return -1;
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
    const TrainSim& t = *sim_.train;
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
    case Var::MainReservoir: return pressure(t.mainReservoir);
    case Var::EmergencyReservoir: return pressure(t.mainReservoir);
    case Var::BrakePipe: return pressure(t.brakePipe);
    case Var::BrakeCylinder: return pressure(t.brakeCylinder);
    case Var::StraightAirPipe: return pressure(t.straightAirPipe);
    case Var::EqualizingReservoir: return pressure(t.brakePipe);
    case Var::Section:
        if (section >= 0 && std::size_t(section) < sim_.sections.size()) return sim_.sections[std::size_t(section)].currentAspect;
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
    case Var::NextStation: return sim_.nextStation;
    case Var::NextStationStop: return sim_.nextStation;
    case Var::TerminalStation: return double(sim_.route.stations.size()) - 1;
    case Var::DistanceNextStation: return sim_.nextStation >= 0 ? sim_.distanceToStop(sim_.nextStation) : 0.0;
    case Var::StopsNextStation: return sim_.nextStation >= 0 && sim_.route.stations[std::size_t(sim_.nextStation)].stopsHere() ? 1.0 : 0.0;
    case Var::DistanceStation: return index >= 0 ? sim_.distanceToStop(index) : 0.0;
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
