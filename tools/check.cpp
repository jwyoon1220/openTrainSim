// Headless checker (no window, no GPU):
//   ots_check <route.csv> [--objects]         parse a route (and every object), short test drive
//   ots_check --dump <object>...              print objects
//   ots_check --sounds <train folder>         resolve and decode a train's sounds
//   ots_check --vvvf <preset.json> <out.wav> [top km/h]   render a VVVF preset to a WAV file
//   ots_check --selftest                      function script compiler tests
//   ots_check --traffic <route.csv> <train folder> <traffic.json> [minutes] [stay|drive]
//   ots_check --terrain <route.csv> <terrain.json> [sample tiles]
//   ots_check --safety <route> <train folder> <obey|ack|ignore|redpass> [traffic.json|none] [minutes] [safety.json]
#include "audio/SoundCfg.hpp"
#include "audio/Vvvf.hpp"
#include "bve/ObjectLibrary.hpp"
#include "bve/Route.hpp"
#include "bve/Train.hpp"
#include "phx/core/Log.hpp"
#include "phx/core/Wav.hpp"
#include "route/NativeRoute.hpp"
#include "sim/Traffic.hpp"
#include "sim/World.hpp"
#include "terrain/Terrain.hpp"
#include "util/Paths.hpp"
#include "util/Text.hpp"
#include "util/ThreadPool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <thread>

using namespace ots;

namespace {

using Args = std::span<const std::string_view>; // the mode's own arguments (after the mode flag)

template <class... T>
void print(std::format_string<T...> fmt, T&&... args) {
    std::cout << std::format(fmt, std::forward<T>(args)...);
}

std::filesystem::path pathOf(std::string_view s) { return std::filesystem::path(s); }

double seconds(std::chrono::steady_clock::time_point since) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count(); }

// FNV-1a over quantised values: a regression fingerprint, stable across runs.
struct Fingerprint {
    u64 h = 1469598103934665603ull;
    void bytes(std::span<const std::byte> data) {
        for (std::byte c : data) h = (h ^ u64(c)) * 1099511628211ull;
    }
    template <class T>
    void raw(const T& v) { bytes(std::as_bytes(std::span(&v, 1))); }
    void add(double v) { raw(i64(std::llround(v * 1000.0))); }
    void add(float v) { add(double(v)); }
    void add(std::optional<double> v) { add(v.value_or(-1.0)); }
    void add(u32 v) { raw(v); }
    void add(i32 v) { raw(v); }
    void add(std::size_t v) { raw(u64(v)); }
    void add(std::string_view s) { bytes(std::as_bytes(std::span(s))); add(s.size()); }
    void add(const glm::vec3& v) { add(v.x), add(v.y), add(v.z); }
    void add(const glm::vec4& v) { add(v.x), add(v.y), add(v.z), add(v.w); }
    void add(const glm::dvec3& v) { add(v.x), add(v.y), add(v.z); }
};

bool writeWav(const std::filesystem::path& file, u32 rate, std::span<const std::int16_t> pcm) {
    std::ofstream f(file, std::ios::binary);
    auto put = [&](auto v) { f.write(reinterpret_cast<const char*>(&v), sizeof v); }; // little endian
    const u32 bytes = u32(pcm.size_bytes());
    f << "RIFF", put(u32(36 + bytes)), f << "WAVEfmt ", put(u32(16)), put(u16(1)), put(u16(1)), put(rate), put(rate * 2), put(u16(2)), put(u16(16));
    f << "data", put(bytes), f.write(reinterpret_cast<const char*>(pcm.data()), std::streamsize(bytes));
    return bool(f);
}

// ------------------------------------------------------------------ modes

int vvvf(Args a) {
    auto preset = VvvfPreset::load(pathOf(a[0]));
    if (!preset) return phx::log::error("{}: {}", a[0], preset.error()), 1;
    const double top = (a.size() > 2 ? parseDouble(a[2]).value_or(100.0) : 100.0) / 3.6;
    constexpr int kRate = 44100, kBlock = 441; // 10 ms control steps, like a game frame
    VvvfSynth synth(std::move(*preset), kRate);
    std::vector<std::int16_t> pcm, block(kBlock);
    double v = 0.0;
    std::string lastStage;
    const auto run = [&](int dir, double accel, double duration) {
        for (int i = 0; i < int(duration * 100); ++i) {
            v = std::clamp(v + accel * 0.01, 0.0, top);
            synth.set(v, dir, 1.0);
            synth.render(block);
            pcm.insert(pcm.end(), block.begin(), block.end());
            if (std::string st = dir ? synth.stageName() : "off"; st.substr(0, 5) != lastStage.substr(0, 5) || (st[0] != 'a' && st != lastStage)) {
                phx::log::info("{:6.1f} s {:5.1f} km/h {:6.1f} Hz  {}", double(pcm.size()) / kRate, v * 3.6, synth.fundamentalHz(), st);
                lastStage = st;
            }
        }
    };
    run(1, 0.9, top / 0.9);   // power from standstill to top speed
    run(0, 0.0, 1.5);         // coast
    run(-1, -1.0, top / 1.0); // brake to a stop
    const bool ok = writeWav(pathOf(a[1]), kRate, pcm);
    phx::log::info("{}: {:.1f} s written", a[1], double(pcm.size()) / kRate);
    return ok ? 0 : 1;
}

// Builds the terrain, then times a fixed sample of tile / tunnel meshes and prints a checksum.
int terrain(Args a) {
    ObjectLibrary lib;
    auto loaded = loadAnyRoute(pathOf(a[0]), lib);
    if (!loaded) return phx::log::error("{}", loaded.error()), 1;
    Route& route = loaded->route;
    const auto t0 = std::chrono::steady_clock::now();
    const auto settings = loadTerrainSettings(pathOf(a[1]), route);
    if (!settings) return phx::log::error("{}", settings.error()), 1;
    const auto terrainStats = buildTerrain(route, lib, *settings, pathOf(a[1]).parent_path());
    if (!terrainStats) return phx::log::error("{}", terrainStats.error()), 1;
    const TerrainStats& st = *terrainStats;
    print("setup {:.2f} s: {} tiles, {} tunnels ({:.1f} km), route tunnels {:.1f} km, {} bridges, DEM {} tiles\n", seconds(t0), st.tiles, st.tunnels,
          st.tunnelLength / 1000.0, st.routeTunnelLength / 1000.0, st.bridges, st.demTiles);
    std::vector<std::pair<std::string, ObjectId>> procedural;
    for (ObjectId id = 0; id < lib.size(); ++id)
        if (lib[id].build) procedural.emplace_back(lib[id].path.generic_string(), id);
    std::ranges::sort(procedural);
    const std::size_t want = a.size() > 2 ? std::size_t(std::max(1, parseIntVb6(a[2]).value_or(400))) : 400;
    const std::size_t step = std::max<std::size_t>(1, procedural.size() / want);
    double sum = 0.0;
    std::size_t verts = 0, tris = 0, built = 0;
    const auto t1 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < procedural.size(); i += step) {
        const ObjectId id = procedural[i].second;
        if (!lib.load(id) || !lib[id].mesh) continue;
        for (const SubMesh& p : lib[id].mesh->parts) {
            verts += p.vertices.size(), tris += p.indices.size() / 3;
            for (const MeshVertex& v : p.vertices) sum += v.pos.x + 2.0 * v.pos.y + 3.0 * v.pos.z + v.normal.y;
        }
        lib.releaseMesh(id, true);
        ++built;
    }
    const double ms = seconds(t1) * 1000.0;
    print("built {} meshes in {:.0f} ms ({:.2f} ms each): {} vertices, {} triangles, checksum {:.6e}\n", built, ms, ms / double(built), verts, tris, sum);
    return 0;
}

// Runs AI traffic headless and prints every train once a minute.
int traffic(Args a) {
    ObjectLibrary lib;
    auto loaded = loadAnyRoute(pathOf(a[0]), lib);
    if (!loaded) return phx::log::error("{}", loaded.error()), 1;
    const Route& route = loaded->route;
    const auto def = loadTrain(pathOf(a[1]), lib);
    if (!def) return phx::log::error("{}", def.error()), 1;
    const auto tc = loadTrafficConfig(pathOf(a[2]));
    if (!tc) return phx::log::error("{}: {}", a[2], tc.error()), 1;
    Simulation sim(route, *def);
    std::deque<TrainDef> defs; // stable addresses
    for (const TrafficEntry& e : tc->trains) {
        const TrainDef* d = &*def;
        if (!e.trainFolder.empty()) {
            auto other = loadTrain(e.trainFolder, lib);
            if (!other) return phx::log::error("{}", other.error()), 1;
            d = &defs.emplace_back(std::move(*other));
        }
        sim.addTraffic(*d, e.options);
    }
    sim.start();
    const double minutes = a.size() > 3 ? parseDouble(a[3]).value_or(30.0) : 30.0;
    const bool drive = a.size() > 4 && a[4] == "drive";
    auto report = [&] {
        const int h = int(sim.time / 3600), m = int(std::fmod(sim.time, 3600) / 60), sec = int(std::fmod(sim.time, 60));
        print("{:02}:{:02}:{:02} player {:8.0f} m {:5.1f} km/h", h, m, sec, sim.train->front, sim.train->speed * 3.6);
        if (const auto sig = sim.signalAhead()) print(" (signal {} in {:.0f} m)", sig->aspect, sig->distance);
        print("\n");
        for (const TrafficTrain& t : sim.traffic) {
            using S = TrafficTrain::State;
            const std::string_view state = t.state() == S::Waiting ? "waiting" : t.state() == S::Done ? "done" : "running";
            const TrainSim& tr = t.train();
            const auto at = t.stoppedAt();
            print("         {:<10} {:<8} {:8.0f} m {:5.1f} km/h P{} B{}{}\n", t.name(), state, tr.front, tr.speed * 3.6, tr.powerNotch, tr.brakeNotch,
                  at ? " at " + route.stations[*at].name : std::string());
        }
    };
    report();
    constexpr double dt = 1.0 / 30.0;
    double next = 60.0;
    for (double elapsed = 0; elapsed < minutes * 60.0; elapsed += dt) {
        TrainSim& p = *sim.train;
        if (drive) { // crude driver: obey the limit (signals included), depart on time
            p.reverser = 1;
            const StationProgress* sp = sim.nextStation ? &sim.stations[*sim.nextStation] : nullptr;
            const double lim = sim.speedLimit();
            const auto sig = sim.signalAhead();
            const bool red = sig && sig->aspect == 0 && sig->distance < std::max(150.0, p.speed * p.speed / 1.0);
            const bool waiting = sp && sp->arrived && !sp->departed;
            const bool fast = std::abs(p.speed) > lim - 2.0 / 3.6;
            p.powerNotch = waiting || red || fast ? 0 : p.maxPower();
            p.brakeNotch = waiting || red ? p.maxBrake() : (std::abs(p.speed) > lim ? 3 : 0);
            if (waiting && sim.time >= route.stations[*sim.nextStation].departure.value_or(-1.0)) p.brakeNotch = 0;
        }
        sim.update(dt);
        if (elapsed >= next) next += 60.0, report();
    }
    return 0;
}

// Drives the player train under the built-in train protection with a scripted driver and
// logs every protection event; ends with intervention counts (a regression record).
//   obey     keeps to the line speed, the code speed and the signals: expect no intervention
//   ack      ignores signal speeds but acknowledges alarms and brakes to the code speed
//   ignore   ignores signal speeds and never acknowledges: alarm → emergency
//   redpass  obeys speeds but does not stop at red signals: emergency at the signal
int safetyRun(Args a) {
    ObjectLibrary lib;
    auto loaded = loadAnyRoute(pathOf(a[0]), lib);
    if (!loaded) return phx::log::error("{}", loaded.error()), 1;
    const Route& route = loaded->route;
    const auto def = loadTrain(pathOf(a[1]), lib);
    if (!def) return phx::log::error("{}", def.error()), 1;
    const std::string_view scenario = a[2];
    if (scenario != "obey" && scenario != "ack" && scenario != "ignore" && scenario != "redpass")
        return phx::log::error("scenario: obey | ack | ignore | redpass"), 1;
    const std::filesystem::path configFile = a.size() > 5 ? pathOf(a[5]) : resolvePath(pathOf(a[1]), "safety.json");
    auto config = configFile.empty() ? Expected<safety::SafetyConfig>(safety::defaultConfig()) : safety::loadConfig(configFile);
    if (!config) return phx::log::error("safety.json: {}", config.error()), 1;
    Simulation sim(route, *def);
    std::deque<TrainDef> defs;
    if (a.size() > 3 && a[3] != "none") {
        const auto tc = loadTrafficConfig(pathOf(a[3]));
        if (!tc) return phx::log::error("{}: {}", a[3], tc.error()), 1;
        for (const TrafficEntry& e : tc->trains) {
            const TrainDef* d = &*def;
            if (!e.trainFolder.empty()) {
                auto other = loadTrain(e.trainFolder, lib);
                if (!other) return phx::log::error("{}", other.error()), 1;
                d = &defs.emplace_back(std::move(*other));
            }
            sim.addTraffic(*d, e.options);
        }
    }
    sim.start();
    sim.onboard = std::make_unique<safety::Onboard>(*config);
    const double minutes = a.size() > 4 ? parseDouble(a[4]).value_or(20.0) : 20.0;
    auto clock = [&] {
        const int s = int(sim.time);
        return std::format("{:02}:{:02}:{:02}", s / 3600, (s / 60) % 60, s % 60);
    };
    constexpr double dt = 1.0 / 30.0, comfort = 0.7; // m/s²
    std::string lastMessage, lastCode;
    bool ackHeld = false;
    for (double elapsed = 0; elapsed < minutes * 60.0; elapsed += dt) {
        TrainSim& p = *sim.train;
        const safety::Display& d = sim.onboard->display();
        const double v = std::abs(p.speed);
        p.reverser = 1;
        // target speed: line speed, then (unless ignored) the code / signal speeds with braking curves
        double target = route.speedLimitAt(p.front) - 3.0 / 3.6;
        double stopAt = std::numeric_limits<double>::infinity(); // distance to the next place to stand
        auto curve = [&](double speedThere, double distance) { return std::sqrt(speedThere * speedThere + 2.0 * comfort * std::max(0.0, distance)); };
        if (scenario == "obey" || scenario == "redpass") {
            if (d.permitted) target = std::min(target, *d.permitted - 6.0 / 3.6);
            // announced lower speed ahead (TVM next block, ATP target). A stop target whose signal
            // has cleared since (ATP point transmission) is approached at the release speed instead.
            const auto ahead = sim.signalAhead();
            const bool stopStillShown = !ahead || ahead->aspect == 0;
            if (d.target && d.targetDistance && *d.target > 6.0 / 3.6)
                target = std::min(target, curve(*d.target - 6.0 / 3.6, *d.targetDistance - 80.0));
            else if (d.target && d.targetDistance && stopStillShown)
                target = std::min(target, curve(0.0, *d.targetDistance - 30.0));
            if (const auto sig = sim.signalAhead()) {
                const double sp = sig->aspect >= 0 && std::size_t(sig->aspect) < route.signalSpeeds.size() ? route.signalSpeeds[std::size_t(sig->aspect)] : 1e9;
                if (sig->aspect == 0 && scenario == "obey") stopAt = std::min(stopAt, sig->distance - 20.0);
                else if (std::isfinite(sp)) target = std::min(target, curve(sp - 5.0 / 3.6, sig->distance - 60.0));
            }
        }
        // stations: stop, open the doors, leave on time
        bool hold = false;
        if (const auto next = sim.nextStation) {
            const Station& st = route.stations[*next];
            StationProgress& sp = sim.stations[*next];
            if (st.stopsHere() && !sp.arrived && sim.distanceToStop(*next) > -4.0) stopAt = std::min(stopAt, sim.distanceToStop(*next)); // overran: go on
            if (sp.arrived && !sp.departed) {
                hold = true;
                if (!sp.doorsOpened) (st.doors < 0 ? p.leftDoorsTarget : p.rightDoorsTarget) = true;
                else if (st.departure && sim.time >= *st.departure - 5.0) p.leftDoorsTarget = p.rightDoorsTarget = false;
            }
        }
        // acknowledge: alarms (ack scenario), zone changes and resets at a standstill (all)
        const bool wantAck = d.ackRequired && (scenario != "ignore" || (d.emergency && v < 0.01) || !d.warning);
        if (wantAck != ackHeld) sim.pluginKey(0, wantAck), ackHeld = wantAck;
        if (scenario == "ack" && d.permitted && v > *d.permitted) target = std::min(target, *d.permitted - 5.0 / 3.6);
        // controls: a stop within reach → brake for exactly that distance; else hold the target speed
        const double notchDecel = std::max(0.05, p.def().spec.deceleration / p.maxBrake());
        if (hold) {
            p.powerNotch = 0, p.brakeNotch = p.maxBrake();
        } else if (v * v / (2.0 * 0.45) >= stopAt - 10.0 || stopAt < 3.0) { // brake build-up lags: start gently, early
            const double need = v * v / (2.0 * std::max(stopAt, 0.3));
            p.powerNotch = 0;
            p.brakeNotch = stopAt < 3.0 && v < 0.3 ? p.maxBrake() : std::clamp(int(std::ceil(need / notchDecel)), v < 0.01 ? p.maxBrake() : 1, p.maxBrake());
            if (stopAt > 3.0 && v < 2.0) p.brakeNotch = 0, p.powerNotch = 1; // creep up to the stop
        } else {
            const double excess = v - target;
            p.powerNotch = v < target - 2.0 / 3.6 ? p.maxPower() : 0;
            p.brakeNotch = excess > 0 ? std::clamp(int(std::ceil(excess / (1.0 / 3.6) * p.maxBrake() / 4.0)), 1, p.maxBrake()) : 0;
        }
        if (scenario == "ack" && d.permitted && v > *d.permitted) p.brakeNotch = std::max(p.brakeNotch, 3); // keeps braking once alarmed
        sim.update(dt);
        if (d.message != lastMessage || d.code != lastCode) {
            print("{} {:8.1f} m {:5.1f} km/h {:<6} {:<4} {}{}\n", clock(), p.front, v * 3.6, sim.onboard->active() ? safety::label(*sim.onboard->active()) : "-",
                  d.code, d.message, d.braking ? (d.emergency ? " [EB]" : " [B]") : "");
            lastMessage = d.message, lastCode = d.code;
        }
        if (p.front > route.endPosition() - 200.0) break;
    }
    if (std::getenv("OTS_SAFETY_DEBUG")) {
        const auto sig = sim.signalAhead();
        const auto n = sim.nextStation;
        print("debug: signal {} aspect {} in {:.0f} m; next station {} arrived {} doors {} departed {}; doors L{} R{}\n", sig ? int(sig->section) : -1,
              sig ? sig->aspect : -1, sig ? sig->distance : 0.0, n ? int(*n) : -1, n && sim.stations[*n].arrived, n && sim.stations[*n].doorsOpened,
              n && sim.stations[*n].departed, sim.train->leftDoors, sim.train->rightDoors);
    }
    print("end {} at {:.0f} m, {:.1f} km/h: {} interventions, {} emergency\n", clock(), sim.train->front, sim.train->speed * 3.6,
          sim.onboard->interventions, sim.onboard->emergencies);
    return 0;
}

int sounds(Args a) {
    TrainSoundFiles sounds;
    const bool loaded = loadTrainSounds(pathOf(a[0]), sounds);
    std::set<std::filesystem::path> files;
    const auto indexed = [&](std::string_view section, const auto& entries) {
        phx::log::info("[{}] {} sounds", section, entries.size());
        for (const auto& entry : entries) files.insert(entry.second);
    };
    const auto named = [&](std::string_view section, std::initializer_list<const std::filesystem::path*> entries) {
        unsigned count = 0;
        for (const auto* path : entries)
            if (!path->empty()) ++count, files.insert(*path);
        phx::log::info("[{}] {} sounds", section, count);
    };
    indexed("Run", sounds.run);
    indexed("Flange", sounds.flange);
    indexed("Motor", sounds.motor);
    indexed("Switch", sounds.pointSwitch);
    indexed("Ats", sounds.ats);
    named("Brake", {&sounds.bcReleaseHigh, &sounds.bcRelease, &sounds.bcReleaseFull, &sounds.emergency, &sounds.emergencyRelease, &sounds.bpDecomp});
    named("Compressor", {&sounds.compressorAttack, &sounds.compressorLoop, &sounds.compressorRelease});
    named("Suspension", {&sounds.springLeft, &sounds.springRight});
    named("Horn", {&sounds.hornPrimary, &sounds.hornPrimaryStart, &sounds.hornPrimaryLoop, &sounds.hornPrimaryEnd, &sounds.hornSecondary,
                   &sounds.hornSecondaryStart, &sounds.hornSecondaryLoop, &sounds.hornSecondaryEnd, &sounds.hornMusic, &sounds.hornMusicStart,
                   &sounds.hornMusicLoop, &sounds.hornMusicEnd});
    named("Door", {&sounds.doorOpenLeft, &sounds.doorOpenRight, &sounds.doorCloseLeft, &sounds.doorCloseRight});
    named("Buzzer", {&sounds.buzzerCorrect});
    named("Pilot Lamp", {&sounds.pilotLampOn, &sounds.pilotLampOff});
    named("Brake Handle", {&sounds.brakeApply, &sounds.brakeApplyFast, &sounds.brakeRelease, &sounds.brakeReleaseFast, &sounds.brakeMin, &sounds.brakeMax});
    named("Master Controller", {&sounds.powerUp, &sounds.powerUpFast, &sounds.powerDown, &sounds.powerDownFast, &sounds.powerMin, &sounds.powerMax});
    named("Reverser", {&sounds.reverserOn, &sounds.reverserOff});
    named("Breaker", {&sounds.breakerOn, &sounds.breakerOff});
    named("Others", {&sounds.noise, &sounds.shoe, &sounds.halt});
    for (const auto& warning : sounds.warnings) phx::log::warn("{}", warning);
    if (!loaded) phx::log::error("{}: no sound.cfg or BVE2 sound files found", a[0]);
    bool ok = loaded;
    for (const auto& file : files) {
        phx::PcmMono16 pcm;
        std::string error;
        if (phx::loadWav(file, pcm, error)) {
            phx::log::info("{}: {} Hz, {:.3f} s, ok", file.string(), pcm.sampleRate, pcm.duration());
        } else {
            phx::log::error("{}: 0 Hz, 0.000 s, error: {}", file.string(), error);
            ok = false;
        }
    }
    return ok ? 0 : 1;
}

int selftest(Args) {
    struct Ctx : FunctionContext {
        double variable(Var v, int i) const override { return v == Var::Speed ? 10.0 : (v == Var::PluginState ? i * 2.0 : 0.0); }
    } ctx;
    constexpr std::pair<std::string_view, double> kCases[] = {
        {"1+2*3", 7},        {"(1+2)*3", 9},   {"-2^0", 1},       {"if[speed > 5, 1, 2]", 1}, {"0 == 0 < 1.3", 1},
        {"pluginState[3] - 1", 5}, {"min[3, 1, 2]", 1}, {"mod[7, 3]", 1}, {"!0 & 1", 1}, {"speed * 3.6", 36}, {"-if[0==0 < 1.3, 2, 0]*0.5", -1},
    };
    int fails = 0;
    for (auto [src, want] : kCases) {
        std::string e;
        auto f = Function::compile(std::string(src), &e);
        const double got = f ? f->evaluate(ctx, 0.0) : NAN;
        if (!f || std::abs(got - want) > 1e-9) {
            ++fails;
            phx::log::error("selftest: {} = {} (want {}) {}", src, got, want, e);
        }
    }
    phx::log::info("function script selftest: {} failures", fails);
    return fails ? 1 : 0;
}

int dump(Args files) {
    ObjectLibrary lib;
    for (std::string_view file : files) {
        const ObjectId id = lib.intern(pathOf(file));
        if (!lib.load(id)) {
            phx::log::error("{}: {}", file, lib[id].error);
            continue;
        }
        const ObjectRecord& r = lib[id];
        phx::log::info("{}: {} tris, radius {:.1f}, bounds ({:.2f},{:.2f},{:.2f})-({:.2f},{:.2f},{:.2f}) {}", file, r.triangles, r.radius, r.bmin.x, r.bmin.y,
                       r.bmin.z, r.bmax.x, r.bmax.y, r.bmax.z, r.error);
        if (r.mesh)
            for (const SubMesh& p : r.mesh->parts)
                phx::log::info("  part: {} verts {} tris color {:08x} emissive {:06x} key {} blend {} wrap {} 2side {} tex '{}'", p.vertices.size(),
                               p.indices.size() / 3, p.material.color, p.material.emissive, p.material.colorKey, int(p.material.blend), int(p.material.wrap),
                               p.doubleSided, p.material.texture);
        if (r.anim)
            for (const AnimatedPart& p : r.anim->parts) {
                std::string states;
                for (ObjectId s : p.states) states += (s == kNoObject ? std::string("-") : lib[s].path.filename().string()) + " ";
                phx::log::info("  anim part pos ({:.2f},{:.2f},{:.2f}) states [{}] fn {}", p.position.x, p.position.y, p.position.z, states,
                               p.stateFn ? p.stateFn->source() : "");
            }
    }
    return 0;
}

// Loads every object of the route on all cores; logs totals and a mesh fingerprint.
void loadAllObjects(ObjectLibrary& lib) {
    ThreadPool pool(std::max(2u, std::thread::hardware_concurrency()));
    const auto t0 = std::chrono::steady_clock::now();
    std::atomic<u64> tris{0}, verts{0}, meshSum{0};
    auto load = [&](ObjectId id) {
            if (!lib.load(id)) return;
            tris += lib[id].triangles;
            if (const auto& mesh = lib[id].mesh) {
                Fingerprint fp;
                fp.add(lib[id].path.generic_string());
                for (const SubMesh& p : mesh->parts) {
                    verts += p.vertices.size();
                    fp.add(p.material.color), fp.add(p.material.emissive), fp.add(p.material.texture), fp.add(u32(p.material.blend)), fp.add(u32(p.doubleSided));
                    for (const MeshVertex& v : p.vertices) fp.add(v.pos), fp.add(v.normal), fp.add(glm::vec3(v.uv, 0.0f));
                    for (u32 i : p.indices) fp.add(i);
                }
                meshSum += fp.h; // order-independent across threads
            }
            lib.releaseMesh(id, false);
        };
    // in rounds: loading .animated files interns their state objects, which the next round loads
    for (std::size_t done = 0, n; (n = lib.size()) > done; done = n) {
        for (ObjectId id = ObjectId(done); id < n; ++id) pool.submit([&, id] { load(id); });
        pool.waitIdle();
    }
    phx::log::info("objects fingerprint {:016x}", meshSum.load());
    phx::log::info("objects: {} parsed, {} failed, {} triangles, {} vertices, {:.2f} s", lib.parseCount.load(), lib.failCount.load(), tris.load(), verts.load(),
                   seconds(t0));
}

int route(std::string_view file, bool objects) {
    ObjectLibrary lib;
    auto loaded = loadAnyRoute(pathOf(file), lib);
    if (!loaded) return phx::log::error("{}", loaded.error()), 1;
    const Route& route = loaded->route;
    const RouteLoadStats& st = loaded->stats;
    phx::log::info("route: {} files, {} expressions, {} blocks ({:.1f} km), {} placements, {} objects interned, {} unknown cmds, "
                   "{} missing objects, {:.2f} s",
                   st.files, st.expressions, route.blocks.size(), route.endPosition() / 1000.0, st.placements, lib.size(), st.unknownCommands,
                   st.missingObjects, st.parseSeconds);
    phx::log::info("stations {}, sections {}, limits {}, brightness {}, backgrounds {}, train folder '{}'", route.stations.size(), route.sections.size(),
                   route.limits.size(), route.brightness.size(), route.backgrounds.size(), route.trainFolder);
    for (const auto& [k, n] : st.unknown) phx::log::info("  unknown command {} x{}", k, n);
    for (std::size_t i = 0; i < route.stations.size(); ++i) {
        const Station& s = route.stations[i];
        phx::log::info("  sta {:2} {:<40} pos {:9.1f} arr {:7.0f} dep {:7.0f} stops {} {}", i, s.name, s.trackPos, s.arrival.value_or(-1.0),
                       s.departure.value_or(-1.0), s.stops.size(), s.stopsHere() ? "" : "(pass)");
    }
    { // regression fingerprint of the parsed geometry (quantised, order-sensitive)
        Fingerprint fp;
        for (const RouteBlock& b : route.blocks) fp.add(b.origin), fp.add(b.pitch), fp.add(b.curveRadius), fp.add(b.cant), fp.add(b.placementCount);
        for (const Placement& p : route.placements)
            fp.add(p.pos), fp.add(glm::vec4(p.rot.x, p.rot.y, p.rot.z, p.rot.w)), fp.add(p.trackOffset), fp.add(p.section ? i32(*p.section) : -1),
                fp.add(lib[p.object].path.generic_string());
        for (const Station& s : route.stations) fp.add(s.trackPos), fp.add(s.arrival), fp.add(s.departure), fp.add(s.stops.size());
        for (const Section& s : route.sections) fp.add(s.trackPos), fp.add(s.aspects.size());
        phx::log::info("route fingerprint {:016x}", fp.h);
    }
    const auto end = route.poseAt(route.endPosition() - 1.0);
    phx::log::info("end of track at ({:.0f}, {:.0f}, {:.0f})", end.position.x, end.position.y, end.position.z);
    // train + a short headless drive
    const auto folder = findTrainFolder(route.file, route.trainFolderHint, route.trainFolder);
    const auto loadedTrain = folder.empty() ? Expected<TrainDef>(Unexpected("folder not found")) : loadTrain(folder, lib);
    if (!loadedTrain) {
        phx::log::warn("train '{}' not loaded: {}", route.trainFolder, loadedTrain.error());
    } else {
        const TrainDef& train = *loadedTrain;
        phx::log::info("train {}: {} cars, {:.1f} m, {:.0f} t, {} power / {} brake notches, cab {}", train.name, train.cars.size(), train.length(),
                       train.totalMass() / 1000.0, train.spec.powerNotches, train.spec.brakeNotches, train.cab != kNoObject ? "3D" : "none");
        for (const auto& w : train.warnings) phx::log::warn("  {}", w);
        Simulation sim(route, train);
        sim.start();
        phx::log::info("start: front {:.1f} m, time {:02}:{:02}, next station {}", sim.train->front, int(sim.time / 3600), int(sim.time / 60) % 60,
                       sim.nextStation ? int(*sim.nextStation) : -1);
        sim.time = 34200; // departure
        for (int k = 0; k < 20; ++k) sim.train->command(Command::BrakeDecrease);
        sim.train->command(Command::ReverserForward);
        for (int k = 0; k < 20; ++k) sim.train->command(Command::PowerIncrease);
        for (int sec = 1; sec <= 600; ++sec) {
            sim.update(1.0);
            if (sec % 60 == 0) {
                const auto section = route.sectionAt(sim.train->front);
                phx::log::info("  t+{:3}s v={:5.1f} km/h a={:5.2f} km/h/s pos={:8.1f} limit={:.0f} signal-sec={}", sec, sim.train->speed * 3.6,
                               sim.train->acceleration * 3.6, sim.train->front, sim.speedLimit() * 3.6, section ? int(*section) : -1);
            }
        }
    }
    if (objects) loadAllObjects(lib);
    return 0;
}

struct Mode {
    std::size_t minArgs;
    std::string_view usage;
    std::function<int(Args)> run;
};

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    const std::map<std::string_view, Mode> modes{
        {"--vvvf", {2, "--vvvf <preset.json> <out.wav> [top km/h]", vvvf}},
        {"--terrain", {2, "--terrain <route.csv> <terrain.json> [sample tiles]", terrain}},
        {"--traffic", {3, "--traffic <route.csv> <train folder> <traffic.json> [minutes] [stay|drive]", traffic}},
        {"--sounds", {1, "--sounds <train folder>", sounds}},
        {"--safety", {3, "--safety <route> <train folder> <obey|ack|ignore|redpass> [traffic.json|none] [minutes] [safety.json]", safetyRun}},
        {"--selftest", {0, "--selftest", selftest}},
        {"--dump", {1, "--dump <object>...", dump}},
    };
    if (args.empty()) {
        std::cerr << "usage: ots_check <route.csv> [--objects]\n";
        return 1;
    }
    if (const auto it = modes.find(args[0]); it != modes.end()) {
        const Args rest = Args(args).subspan(1);
        if (rest.size() < it->second.minArgs) {
            std::cerr << "usage: ots_check " << it->second.usage << '\n';
            return 1;
        }
        return it->second.run(rest);
    }
    return route(args[0], args.size() > 1 && args[1] == "--objects");
}
