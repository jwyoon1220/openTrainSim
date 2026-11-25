#pragma once
// openBVE-compatible key bindings. Reads openBVE control files ("COMMAND, Keyboard, KEY,
// MODIFIERS[, OPTION]") in both the OpenTK key naming (1.4.4+, "Period") and the legacy SDL
// naming ("PERIOD"), so a player's own ~/.config/OpenBve/Settings/*/controls.cfg works as is.
#include <optional>
#include <filesystem>
#include <string>
#include <vector>

namespace ots {

enum class Command : unsigned short {
    PowerIncrease, PowerDecrease, PowerFull, PowerNeutral,
    BrakeIncrease, BrakeDecrease, BrakeFull, BrakeEmergency, BrakeNeutral,
    SinglePower, SingleNeutral, SingleBrake, SingleEmergency,
    ReverserForward, ReverserBackward,
    HoldBrake, ConstSpeed,
    DoorsLeft, DoorsRight,
    HornPrimary, HornSecondary, HornMusic,
    Headlights, RaisePantograph, LowerPantograph, MainBreaker,
    // safety-system keys, in openBVE VirtualKeys order (S = 0 … L = 15)
    SecurityS, SecurityA1, SecurityA2, SecurityB1, SecurityB2, SecurityC1, SecurityC2,
    SecurityD, SecurityE, SecurityF, SecurityG, SecurityH, SecurityI, SecurityJ, SecurityK, SecurityL,
    CameraInterior, CameraInteriorNoPanel, CameraExterior, CameraTrack, CameraFlyby, CameraHeadoutLeft, CameraHeadoutRight,
    CameraMoveForward, CameraMoveBackward, CameraMoveLeft, CameraMoveRight, CameraMoveUp, CameraMoveDown,
    CameraRotateLeft, CameraRotateRight, CameraRotateUp, CameraRotateDown, CameraRotateCcw, CameraRotateCw,
    CameraZoomIn, CameraZoomOut, CameraPoiPrevious, CameraPoiNext, CameraReset, CameraRestriction,
    TimetableToggle, MenuActivate,
    MiscClock, MiscSpeed, MiscGradient, MiscDistNextStation, MiscTimeFactor, MiscFps, MiscAi, MiscPause, MiscQuit,
    MiscFullscreen, MiscMute, MiscInterface, MiscBackface, MiscCpuMode, RouteInformation,
    Count
};

std::string_view commandName(Command c);
// openBVE command name ("DOORS_LEFT", case-insensitive) → command; false if unknown.
std::optional<Command> commandFromName(std::string_view name); // case-insensitive
// openBVE VirtualKeys index of a SECURITY_* command, or -1.
inline int securityKey(Command c) {
    const int i = int(c) - int(Command::SecurityS);
    return i >= 0 && i <= 15 ? i : -1;
}

struct KeyBinding {
    Command command;
    int glfwKey = -1;
    int modifiers = 0; // openBVE: 1 Shift, 2 Ctrl, 4 Alt
};

class Controls {
public:
    // Built-in defaults: identical to openBVE's Default.controls.
    void loadDefaults();
    // Returns false if the file could not be read (bindings unchanged).
    bool loadFile(const std::filesystem::path& file);
    // Adds a route / train specific control file on top of the current bindings: a binding
    // whose key + modifiers is already used replaces those, others are added. Returns the
    // number of bindings read (0 = not readable).
    int mergeFile(const std::filesystem::path& file);
    // Extra files shipped with a train or route: <train>/controls.cfg, <train>/*.controls,
    // <route dir>/<route name>.controls, <route dir>/controls.cfg.
    static std::vector<std::filesystem::path> findContentControls(const std::filesystem::path& trainFolder,
                                                                 const std::filesystem::path& routeFile);
    bool saveFile(const std::filesystem::path& file) const;
    // Commands triggered by a key press (with GLFW modifier bits).
    void commandsFor(int glfwKey, int glfwMods, std::vector<Command>& out) const;
    // Is a held key currently bound to `c` (for continuous camera commands)?
    [[nodiscard]] const std::vector<KeyBinding>& bindings() const { return bindings_; }
    [[nodiscard]] static std::filesystem::path findOpenBveControls();

private:
    static std::vector<KeyBinding> parse(const std::string& text, int* unknownKeys);
    std::vector<KeyBinding> bindings_;
};

int glfwKeyFromName(std::string_view name);
std::string keyName(int glfwKey);

} // namespace ots
