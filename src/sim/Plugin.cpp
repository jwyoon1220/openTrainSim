#include "sim/Plugin.hpp"

#include "phx/core/Log.hpp"
#include "util/Paths.hpp"
#include "util/Text.hpp"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <format>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace ots {

std::filesystem::path pluginHostPath() {
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = 0;
    return std::filesystem::path(buf).parent_path() / "PluginHost.exe";
}

std::filesystem::path findTrainPlugin(const std::filesystem::path& folder) {
    const std::filesystem::path cfg = resolvePath(folder, "ats.cfg");
    std::string text;
    if (!cfg.empty() && readTextFile(cfg, text)) {
        for (const std::string& raw : splitLines(text)) {
            const std::string_view l = trim(raw);
            if (l.empty() || l.front() == ';') continue;
            const std::filesystem::path p = resolvePath(folder, l);
            if (!p.empty()) return p;
        }
    }
    return {};
}

TrainPlugin::~TrainPlugin() { stop(); }

bool TrainPlugin::start(const std::filesystem::path& hostExe, const std::filesystem::path& dll, const std::filesystem::path& trainFolder,
                        int powerNotches, int brakeType, int brakeNotches, bool holdBrake, int cars, int initMode,
                        const std::vector<PluginStation>& stations, std::string& error) {
    name = dll.filename().string();
    if (!std::filesystem::exists(hostExe)) {
        error = "PluginHost.exe not built (needs mono + OpenBveApi.dll at configure time)";
        return false;
    }
    int toHost[2], fromHost[2];
    if (pipe(toHost) != 0 || pipe(fromHost) != 0) {
        error = "pipe() failed";
        return false;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, toHost[0], 0);
    posix_spawn_file_actions_adddup2(&fa, fromHost[1], 1);
    posix_spawn_file_actions_addclose(&fa, toHost[1]);
    posix_spawn_file_actions_addclose(&fa, fromHost[0]);
    const std::string exe = hostExe.string(), plug = dll.string(), folder = trainFolder.string();
    const char* mono = "mono";
    char* argv[] = {const_cast<char*>(mono), const_cast<char*>("--debug"), const_cast<char*>(exe.c_str()), const_cast<char*>(plug.c_str()),
                    const_cast<char*>(folder.c_str()), nullptr};
    const int rc = posix_spawnp(&pid_, mono, &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(toHost[0]);
    close(fromHost[1]);
    in_ = toHost[1];
    out_ = fromHost[0];
    if (rc != 0) {
        pid_ = -1;
        error = std::format("cannot start mono: {}", std::strerror(rc));
        stop();
        return false;
    }
    std::signal(SIGPIPE, SIG_IGN);
    for (const PluginStation& s : stations)
        send(std::format("STATION {}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", s.name, s.arrival, s.departure, s.stop, s.forward, s.backward,
                         int(s.left), int(s.right), s.mode));
    send(std::format("LOAD {} {} {} {} {} {}", powerNotches, brakeType, brakeNotches, int(holdBrake), cars, initMode));
    std::string reply;
    while (readLine(reply)) {
        if (reply.starts_with("ERR ")) {
            lastError = reply.substr(4);
            continue;
        }
        break;
    }
    if (!reply.starts_with("OK")) {
        error = reply.empty() ? "plugin host exited" : reply.substr(reply.starts_with("FAIL ") ? 5 : 0);
        if (!lastError.empty()) error += " (" + lastError + ")";
        stop();
        return false;
    }
    int n = 0;
    parseIntVb6(std::string_view(reply).substr(2), n);
    panel.assign(std::size_t(std::max(0, n)), 0);
    return true;
}

bool TrainPlugin::send(const std::string& line) {
    if (in_ < 0) return false;
    std::string l = line;
    l.push_back('\n');
    const char* p = l.data();
    std::size_t left = l.size();
    while (left) {
        const ssize_t w = write(in_, p, left);
        if (w <= 0) {
            stop();
            return false;
        }
        p += w, left -= std::size_t(w);
    }
    return true;
}

bool TrainPlugin::readLine(std::string& line) {
    for (;;) {
        const auto nl = buffer_.find('\n');
        if (nl != std::string::npos) {
            line = buffer_.substr(0, nl);
            buffer_.erase(0, nl + 1);
            return true;
        }
        if (out_ < 0) return false;
        pollfd pf{out_, POLLIN, 0};
        if (poll(&pf, 1, 5000) <= 0) { // a plugin that hangs for 5 s is dropped
            lastError = "plugin host timed out";
            stop();
            return false;
        }
        char buf[65536];
        const ssize_t r = read(out_, buf, sizeof(buf));
        if (r <= 0) {
            stop();
            return false;
        }
        buffer_.append(buf, std::size_t(r));
    }
}

bool TrainPlugin::elapse(const PluginVehicle& v, PluginHandles& out) {
    sounds.clear();
    if (!running()) return false;
    const PluginHandles& d = v.driver;
    if (!send(std::format("ELAPSE {:.4f} {:.4f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.5f} {:.5f} {:.0f} {:.0f} {} {} {} {} {} {} {} {}",
                          v.location, v.speedKmh, v.bc, v.mr, v.er, v.bp, v.sap, v.radius, v.cant, v.pitch, v.totalMs, v.elapsedMs,
                          d.reverser, d.power, d.brake, int(d.constSpeed), int(d.holdBrake), d.doorInterlock, d.headlights, v.camera)))
        return false;
    std::string line;
    debug.clear();
    while (readLine(line)) {
        if (line == "END") return true;
        if (line.starts_with("H ")) {
            auto f = split(std::string_view(line).substr(2), ' ');
            int x[7] = {};
            for (std::size_t i = 0; i < 7 && i < f.size(); ++i) parseIntVb6(f[i], x[i]);
            out = {x[0], x[1], x[2], x[3] != 0, x[4] != 0, x[5], x[6]};
        } else if (line.starts_with("P ")) {
            auto f = split(std::string_view(line).substr(2), ' ');
            int n = 0;
            if (!f.empty()) parseIntVb6(f[0], n);
            panel.resize(std::size_t(std::max(0, n)));
            for (int i = 0; i < n && std::size_t(i + 1) < f.size(); ++i) parseIntVb6(f[std::size_t(i) + 1], panel[std::size_t(i)]);
        } else if (line.starts_with("MSG ")) {
            auto f = split(std::string_view(line).substr(4), ' ');
            PluginMessage m;
            if (f.size() >= 2) {
                parseIntVb6(f[0], m.color);
                double s;
                if (parseDoubleVb6(f[1], s)) m.seconds = s;
                const std::size_t skip = 4 + f[0].size() + 1 + f[1].size() + 1;
                m.text = skip < line.size() ? line.substr(skip) : std::string();
            }
            messages.push_back(std::move(m));
        } else if (line.starts_with("DOORS ")) {
            const bool open = line.compare(6, 4, "open") == 0;
            const auto f = split(std::string_view(line).substr(open ? 11 : 12), ' ');
            if (f.size() >= 2) {
                if (f[0] == "1") doorRequest |= open ? 1 : 4;
                if (f[1] == "1") doorRequest |= open ? 2 : 8;
            }
        } else if (line.starts_with("SND ")) {
            const auto f = split(std::string_view(line).substr(4), ' ');
            PluginSoundCommand sound;
            if (f.size() == 6 && parseIntVb6(f[0], sound.handle) && parseIntVb6(f[1], sound.car) &&
                parseIntVb6(f[2], sound.index) && parseDouble(f[3], sound.volume) && parseDouble(f[4], sound.pitch) &&
                (f[5] == "0" || f[5] == "1")) {
                sound.looped = f[5] == "1";
                sounds.push_back(sound);
            }
        } else if (line.starts_with("SNDSET ")) {
            const auto f = split(std::string_view(line).substr(7), ' ');
            PluginSoundCommand sound{PluginSoundCommand::Kind::Set};
            if (f.size() == 3 && parseIntVb6(f[0], sound.handle) && parseDouble(f[1], sound.volume) && parseDouble(f[2], sound.pitch))
                sounds.push_back(sound);
        } else if (line.starts_with("SNDSTOP ")) {
            PluginSoundCommand sound{PluginSoundCommand::Kind::Stop};
            if (parseIntVb6(std::string_view(line).substr(8), sound.handle))
                sounds.push_back(sound);
        } else if (line.starts_with("DEBUG ")) {
            debug = line.substr(6);
        } else if (line.starts_with("ERR ")) {
            lastError = line.substr(4);
            phx::log::warn("plugin {}: {}", name, lastError);
        }
    }
    return false;
}

void TrainPlugin::setReverser(int r) { send(std::format("SETREV {}", r)); }
void TrainPlugin::setPower(int p) { send(std::format("SETPOW {}", p)); }
void TrainPlugin::setBrake(int b) { send(std::format("SETBRK {}", b)); }
void TrainPlugin::keyDown(int k) { send(std::format("KEYDOWN {}", k)); }
void TrainPlugin::keyUp(int k) { send(std::format("KEYUP {}", k)); }
void TrainPlugin::horn(int t) { send(std::format("HORN {}", t)); }
void TrainPlugin::doorChange(int o, int n) { send(std::format("DOOR {} {}", o, n)); }
void TrainPlugin::initialize(int mode) { send(std::format("INIT {}", mode)); }
void TrainPlugin::soundEnded(int handle) { send(std::format("SNDEND {}", handle)); }
void TrainPlugin::beacon(int type, int optional, int aspect, double distance) {
    send(std::format("BEACON {} {} {} {:.3f}", type, optional, aspect, std::min(distance, 1.0e9)));
}
void TrainPlugin::signal(const std::vector<std::pair<int, double>>& a) {
    std::string s = std::format("SIGNAL {}", a.size());
    for (auto [asp, d] : a) s += std::format(" {} {:.3f}", asp, std::min(d, 1.0e9));
    send(s);
}

void TrainPlugin::stop() {
    if (in_ >= 0) {
        const char q[] = "QUIT\n";
        (void)!write(in_, q, sizeof(q) - 1);
        close(in_);
        in_ = -1;
    }
    if (out_ >= 0) {
        close(out_);
        out_ = -1;
    }
    if (pid_ > 0) {
        int status = 0;
        for (int i = 0; i < 300 && waitpid(pid_, &status, WNOHANG) == 0; ++i) usleep(10000); // let Unload save its settings
        if (waitpid(pid_, &status, WNOHANG) == 0) {
            kill(pid_, SIGKILL);
            waitpid(pid_, &status, 0);
        }
        pid_ = -1;
    }
}

} // namespace ots
