#include "sim/Traffic.hpp"

#include "phx/core/Log.hpp"
#include "sim/World.hpp"
#include "util/Json.hpp"
#include "util/Paths.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>

namespace ots {
namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kStopped = 0.05; // m/s

// Braking curves to targets ahead: the highest speed now from which the train can still
// slow to `vEnd` within `dist`, and the deceleration the tightest one needs right now.
class BrakingCurves {
public:
    BrakingCurves(double speed, double comfortable) : v_(speed), b_(comfortable) {}
    double operator()(double vEnd, double dist) {
        if (v_ > vEnd) needed_ = std::max(needed_, (v_ * v_ - vEnd * vEnd) / (2.0 * std::max(0.5, dist)));
        return std::sqrt(vEnd * vEnd + 2.0 * b_ * std::max(0.0, dist));
    }
    [[nodiscard]] double needed() const { return needed_; }
    [[nodiscard]] double horizon() const { return v_ * v_ / (2.0 * b_) + 400.0; } // nothing farther matters yet

private:
    double v_, b_, needed_ = 0.0;
};

double signalSpeed(const Route& route, int aspect) {
    return aspect >= 0 && std::size_t(aspect) < route.signalSpeeds.size() ? route.signalSpeeds[std::size_t(aspect)] : kInf;
}

} // namespace

TrafficTrain::TrafficTrain(const TrainDef& def, const Route& route, TrafficOptions options)
    : options_(std::move(options)), route_(route), train_(std::make_unique<TrainSim>(def, route)), progress_(route.stations.size()) {}

double TrafficTrain::stopPosition(std::size_t station) const { return route_.stations[station].stopPosition(int(train_->cars.size())); }

double TrafficTrain::schedule(double clock, std::optional<std::size_t> playerStation) {
    startStation_ = options_.station && *options_.station < route_.stations.size() ? options_.station : playerStation;
    const Station* st = startStation_ ? &route_.stations[*startStation_] : nullptr;
    const double dwell = options_.dwell.value_or(st ? st->stopDuration : 30.0);
    if (options_.departure) {
        timeOffset_ = st && st->departure ? *st->departure - *options_.departure : 0.0;
        spawnTime_ = *options_.departure - std::max(dwell, 30.0);
    } else {
        timeOffset_ = options_.interval;
        spawnTime_ = clock - options_.interval;
    }
    spawnTime_ = std::max(spawnTime_, clock - 4.0 * 3600.0); // bounded warm-up
    return spawnTime_;
}

double TrafficTrain::spawnFront() const {
    const double front = train_->length() + 1.0;
    return startStation_ ? std::max(front, stopPosition(*startStation_)) : front;
}

void TrafficTrain::spawn(double clock) {
    train_->placeAt(spawnFront());
    train_->reverser = 1;
    hold();
    state_ = State::Running;
    section_.reset();
    if (!startStation_) return;
    const std::size_t start = *startStation_;
    for (std::size_t i = 0; i < start; ++i) progress_[i].passed = true;
    progress_[start].arrived = true;
    progress_[start].arrivalTime = clock;
    const Station& s = route_.stations[start];
    double departAt = options_.departure.value_or(clock + options_.dwell.value_or(s.stopDuration));
    if (s.departure && !options_.departure) departAt = std::max(departAt, *s.departure - timeOffset_);
    stop_ = Stop{start, departAt};
}

void TrafficTrain::postpone(double clock) {
    state_ = State::Waiting;
    spawnTime_ = clock;
    stop_.reset();
    std::ranges::fill(progress_, StationProgress{});
}

void TrafficTrain::hold() {
    train_->powerNotch = 0;
    train_->brakeNotch = train_->maxBrake();
}

double TrafficTrain::departureAt(const Station& s, double arrival) const {
    const double earliest = arrival + options_.dwell.value_or(s.stopDuration);
    return s.departure ? std::max(earliest, *s.departure - timeOffset_) : earliest;
}

void TrafficTrain::trackSignalSpeed(const Simulation& sim, std::size_t section) {
    if (section_ != section) {
        if (section_ && section > *section_) signalSpeed_ = aheadSignalSpeed_; // passed the signal
        section_ = section;
    }
    if (section + 1 < sim.sections.size()) aheadSignalSpeed_ = signalSpeed(route_, sim.sections[section + 1].currentAspect);
}

void TrafficTrain::serveStop(const Simulation& sim) {
    hold();
    const Station& s = route_.stations[stop_->station];
    StationProgress& p = progress_[stop_->station];
    constexpr double kDoorTime = 5.0; // doors close this long before departure
    if (!p.doorsOpened && sim.time < stop_->departAt - kDoorTime) {
        train_->leftDoorsTarget = s.doors <= 0;
        train_->rightDoorsTarget = s.doors >= 0;
        p.doorsOpened = true;
    }
    if (sim.time >= stop_->departAt - kDoorTime) train_->leftDoorsTarget = train_->rightDoorsTarget = false;
    if (sim.time < stop_->departAt || !train_->doorsClosed()) return;
    if (s.terminal || stop_->station + 1 == route_.stations.size()) {
        state_ = State::Done; // leaves the route at its terminal
        return;
    }
    p.departed = true;
    stop_.reset();
}

std::optional<std::size_t> TrafficTrain::nextStop(const Simulation&) {
    for (std::size_t i = 0; i < route_.stations.size(); ++i) {
        StationProgress& p = progress_[i];
        if (p.passed || p.departed) continue;
        const Station& s = route_.stations[i];
        const StopPoint* sp = s.stopFor(int(train_->cars.size()));
        const double d = stopPosition(i) - train_->front, forward = sp ? sp->forward : 5.0;
        const bool stopsHere = options_.stops && s.stopsHere();
        if (d < -forward - (stopsHere ? 10.0 : 0.0)) {
            p.passed = true; // behind us (passed through, or overran and never stopped)
            continue;
        }
        if (stopsHere) return i;
    }
    return std::nullopt;
}

bool TrafficTrain::tryArrive(const Simulation& sim, std::size_t station) {
    const Station& s = route_.stations[station];
    const StopPoint* sp = s.stopFor(int(train_->cars.size()));
    const double d = stopPosition(station) - train_->front;
    const double backward = sp ? sp->backward : 5.0, forward = sp ? sp->forward : 5.0;
    // stopped short enough of the stop point; a small overrun still counts
    if (std::abs(train_->speed) >= kStopped || d > std::min(backward, 2.0) || d < -forward - 10.0) return false;
    StationProgress& p = progress_[station];
    p.arrived = true;
    p.arrivalTime = sim.time;
    p.stopError = -d;
    stop_ = Stop{station, departureAt(s, sim.time)};
    hold();
    return true;
}

void TrafficTrain::drive(const Simulation& sim) {
    TrainSim& tr = *train_;
    const std::size_t section = route_.sectionAt(tr.front).value_or(0); // the route starts with a section at 0
    trackSignalSpeed(sim, section);
    if (stop_) {
        serveStop(sim);
        return;
    }
    const auto next = nextStop(sim);
    if (next && tryArrive(sim, *next)) return;
    if (tr.front >= route_.endPosition() - 60.0 && std::abs(tr.speed) < 0.1) {
        state_ = State::Done;
        return;
    }

    // target speed: limits where the train is, and braking curves to everything ahead
    const double comfortable = std::max(0.25, 0.55 * tr.def().spec.deceleration);
    BrakingCurves curve(tr.speed, comfortable);
    double target = std::min({options_.maxSpeed, route_.speedLimitAt(tr.front), route_.speedLimitAt(tr.rear()), signalSpeed_});
    const double horizon = curve.horizon();
    for (const SpeedLimit& l : route_.limits) {
        if (l.trackPos <= tr.front) continue;
        if (l.trackPos > tr.front + horizon) break;
        target = std::min(target, curve(l.speed, l.trackPos - tr.front - 5.0));
    }
    for (std::size_t i = section + 1; i < sim.sections.size(); ++i) {
        const Section& sec = sim.sections[i];
        const double d = sec.trackPos - tr.front;
        if (d > horizon) break;
        if (sec.aspects.empty()) continue;
        if (sec.currentAspect == 0) {
            target = std::min(target, curve(0.0, d - 8.0)); // stop in front of a red signal
            break;
        }
        if (sec.currentAspect > 0) target = std::min(target, curve(signalSpeed(route_, sec.currentAspect), d));
    }
    if (next) target = std::min(target, curve(0.0, stopPosition(*next) - tr.front - 0.5));
    target = std::min(target, curve(0.0, route_.endPosition() - 60.0 - tr.front));
    sim.forEachTrain([&](const TrainSim& other, auto&&, bool) { // never run into another train, signals or not
        if (&other != &tr && other.front > tr.front) target = std::min(target, curve(0.0, other.rear() - tr.front - 30.0));
    });

    static const bool trace = std::getenv("OTS_TRAFFIC_TRACE") != nullptr;
    if (trace && std::abs(tr.speed) < 0.1 && std::fmod(sim.time, 20.0) < 0.05) {
        const auto ahead = std::find_if(sim.sections.begin() + std::ptrdiff_t(std::min(section + 1, sim.sections.size())), sim.sections.end(), [](const Section& s) { return !s.aspects.empty(); });
        phx::log::info("traffic {} stopped at {:.0f} m: vt {:.1f} km/h, limit {:.0f}, sig {:.0f}, next signal {} in {:.0f} m, next stop {} ({:.0f} m)", name(),
                       tr.front, target * 3.6, route_.speedLimitAt(tr.front) * 3.6, signalSpeed_ * 3.6, ahead != sim.sections.end() ? ahead->currentAspect : -1,
                       ahead != sim.sections.end() ? ahead->trackPos - tr.front : -1.0, next ? int(*next) : -1, next ? stopPosition(*next) - tr.front : 0.0);
    }
    setHandles(target, curve.needed(), comfortable);
}

void TrafficTrain::setHandles(double target, double needed, double comfortable) {
    TrainSim& tr = *train_;
    tr.reverser = 1;
    tr.emergency = false;
    const double err = tr.speed - target;
    // brake on the speed error, and early enough for what is ahead (handles and the brake
    // cylinder lag behind): the notch that gives the needed deceleration, with margin
    int brake = err > 0.3 ? int(std::ceil(std::min(1.0, (err - 0.3) / 2.0) * tr.maxBrake())) : 0;
    if (needed > 0.8 * comfortable)
        brake = std::max(brake, int(std::ceil(std::min(1.0, 1.25 * needed / std::max(0.1, tr.def().spec.deceleration)) * tr.maxBrake())));
    if (target < 0.3 && tr.speed < 0.5) {
        hold();
    } else if (brake > 0) {
        tr.powerNotch = 0;
        tr.brakeNotch = std::clamp(brake, 1, tr.maxBrake());
    } else if (err < -1.0) {
        tr.brakeNotch = 0;
        tr.powerNotch = std::clamp(int(std::ceil(std::min(1.0, (-err - 1.0) / 3.0) * tr.maxPower())), 1, tr.maxPower());
    } else {
        tr.powerNotch = tr.brakeNotch = 0; // coast
    }
}

// ------------------------------------------------------------------ traffic.json
std::optional<double> parseClock(std::string_view s) {
    auto number = [](std::string_view t) -> std::optional<int> {
        int v = 0;
        const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
        return ec == std::errc() && end == t.data() + t.size() ? std::optional(v) : std::nullopt;
    };
    if (s.find(':') != std::string_view::npos) {
        std::array<int, 3> parts{};
        std::size_t n = 0;
        for (std::size_t start = 0; start <= s.size() && n < 3; ++n) {
            const std::size_t end = std::min(s.find(':', start), s.size());
            const auto v = number(s.substr(start, end - start));
            if (!v) return std::nullopt;
            parts[n] = *v;
            start = end + 1;
        }
        return n >= 2 ? std::optional(parts[0] * 3600.0 + parts[1] * 60.0 + parts[2]) : std::nullopt;
    }
    const std::size_t dot = s.find('.');
    if (dot == std::string_view::npos) return std::nullopt;
    std::string frac(s.substr(dot + 1));
    frac.resize(4, '0');
    const auto h = number(s.substr(0, dot)), m = number(std::string_view(frac).substr(0, 2)), sec = number(std::string_view(frac).substr(2, 2));
    if (!h || !m || !sec) return std::nullopt;
    return *h * 3600.0 + *m * 60.0 + *sec;
}

namespace {


Expected<TrafficEntry> parseEntry(const std::filesystem::path& file, const Json& j, std::size_t index) {
    TrafficEntry e;
    if (const std::string folder = j.str("train"); !folder.empty()) {
        e.trainFolder = resolvePath(file.parent_path(), folder);
        if (e.trainFolder.empty()) return Unexpected("train folder not found: " + folder);
    }
    TrafficOptions& o = e.options;
    o.name = j.str("name", std::format("traffic {}", index + 1));
    o.interval = j.num("interval", 0.0);
    if (j["departure"].isString()) {
        o.departure = parseClock(j["departure"].string());
        if (!o.departure) return Unexpected(std::format("bad departure time '{}'", j["departure"].string()));
    }
    if (!o.departure && o.interval == 0.0) return Unexpected("needs \"interval\" (s) or \"departure\" (HH:MM:SS)");
    if (j["station"].isNumber() && j["station"].number() >= 0) o.station = std::size_t(j["station"].number());
    if (j.has("maxSpeed")) o.maxSpeed = j.num("maxSpeed", 0.0) / 3.6;
    o.stops = j.flag("stops", true);
    if (j["dwell"].isNumber() && j["dwell"].number() >= 0) o.dwell = j["dwell"].number();
    return e;
}

} // namespace

std::filesystem::path findTrafficFile(const std::filesystem::path& routeFile) {
    std::string base = routeFile.stem().string();
    if (base.ends_with(".route")) base.resize(base.size() - 6); // native "<name>.route.json" → <name>.traffic.json
    return resolvePath(routeFile.parent_path(), base + ".traffic.json");
}

Expected<TrafficConfig> loadTrafficConfig(const std::filesystem::path& file) {
    const auto root = Json::parseFile(file);
    if (!root) return Unexpected(root.error());
    TrafficConfig out;
    out.routeIntervals = root->flag("routeIntervals", true);
    const Json& list = (*root)["trains"];
    for (std::size_t i = 0; i < list.size(); ++i) {
        auto e = parseEntry(file, list[i], i);
        if (!e) return Unexpected(std::format("trains[{}]: {}", i, e.error()));
        out.trains.push_back(std::move(*e));
    }
    return out;
}

} // namespace ots
