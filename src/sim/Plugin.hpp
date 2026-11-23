#pragma once
// Train safety-system plugins (ATS / ATC / ATP / TVM ...) written against openBVE's managed
// API (OpenBveApi.Runtime.IRuntime). They run unmodified in a separate Mono process
// (tools/PluginHost) — the same isolation openBVE uses for Win32 plugins — and talk to the
// simulator over pipes, one request / reply per frame.
#include <filesystem>
#include <string>
#include <vector>

#include <sys/types.h>

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
    int car = -1;        // -1 = driver car panel position (PlaySound), else car index (PlayCarSound)
    double volume = 1.0, pitch = 1.0;
    bool looped = false; // Play
};

class TrainPlugin {
public:
    ~TrainPlugin();
    // Starts the host and loads `dll`. Returns false with `error` set on failure.
    bool start(const std::filesystem::path& hostExe, const std::filesystem::path& dll, const std::filesystem::path& trainFolder,
               int powerNotches, int brakeType, int brakeNotches, bool holdBrake, int cars, int initMode,
               const std::vector<PluginStation>& stations, std::string& error);
    [[nodiscard]] bool running() const { return pid_ > 0; }
    // One frame: driver handles + vehicle state in, safety handles + panel out.
    bool elapse(const PluginVehicle& v, PluginHandles& out);
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
    bool send(const std::string& line);
    bool readLine(std::string& line);
    pid_t pid_ = -1;
    int in_ = -1, out_ = -1; // our write end (host stdin), our read end (host stdout)
    std::string buffer_;
};

// Locates the plugin named in the train's ats.cfg (or the first .dll in the folder).
std::filesystem::path findTrainPlugin(const std::filesystem::path& trainFolder);
// PluginHost.exe next to the running executable.
std::filesystem::path pluginHostPath();

} // namespace ots
