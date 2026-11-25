#pragma once
#include "util/Expected.hpp"
// Train safety-system plugins (ATS / ATC / ATP / TVM ...) written against openBVE's managed
// API (OpenBveApi.Runtime.IRuntime). They run unmodified in a separate .NET process
// (tools/PluginHost: Mono on Linux, the .NET Framework on Windows) — the same isolation openBVE
// uses for Win32 plugins — and talk to the simulator over pipes, one request / reply per frame.
#include <filesystem>
#include <string>
#include <string_view>
#include <span>
#include <optional>
#include <chrono>
#include <cstdint>
#include <vector>

namespace ots {

struct PluginHandles {
    int reverser = 0, power = 0, brake = 0;
    bool constSpeed = false, holdBrake = false;
    int doorInterlock = 0, headlights = 0;
};

struct PluginVehicle {
    double location = 0, speedKmh = 0;
    double bc = 0, mr = 0, er = 0, bp = 0, sap = 0; // kPa
    double radius = 0, cant = 0, pitch = 0;
    double totalMs = 0, elapsedMs = 0;
    PluginHandles driver;
    int camera = 2; // OpenBveApi CameraViewMode (2 interior, 8 exterior)
};

struct PluginStation {
    std::string name;
    double arrival = -1, departure = -1, stop = 0, forward = 5, backward = 5;
    bool left = false, right = false;
    int mode = 0;
};

struct PluginMessage {
    std::string text;
    int color = 0;
    double seconds = 3.0;
    double until = 0.0; // simulation clock when it disappears
};

// Sound requests from the plugin (OpenBveApi PlaySound / PlayCarSound / PlayMultipleCarSound and
// the SoundHandle it returned). Consumed by the audio side once per frame.
struct PluginSoundCommand {
    enum class Kind { Play, Set, Stop } kind = Kind::Play;
    int handle = 0;      // host-assigned id, unique for the plugin's lifetime
    int index = 0;       // [Ats] sound index in sound.cfg (Play)
    std::optional<int> car; // none: the panel position in the driver car (PlaySound), else a car (PlayCarSound)
    double volume = 1.0, pitch = 1.0;
    bool looped = false; // Play
};

class TrainPlugin {
public:
    ~TrainPlugin();
    // What the plugin is told about the train and the route when it loads.
    struct Setup {
        int powerNotches = 0, brakeType = 0, brakeNotches = 0;
        bool holdBrake = false;
        int cars = 1;
        int initMode = -1; // openBVE InitializationModes
        std::vector<PluginStation> stations;
    };
    // Starts the host and loads `dll`.
    Expected<void> start(const std::filesystem::path& hostExe, const std::filesystem::path& dll, const std::filesystem::path& trainFolder,
                         const Setup& setup);
    [[nodiscard]] bool running() const;
    // One frame: driver handles + vehicle state in, safety handles out (panel, messages,
    // sounds ... updated); nullopt when the host is gone.
    std::optional<PluginHandles> elapse(const PluginVehicle& v);
    void setReverser(int r), setPower(int p), setBrake(int b);
    void keyDown(int virtualKey), keyUp(int virtualKey);
    void horn(int type);
    void doorChange(int oldState, int newState);
    void signal(const std::vector<std::pair<int, double>>& aspects);
    void beacon(int type, int optional, int aspect, double distance);
    void initialize(int mode);
    void stop();

    std::vector<int> panel;
    std::vector<PluginMessage> messages; // since the last elapse (consumed by the caller)
    std::string debug, lastError, name;
    int doorRequest = 0;      // bit 1 open-left, 2 open-right, 4 close-left, 8 close-right (consumed)
    std::vector<PluginSoundCommand> sounds; // since the last elapse, in order (consumed by the caller)
    // A non-looped sound finished (or the audio side dropped it): the plugin's handle reports
    // Playing = false from the next Elapse on.
    void soundEnded(int handle);

private:
    bool send(std::string_view line);
    std::optional<std::string> readLine(); // nullopt: host gone or hung
    // Platform pipe I/O (sim/Plugin.cpp).
    Expected<void> spawn(const std::filesystem::path& hostExe, const std::filesystem::path& dll, const std::filesystem::path& trainFolder);
    bool writeAll(std::string_view bytes);
    struct ReadResult {
        enum class Status : std::uint8_t { Data, Timeout, Closed };
        Status status;
        std::size_t bytes = 0;
    };
    ReadResult readSome(std::span<char> buffer, std::chrono::milliseconds timeout);
    void closeHost();
#ifdef _WIN32
    void* process_ = nullptr;           // HANDLE
    void* in_ = nullptr, *out_ = nullptr; // our write end (host stdin), our read end (host stdout)
#else
    int pid_ = -1;
    int in_ = -1, out_ = -1; // our write end (host stdin), our read end (host stdout)
#endif
    std::string buffer_;                 // received, not yet split into lines
    std::vector<char> chunk_ = std::vector<char>(65536); // read scratch
};

// Locates the plugin named in the train's ats.cfg (or the first .dll in the folder).
std::filesystem::path findTrainPlugin(const std::filesystem::path& trainFolder);
// PluginHost.exe next to the running executable.
std::filesystem::path pluginHostPath();

} // namespace ots
