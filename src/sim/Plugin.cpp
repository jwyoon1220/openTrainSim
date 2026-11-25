#include "sim/Plugin.hpp"

#include "phx/core/Log.hpp"
#include "util/Paths.hpp"
#include "util/Text.hpp"

#include "util/Platform.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace ots {

std::filesystem::path pluginHostPath() {
    const std::filesystem::path exe = executablePath();
    return exe.empty() ? exe : exe.parent_path() / "PluginHost.exe";
}

// ---------------------------------------------------------------- host process and pipes
#ifdef _WIN32
namespace {
// CommandLineToArgvW quoting.
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    for (std::size_t i = 0;; ++i) {
        std::size_t slashes = 0;
        while (i < a.size() && a[i] == L'\\') ++i, ++slashes;
        if (i == a.size()) {
            q.append(slashes * 2, L'\\');
            break;
        }
        if (a[i] == L'"') q.append(slashes * 2 + 1, L'\\');
        else q.append(slashes, L'\\');
        q += a[i];
    }
    return q + L"\"";
}
std::string lastErrorText() {
    const DWORD e = GetLastError();
    char* msg = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                   reinterpret_cast<char*>(&msg), 0, nullptr);
    std::string s = msg ? msg : std::format("error {}", e);
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}
} // namespace

bool TrainPlugin::running() const { return process_ != nullptr; }

// PluginHost.exe is a .NET Framework executable: Windows runs it directly.
Expected<void> TrainPlugin::spawn(const std::filesystem::path& hostExe, const std::filesystem::path& dll, const std::filesystem::path& trainFolder) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE toHostR = nullptr, toHostW = nullptr, fromHostR = nullptr, fromHostW = nullptr;
    if (!CreatePipe(&toHostR, &toHostW, &sa, 0)) return Unexpected("CreatePipe failed: " + lastErrorText());
    if (!CreatePipe(&fromHostR, &fromHostW, &sa, 0)) {
        CloseHandle(toHostR), CloseHandle(toHostW);
        return Unexpected("CreatePipe failed: " + lastErrorText());
    }
    // Only the child's ends are inherited.
    SetHandleInformation(toHostW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(fromHostR, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = toHostR;
    si.hStdOutput = fromHostW;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    std::wstring cmd = quoteArg(hostExe.wstring()) + L" " + quoteArg(dll.wstring()) + L" " + quoteArg(trainFolder.wstring());
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(hostExe.wstring().c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    const std::string why = ok ? std::string() : lastErrorText();
    CloseHandle(toHostR);
    CloseHandle(fromHostW);
    in_ = toHostW;
    out_ = fromHostR;
    if (!ok) return Unexpected("cannot start PluginHost.exe (needs the .NET Framework 4.x): " + why);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    return {};
}

bool TrainPlugin::writeAll(std::string_view bytes) {
    while (!bytes.empty()) {
        DWORD w = 0;
        if (!in_ || !WriteFile(in_, bytes.data(), DWORD(std::min<std::size_t>(bytes.size(), 1u << 20)), &w, nullptr) || w == 0) return false;
        bytes.remove_prefix(w);
    }
    return true;
}

TrainPlugin::ReadResult TrainPlugin::readSome(std::span<char> buffer, std::chrono::milliseconds timeout) {
    using enum ReadResult::Status;
    if (!out_) return {Closed};
    // Anonymous pipes have no overlapped I/O: poll. The host answers within a millisecond or
    // so, so yield first and only then sleep.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (int spins = 0;; ++spins) {
        DWORD avail = 0;
        if (!PeekNamedPipe(out_, nullptr, 0, nullptr, &avail, nullptr)) return {Closed}; // broken pipe: host exited
        if (avail) {
            DWORD r = 0;
            if (!ReadFile(out_, buffer.data(), DWORD(std::min<std::size_t>(buffer.size(), avail)), &r, nullptr) || r == 0) return {Closed};
            return {Data, r};
        }
        if (std::chrono::steady_clock::now() >= deadline) return {Timeout};
        if (spins < 2000) SwitchToThread();
        else Sleep(1);
    }
}

void TrainPlugin::closeHost() {
    if (in_) {
        DWORD w = 0;
        WriteFile(in_, "QUIT\n", 5, &w, nullptr);
        CloseHandle(in_);
        in_ = nullptr;
    }
    if (out_) {
        CloseHandle(out_);
        out_ = nullptr;
    }
    if (process_) {
        if (WaitForSingleObject(process_, 3000) != WAIT_OBJECT_0) { // let Unload save its settings
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 1000);
        }
        CloseHandle(process_);
        process_ = nullptr;
    }
}
#else
bool TrainPlugin::running() const { return pid_ > 0; }

// PluginHost.exe runs under Mono.
Expected<void> TrainPlugin::spawn(const std::filesystem::path& hostExe, const std::filesystem::path& dll, const std::filesystem::path& trainFolder) {
    std::array<int, 2> toHost{}, fromHost{}; // read end, write end
    if (pipe(toHost.data()) != 0 || pipe(fromHost.data()) != 0) return Unexpected("pipe() failed");
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, toHost[0], 0);
    posix_spawn_file_actions_adddup2(&fa, fromHost[1], 1);
    posix_spawn_file_actions_addclose(&fa, toHost[1]);
    posix_spawn_file_actions_addclose(&fa, fromHost[0]);
    std::array<std::string, 5> args{"mono", "--debug", hostExe.string(), dll.string(), trainFolder.string()};
    std::array<char*, args.size() + 1> argv{}; // posix_spawn wants mutable C strings, null-terminated list
    std::ranges::transform(args, argv.begin(), [](std::string& a) { return a.data(); });
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(toHost[0]);
    close(fromHost[1]);
    in_ = toHost[1];
    out_ = fromHost[0];
    if (rc != 0) return Unexpected(std::format("cannot start mono: {}", std::strerror(rc)));
    pid_ = int(pid);
    std::signal(SIGPIPE, SIG_IGN);
    return {};
}

bool TrainPlugin::writeAll(std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t w = in_ >= 0 ? write(in_, bytes.data(), bytes.size()) : -1;
        if (w <= 0) return false;
        bytes.remove_prefix(std::size_t(w));
    }
    return true;
}

TrainPlugin::ReadResult TrainPlugin::readSome(std::span<char> buffer, std::chrono::milliseconds timeout) {
    using enum ReadResult::Status;
    if (out_ < 0) return {Closed};
    pollfd pf{out_, POLLIN, 0};
    if (poll(&pf, 1, int(timeout.count())) <= 0) return {Timeout};
    const ssize_t r = read(out_, buffer.data(), buffer.size());
    return r <= 0 ? ReadResult{Closed} : ReadResult{Data, std::size_t(r)};
}

void TrainPlugin::closeHost() {
    if (in_ >= 0) {
        constexpr std::string_view quit = "QUIT\n";
        (void)!write(in_, quit.data(), quit.size());
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
#endif

// ---------------------------------------------------------------- protocol

std::filesystem::path findTrainPlugin(const std::filesystem::path& folder) {
    const std::filesystem::path cfg = resolvePath(folder, "ats.cfg");
    if (const std::optional<std::string> text = cfg.empty() ? std::nullopt : readTextFile(cfg)) {
        for (const std::string& raw : splitLines(*text)) {
            const std::string_view l = trim(raw);
            if (l.empty() || l.front() == ';') continue;
            const std::filesystem::path p = resolvePath(folder, l);
            if (!p.empty()) return p;
        }
    }
    return {};
}

TrainPlugin::~TrainPlugin() { stop(); }

Expected<void> TrainPlugin::start(const std::filesystem::path& hostExe, const std::filesystem::path& dll, const std::filesystem::path& trainFolder,
                                  const Setup& setup) {
    name = dll.filename().string();
    if (!std::filesystem::exists(hostExe)) return Unexpected("PluginHost.exe not built (needs a C# compiler and OpenBveApi.dll at configure time)");
    if (auto spawned = spawn(hostExe, dll, trainFolder); !spawned) {
        stop();
        return spawned;
    }
    for (const PluginStation& s : setup.stations)
        send(std::format("STATION {}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", s.name, s.arrival, s.departure, s.stop, s.forward, s.backward,
                         int(s.left), int(s.right), s.mode));
    send(std::format("LOAD {} {} {} {} {} {}", setup.powerNotches, setup.brakeType, setup.brakeNotches, int(setup.holdBrake), setup.cars,
                     setup.initMode));
    std::string reply; // the first line that is not an error report
    while (auto line = readLine()) {
        reply = std::move(*line);
        if (!reply.starts_with("ERR ")) break;
        lastError = reply.substr(4);
    }
    if (!reply.starts_with("OK")) {
        std::string error = reply.empty() ? "plugin host exited" : reply.substr(reply.starts_with("FAIL ") ? 5 : 0);
        if (!lastError.empty()) error += " (" + lastError + ")";
        stop();
        return Unexpected(std::move(error));
    }
    panel.assign(std::size_t(std::max(0, parseIntVb6(std::string_view(reply).substr(2)).value_or(0))), 0); // "OK <panel size>"
    return {};
}

bool TrainPlugin::send(std::string_view line) {
    if (!running()) return false;
    if (!writeAll(std::string(line) + '\n')) {
        stop();
        return false;
    }
    return true;
}

std::optional<std::string> TrainPlugin::readLine() {
    using enum ReadResult::Status;
    for (;;) {
        if (const auto nl = buffer_.find('\n'); nl != std::string::npos) {
            std::string line = buffer_.substr(0, nl);
            buffer_.erase(0, nl + 1);
            return line;
        }
        if (!running()) return std::nullopt;
        const ReadResult r = readSome(chunk_, std::chrono::seconds(5));
        if (r.status == Timeout) lastError = "plugin host timed out"; // a plugin that hangs for 5 s is dropped
        if (r.status != Data) {
            stop();
            return std::nullopt;
        }
        buffer_.append(chunk_.data(), r.bytes);
    }
}

std::optional<PluginHandles> TrainPlugin::elapse(const PluginVehicle& v) {
    sounds.clear();
    if (!running()) return std::nullopt;
    const PluginHandles& d = v.driver;
    if (!send(std::format("ELAPSE {:.4f} {:.4f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.5f} {:.5f} {:.0f} {:.0f} {} {} {} {} {} {} {} {}",
                          v.location, v.speedKmh, v.bc, v.mr, v.er, v.bp, v.sap, v.radius, v.cant, v.pitch, v.totalMs, v.elapsedMs,
                          d.reverser, d.power, d.brake, int(d.constSpeed), int(d.holdBrake), d.doorInterlock, d.headlights, v.camera)))
        return std::nullopt;
    auto integer = [](std::string_view s, int fallback = 0) { return parseIntVb6(s).value_or(fallback); };
    PluginHandles out;
    debug.clear();
    while (const auto received = readLine()) {
        const std::string& line = *received;
        if (line == "END") return out;
        if (line.starts_with("H ")) {
            const auto f = split(std::string_view(line).substr(2), ' ');
            std::array<int, 7> x{};
            for (std::size_t i = 0; i < x.size() && i < f.size(); ++i) x[i] = integer(f[i]);
            out = {x[0], x[1], x[2], x[3] != 0, x[4] != 0, x[5], x[6]};
        } else if (line.starts_with("P ")) {
            const auto f = split(std::string_view(line).substr(2), ' ');
            const int n = f.empty() ? 0 : integer(f[0]);
            panel.resize(std::size_t(std::max(0, n)));
            for (std::size_t i = 0; i < panel.size() && i + 1 < f.size(); ++i) panel[i] = integer(f[i + 1], panel[i]);
        } else if (line.starts_with("MSG ")) {
            auto f = split(std::string_view(line).substr(4), ' ');
            PluginMessage m;
            if (f.size() >= 2) {
                m.color = integer(f[0], m.color);
                m.seconds = parseDoubleVb6(f[1]).value_or(m.seconds);
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
            if (f.size() != 6 || (f[5] != "0" && f[5] != "1")) continue;
            const auto handle = parseIntVb6(f[0]), car = parseIntVb6(f[1]), index = parseIntVb6(f[2]);
            const auto volume = parseDouble(f[3]), pitch = parseDouble(f[4]);
            if (handle && car && index && volume && pitch)
                sounds.push_back({.kind = PluginSoundCommand::Kind::Play, .handle = *handle, .index = *index, .car = *car >= 0 ? car : std::nullopt, .volume = *volume, .pitch = *pitch, .looped = f[5] == "1"});
        } else if (line.starts_with("SNDSET ")) {
            const auto f = split(std::string_view(line).substr(7), ' ');
            if (f.size() != 3) continue;
            PluginSoundCommand sound{PluginSoundCommand::Kind::Set};
            const auto handle = parseIntVb6(f[0]);
            const auto volume = parseDouble(f[1]), pitch = parseDouble(f[2]);
            if (!handle || !volume || !pitch) continue;
            sound.handle = *handle, sound.volume = *volume, sound.pitch = *pitch;
            sounds.push_back(sound);
        } else if (line.starts_with("SNDSTOP ")) {
            PluginSoundCommand sound{PluginSoundCommand::Kind::Stop};
            if (const auto handle = parseIntVb6(std::string_view(line).substr(8))) {
                sound.handle = *handle;
                sounds.push_back(sound);
            }
        } else if (line.starts_with("DEBUG ")) {
            debug = line.substr(6);
        } else if (line.starts_with("ERR ")) {
            lastError = line.substr(4);
            phx::log::warn("plugin {}: {}", name, lastError);
        }
    }
    return std::nullopt;
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

void TrainPlugin::stop() { closeHost(); }

} // namespace ots
