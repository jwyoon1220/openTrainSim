// Headless checker: parses a route (and optionally loads every referenced object) and prints
// statistics. Usage: ots_check <route.csv> [--objects] or ots_check --sounds <train folder>
#include "audio/SoundCfg.hpp"
#include "audio/Wav.hpp"
#include "bve/ObjectLibrary.hpp"
#include "bve/Route.hpp"
#include "bve/Train.hpp"
#include "sim/World.hpp"
#include "phx/core/Log.hpp"
#include "util/ThreadPool.hpp"

#include <atomic>
#include <cmath>
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <map>
#include <set>
#include <thread>

using namespace ots;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <route.csv> [--objects]\n", argv[0]);
        return 1;
    }
    if (!std::strcmp(argv[1], "--sounds")) {
        if (argc != 3) {
            std::fprintf(stderr, "usage: %s --sounds <train folder>\n", argv[0]);
            return 1;
        }
        TrainSoundFiles sounds;
        const bool loaded = loadTrainSounds(argv[2], sounds);
        std::set<std::filesystem::path> files;
        const auto indexed = [&](const char* section, const auto& entries) {
            phx::log::info("[{}] {} sounds", section, entries.size());
            for (const auto& entry : entries) files.insert(entry.second);
        };
        const auto named = [&](const char* section, std::initializer_list<const std::filesystem::path*> entries) {
            unsigned count = 0;
            for (const auto* path : entries) {
                if (path->empty()) continue;
                ++count;
                files.insert(*path);
            }
            phx::log::info("[{}] {} sounds", section, count);
        };
        indexed("Run", sounds.run);
        indexed("Flange", sounds.flange);
        indexed("Motor", sounds.motor);
        indexed("Switch", sounds.pointSwitch);
        indexed("Ats", sounds.ats);
        named("Brake", {&sounds.bcReleaseHigh, &sounds.bcRelease, &sounds.bcReleaseFull, &sounds.emergency,
               &sounds.emergencyRelease, &sounds.bpDecomp});
        named("Compressor", {&sounds.compressorAttack, &sounds.compressorLoop, &sounds.compressorRelease});
        named("Suspension", {&sounds.springLeft, &sounds.springRight});
        named("Horn", {&sounds.hornPrimary, &sounds.hornPrimaryStart, &sounds.hornPrimaryLoop, &sounds.hornPrimaryEnd,
               &sounds.hornSecondary, &sounds.hornSecondaryStart, &sounds.hornSecondaryLoop, &sounds.hornSecondaryEnd,
               &sounds.hornMusic, &sounds.hornMusicStart, &sounds.hornMusicLoop, &sounds.hornMusicEnd});
        named("Door", {&sounds.doorOpenLeft, &sounds.doorOpenRight, &sounds.doorCloseLeft, &sounds.doorCloseRight});
        named("Buzzer", {&sounds.buzzerCorrect});
        named("Pilot Lamp", {&sounds.pilotLampOn, &sounds.pilotLampOff});
        named("Brake Handle", {&sounds.brakeApply, &sounds.brakeApplyFast, &sounds.brakeRelease,
               &sounds.brakeReleaseFast, &sounds.brakeMin, &sounds.brakeMax});
        named("Master Controller", {&sounds.powerUp, &sounds.powerUpFast, &sounds.powerDown, &sounds.powerDownFast,
               &sounds.powerMin, &sounds.powerMax});
        named("Reverser", {&sounds.reverserOn, &sounds.reverserOff});
        named("Breaker", {&sounds.breakerOn, &sounds.breakerOff});
        named("Others", {&sounds.noise, &sounds.shoe, &sounds.halt});
        for (const auto& warning : sounds.warnings) phx::log::warn("{}", warning);
        if (!loaded) phx::log::error("{}: no sound.cfg or BVE2 sound files found", argv[2]);
        bool ok = loaded;
        for (const auto& file : files) {
            PcmMono16 pcm;
            std::string error;
            if (loadWav(file, pcm, error)) {
                phx::log::info("{}: {} Hz, {:.3f} s, ok", file.string(), pcm.sampleRate, pcm.duration());
            } else {
                phx::log::error("{}: 0 Hz, 0.000 s, error: {}", file.string(), error);
                ok = false;
            }
        }
        return ok ? 0 : 1;
    }
    if (!std::strcmp(argv[1], "--selftest")) {
        struct Ctx : FunctionContext {
            double variable(Var v, int i) const override { return v == Var::Speed ? 10.0 : (v == Var::PluginState ? i * 2.0 : 0.0); }
        } ctx;
        const std::pair<const char*, double> cases[] = {
            {"1+2*3", 7}, {"(1+2)*3", 9}, {"-2^0", 1}, {"if[speed > 5, 1, 2]", 1}, {"0 == 0 < 1.3", 1}, {"pluginState[3] - 1", 5},
            {"min[3, 1, 2]", 1}, {"mod[7, 3]", 1}, {"!0 & 1", 1}, {"speed * 3.6", 36}, {"-if[0==0 < 1.3, 2, 0]*0.5", -1},
        };
        int fails = 0;
        for (auto [src, want] : cases) {
            std::string e;
            auto f = Function::compile(src, &e);
            const double got = f ? f->evaluate(ctx, 0.0) : NAN;
            if (!f || std::abs(got - want) > 1e-9) {
                ++fails;
                phx::log::error("selftest: {} = {} (want {}) {}", src, got, want, e);
            }
        }
        phx::log::info("function script selftest: {} failures", fails);
        return fails ? 1 : 0;
    }
    if (!std::strcmp(argv[1], "--dump") && argc > 2) {
        ObjectLibrary lib;
        for (int i = 2; i < argc; ++i) {
            const ObjectId id = lib.intern(argv[i]);
            if (!lib.load(id)) {
                phx::log::error("{}: {}", argv[i], lib[id].error);
                continue;
            }
            const ObjectRecord& r = lib[id];
            phx::log::info("{}: {} tris, radius {:.1f}, bounds ({:.2f},{:.2f},{:.2f})-({:.2f},{:.2f},{:.2f}) {}", argv[i], r.triangles, r.radius,
                           r.bmin.x, r.bmin.y, r.bmin.z, r.bmax.x, r.bmax.y, r.bmax.z, r.error);
            if (r.mesh)
                for (const SubMesh& p : r.mesh->parts)
                    phx::log::info("  part: {} verts {} tris color {:08x} emissive {:06x} key {} blend {} wrap {} 2side {} tex '{}'", p.vertices.size(),
                                   p.indices.size() / 3, p.material.color, p.material.emissive, p.material.colorKey, int(p.material.blend),
                                   int(p.material.wrap), p.doubleSided, p.material.texture);
            if (r.anim)
                for (const AnimatedPart& p : r.anim->parts) {
                    std::string st;
                    for (ObjectId s : p.states) st += (s == kNoObject ? std::string("-") : lib[s].path.filename().string()) + " ";
                    phx::log::info("  anim part pos ({:.2f},{:.2f},{:.2f}) states [{}] fn {}", p.position.x, p.position.y, p.position.z, st,
                                   p.stateFn ? p.stateFn->source() : "");
                }
        }
        return 0;
    }
    const bool objects = argc > 2 && !std::strcmp(argv[2], "--objects");
    ObjectLibrary lib;
    Route route;
    RouteLoadStats st;
    std::string err;
    if (!loadRoute(argv[1], lib, route, st, err)) {
        phx::log::error("{}", err);
        return 1;
    }
    phx::log::info("route: {} files, {} expressions, {} blocks ({:.1f} km), {} placements, {} objects interned, {} unknown cmds, "
                   "{} missing objects, {:.2f} s",
                   st.files, st.expressions, route.blocks.size(), route.endPosition() / 1000.0, st.placements, lib.size(),
                   st.unknownCommands, st.missingObjects, st.parseSeconds);
    phx::log::info("stations {}, sections {}, limits {}, brightness {}, backgrounds {}, train folder '{}'", route.stations.size(),
                   route.sections.size(), route.limits.size(), route.brightness.size(), route.backgrounds.size(), route.trainFolder);
    for (const auto& [k, n] : st.unknown) phx::log::info("  unknown command {} x{}", k, n);
    for (std::size_t i = 0; i < route.stations.size(); ++i) {
        const Station& s = route.stations[i];
        phx::log::info("  sta {:2} {:<40} pos {:9.1f} arr {:7.0f} dep {:7.0f} stops {} {}", i, s.name, s.trackPos, s.arrival, s.departure,
                       s.stops.size(), s.stopsHere() ? "" : "(pass)");
    }
    const auto end = route.poseAt(route.endPosition() - 1.0);
    phx::log::info("end of track at ({:.0f}, {:.0f}, {:.0f})", end.position.x, end.position.y, end.position.z);
    // train + a short headless drive
    const auto folder = findTrainFolder(route.file, route.trainFolderHint, route.trainFolder);
    TrainDef train;
    if (folder.empty() || !loadTrain(folder, lib, train, err)) {
        phx::log::warn("train '{}' not loaded: {}", route.trainFolder, err);
    } else {
        phx::log::info("train {}: {} cars, {:.1f} m, {:.0f} t, {} power / {} brake notches, cab {}", train.name, train.cars.size(), train.length(),
                       train.totalMass() / 1000.0, train.spec.powerNotches, train.spec.brakeNotches, train.cab != kNoObject ? "3D" : "none");
        for (auto& w : train.warnings) phx::log::warn("  {}", w);
        Simulation sim(route, train);
        sim.start();
        phx::log::info("start: front {:.1f} m, time {:02}:{:02}, next station {}", sim.train->front, int(sim.time / 3600), int(sim.time / 60) % 60,
                       sim.nextStation);
        sim.time = 34200; // departure
        for (int k = 0; k < 20; ++k) sim.train->command(Command::BrakeDecrease);
        sim.train->command(Command::ReverserForward);
        for (int k = 0; k < 20; ++k) sim.train->command(Command::PowerIncrease);
        for (int sec = 1; sec <= 600; ++sec) {
            sim.update(1.0);
            if (sec % 60 == 0)
                phx::log::info("  t+{:3}s v={:5.1f} km/h a={:5.2f} km/h/s pos={:8.1f} limit={:.0f} signal-sec={}", sec, sim.train->speed * 3.6,
                               sim.train->acceleration * 3.6, sim.train->front, sim.speedLimit() * 3.6, route.sectionAt(sim.train->front));
        }
    }
    if (objects) {
        ThreadPool pool(std::max(2u, std::thread::hardware_concurrency()));
        const auto t0 = std::chrono::steady_clock::now();
        std::atomic<u64> tris{0}, verts{0};
        for (ObjectId id = 0; id < lib.size(); ++id)
            pool.submit([&, id] {
                if (lib.load(id)) {
                    tris += lib[id].triangles;
                    if (lib[id].mesh)
                        for (auto& p : lib[id].mesh->parts) verts += p.vertices.size();
                    lib.releaseMesh(id, false);
                }
            });
        pool.waitIdle();
        // animated states interned while loading
        for (ObjectId id = 0; id < lib.size(); ++id)
            if (lib[id].state == LoadState::Unloaded) pool.submit([&, id] { lib.load(id); });
        pool.waitIdle();
        phx::log::info("objects: {} parsed, {} failed, {} triangles, {} vertices, {:.2f} s", lib.parseCount.load(), lib.failCount.load(),
                       tris.load(), verts.load(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }
    return 0;
}
