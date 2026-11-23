#include "bve/Controls.hpp"

#include "phx/core/Log.hpp"
#include "util/Paths.hpp"
#include "util/Text.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <unordered_map>

namespace ots {
namespace {

struct CommandInfo {
    Command cmd;
    const char* name;
};
constexpr CommandInfo kCommands[] = {
    {Command::PowerIncrease, "POWER_INCREASE"}, {Command::PowerDecrease, "POWER_DECREASE"}, {Command::PowerFull, "POWER_FULL"},
    {Command::PowerNeutral, "POWER_NEUTRAL"}, {Command::BrakeIncrease, "BRAKE_INCREASE"}, {Command::BrakeDecrease, "BRAKE_DECREASE"},
    {Command::BrakeFull, "BRAKE_FULL"}, {Command::BrakeEmergency, "BRAKE_EMERGENCY"}, {Command::BrakeNeutral, "BRAKE_NEUTRAL"},
    {Command::SinglePower, "SINGLE_POWER"}, {Command::SingleNeutral, "SINGLE_NEUTRAL"}, {Command::SingleBrake, "SINGLE_BRAKE"},
    {Command::SingleEmergency, "SINGLE_EMERGENCY"}, {Command::ReverserForward, "REVERSER_FORWARD"},
    {Command::ReverserBackward, "REVERSER_BACKWARD"}, {Command::HoldBrake, "HOLD_BRAKE"}, {Command::ConstSpeed, "DEVICE_CONSTSPEED"},
    {Command::DoorsLeft, "DOORS_LEFT"}, {Command::DoorsRight, "DOORS_RIGHT"}, {Command::HornPrimary, "HORN_PRIMARY"},
    {Command::HornSecondary, "HORN_SECONDARY"}, {Command::HornMusic, "HORN_MUSIC"}, {Command::Headlights, "HEADLIGHTS"},
    {Command::RaisePantograph, "RAISE_PANTOGRAPH"}, {Command::LowerPantograph, "LOWER_PANTOGRAPH"},
    {Command::MainBreaker, "MAIN_BREAKER"}, {Command::SecurityS, "SECURITY_S"}, {Command::SecurityA1, "SECURITY_A1"},
    {Command::SecurityA2, "SECURITY_A2"}, {Command::SecurityB1, "SECURITY_B1"}, {Command::SecurityB2, "SECURITY_B2"},
    {Command::SecurityC1, "SECURITY_C1"}, {Command::SecurityC2, "SECURITY_C2"}, {Command::SecurityD, "SECURITY_D"},
    {Command::SecurityE, "SECURITY_E"}, {Command::SecurityF, "SECURITY_F"}, {Command::SecurityG, "SECURITY_G"}, {Command::SecurityH, "SECURITY_H"},
    {Command::SecurityI, "SECURITY_I"}, {Command::SecurityJ, "SECURITY_J"}, {Command::SecurityK, "SECURITY_K"}, {Command::SecurityL, "SECURITY_L"}, {Command::CameraInterior, "CAMERA_INTERIOR"},
    {Command::CameraInteriorNoPanel, "CAMERA_INTERIOR_NOPANEL"}, {Command::CameraExterior, "CAMERA_EXTERIOR"},
    {Command::CameraTrack, "CAMERA_TRACK"}, {Command::CameraFlyby, "CAMERA_FLYBY"}, {Command::CameraHeadoutLeft, "CAMERA_HEADOUT_LEFT"},
    {Command::CameraHeadoutRight, "CAMERA_HEADOUT_RIGHT"}, {Command::CameraMoveForward, "CAMERA_MOVE_FORWARD"},
    {Command::CameraMoveBackward, "CAMERA_MOVE_BACKWARD"}, {Command::CameraMoveLeft, "CAMERA_MOVE_LEFT"},
    {Command::CameraMoveRight, "CAMERA_MOVE_RIGHT"}, {Command::CameraMoveUp, "CAMERA_MOVE_UP"}, {Command::CameraMoveDown, "CAMERA_MOVE_DOWN"},
    {Command::CameraRotateLeft, "CAMERA_ROTATE_LEFT"}, {Command::CameraRotateRight, "CAMERA_ROTATE_RIGHT"},
    {Command::CameraRotateUp, "CAMERA_ROTATE_UP"}, {Command::CameraRotateDown, "CAMERA_ROTATE_DOWN"},
    {Command::CameraRotateCcw, "CAMERA_ROTATE_CCW"}, {Command::CameraRotateCw, "CAMERA_ROTATE_CW"}, {Command::CameraZoomIn, "CAMERA_ZOOM_IN"},
    {Command::CameraZoomOut, "CAMERA_ZOOM_OUT"}, {Command::CameraPoiPrevious, "CAMERA_POI_PREVIOUS"}, {Command::CameraPoiNext, "CAMERA_POI_NEXT"},
    {Command::CameraReset, "CAMERA_RESET"}, {Command::CameraRestriction, "CAMERA_RESTRICTION"}, {Command::TimetableToggle, "TIMETABLE_TOGGLE"},
    {Command::MenuActivate, "MENU_ACTIVATE"}, {Command::MiscClock, "MISC_CLOCK"}, {Command::MiscSpeed, "MISC_SPEED"},
    {Command::MiscGradient, "MISC_GRADIENT"}, {Command::MiscDistNextStation, "MISC_DIST_NEXT_STATION"},
    {Command::MiscTimeFactor, "MISC_TIMEFACTOR"}, {Command::MiscFps, "MISC_FPS"}, {Command::MiscAi, "MISC_AI"}, {Command::MiscPause, "MISC_PAUSE"},
    {Command::MiscQuit, "MISC_QUIT"}, {Command::MiscFullscreen, "MISC_FULLSCREEN"}, {Command::MiscMute, "MISC_MUTE"},
    {Command::MiscInterface, "MISC_INTERFACE"}, {Command::MiscBackface, "MISC_BACKFACE"}, {Command::MiscCpuMode, "MISC_CPUMODE"},
    {Command::RouteInformation, "ROUTE_INFORMATION"},
};

const std::unordered_map<std::string, int>& keyTable() {
    static const std::unordered_map<std::string, int> t = [] {
        std::unordered_map<std::string, int> m;
        for (char c = 'a'; c <= 'z'; ++c) m[std::string(1, c)] = GLFW_KEY_A + (c - 'a');
        for (int i = 0; i <= 9; ++i) {
            m["number" + std::to_string(i)] = GLFW_KEY_0 + i;
            m["d" + std::to_string(i)] = GLFW_KEY_0 + i;
            m[std::to_string(i)] = GLFW_KEY_0 + i;
            m["keypad" + std::to_string(i)] = GLFW_KEY_KP_0 + i;
            m["kp" + std::to_string(i)] = GLFW_KEY_KP_0 + i;
            m["kp_" + std::to_string(i)] = GLFW_KEY_KP_0 + i;
        }
        for (int i = 1; i <= 25; ++i) m["f" + std::to_string(i)] = GLFW_KEY_F1 + i - 1;
        const std::pair<const char*, int> named[] = {
            {"keypaddivide", GLFW_KEY_KP_DIVIDE}, {"kp_divide", GLFW_KEY_KP_DIVIDE}, {"keypadmultiply", GLFW_KEY_KP_MULTIPLY},
            {"kp_multiply", GLFW_KEY_KP_MULTIPLY}, {"keypadminus", GLFW_KEY_KP_SUBTRACT}, {"keypadsubtract", GLFW_KEY_KP_SUBTRACT},
            {"kp_minus", GLFW_KEY_KP_SUBTRACT}, {"keypadplus", GLFW_KEY_KP_ADD}, {"keypadadd", GLFW_KEY_KP_ADD}, {"kp_plus", GLFW_KEY_KP_ADD},
            {"keypadperiod", GLFW_KEY_KP_DECIMAL}, {"keypaddecimal", GLFW_KEY_KP_DECIMAL}, {"kp_period", GLFW_KEY_KP_DECIMAL},
            {"keypadenter", GLFW_KEY_KP_ENTER}, {"kp_enter", GLFW_KEY_KP_ENTER}, {"keypadequal", GLFW_KEY_KP_EQUAL},
            {"escape", GLFW_KEY_ESCAPE}, {"enter", GLFW_KEY_ENTER}, {"return", GLFW_KEY_ENTER}, {"space", GLFW_KEY_SPACE},
            {"backspace", GLFW_KEY_BACKSPACE}, {"back", GLFW_KEY_BACKSPACE}, {"tab", GLFW_KEY_TAB}, {"insert", GLFW_KEY_INSERT},
            {"delete", GLFW_KEY_DELETE}, {"home", GLFW_KEY_HOME}, {"end", GLFW_KEY_END}, {"pageup", GLFW_KEY_PAGE_UP},
            {"pagedown", GLFW_KEY_PAGE_DOWN}, {"up", GLFW_KEY_UP}, {"down", GLFW_KEY_DOWN}, {"left", GLFW_KEY_LEFT},
            {"right", GLFW_KEY_RIGHT}, {"comma", GLFW_KEY_COMMA}, {"period", GLFW_KEY_PERIOD}, {"slash", GLFW_KEY_SLASH},
            {"semicolon", GLFW_KEY_SEMICOLON}, {"quote", GLFW_KEY_APOSTROPHE}, {"bracketleft", GLFW_KEY_LEFT_BRACKET},
            {"lbracket", GLFW_KEY_LEFT_BRACKET}, {"leftbracket", GLFW_KEY_LEFT_BRACKET}, {"bracketright", GLFW_KEY_RIGHT_BRACKET},
            {"rbracket", GLFW_KEY_RIGHT_BRACKET}, {"rightbracket", GLFW_KEY_RIGHT_BRACKET}, {"backslash", GLFW_KEY_BACKSLASH},
            {"nonusbackslash", GLFW_KEY_WORLD_1}, {"minus", GLFW_KEY_MINUS}, {"plus", GLFW_KEY_EQUAL}, {"equals", GLFW_KEY_EQUAL},
            {"tilde", GLFW_KEY_GRAVE_ACCENT}, {"grave", GLFW_KEY_GRAVE_ACCENT}, {"backquote", GLFW_KEY_GRAVE_ACCENT},
            {"capslock", GLFW_KEY_CAPS_LOCK}, {"scrolllock", GLFW_KEY_SCROLL_LOCK}, {"numlock", GLFW_KEY_NUM_LOCK},
            {"printscreen", GLFW_KEY_PRINT_SCREEN}, {"print", GLFW_KEY_PRINT_SCREEN}, {"pause", GLFW_KEY_PAUSE}, {"menu", GLFW_KEY_MENU},
        };
        for (auto [n, k] : named) m[n] = k;
        return m;
    }();
    return t;
}

} // namespace

const char* commandName(Command c) {
    for (const CommandInfo& i : kCommands)
        if (i.cmd == c) return i.name;
    return "?";
}

int glfwKeyFromName(std::string_view name) {
    const auto& t = keyTable();
    auto it = t.find(lower(trim(name)));
    return it == t.end() ? -1 : it->second;
}

std::string keyName(int key) {
    std::string best;
    for (const auto& [n, k] : keyTable())
        if (k == key && (best.empty() || n.size() > best.size())) best = n;
    if (!best.empty()) best[0] = char(std::toupper(static_cast<unsigned char>(best[0])));
    return best;
}

void Controls::loadDefaults() {
    static const char* kDefault = R"(POWER_INCREASE, keyboard, Z, 0
POWER_DECREASE, keyboard, A, 0
BRAKE_DECREASE, keyboard, Comma, 0
BRAKE_INCREASE, keyboard, Period, 0
BRAKE_EMERGENCY, keyboard, Slash, 0
SINGLE_POWER, keyboard, Z, 0
SINGLE_NEUTRAL, keyboard, A, 0
SINGLE_BRAKE, keyboard, Q, 0
SINGLE_EMERGENCY, keyboard, Number1, 0
REVERSER_FORWARD, keyboard, F, 0
REVERSER_BACKWARD, keyboard, V, 0
DOORS_LEFT, keyboard, F5, 0
DOORS_RIGHT, keyboard, F6, 0
HORN_PRIMARY, keyboard, Enter, 0
HORN_PRIMARY, keyboard, KeypadEnter, 0
HORN_SECONDARY, keyboard, Enter, 1
HORN_SECONDARY, keyboard, KeypadPlus, 0
HORN_MUSIC, keyboard, Enter, 2
HORN_MUSIC, keyboard, KeypadMinus, 0
HEADLIGHTS, keyboard, H, 0
RAISE_PANTOGRAPH, keyboard, P, 0
LOWER_PANTOGRAPH, keyboard, P, 2
MAIN_BREAKER, keyboard, B, 0
DEVICE_CONSTSPEED, keyboard, BackSpace, 0
SECURITY_S, keyboard, Space, 0
SECURITY_A1, keyboard, Insert, 0
SECURITY_A2, keyboard, Delete, 0
SECURITY_B1, keyboard, Home, 0
SECURITY_B2, keyboard, End, 0
SECURITY_C1, keyboard, PageUp, 0
SECURITY_C2, keyboard, PageDown, 0
SECURITY_D, keyboard, Number2, 0
SECURITY_E, keyboard, Number3, 0
SECURITY_F, keyboard, Number4, 0
SECURITY_G, keyboard, Number5, 0
SECURITY_H, keyboard, Number6, 0
SECURITY_I, keyboard, Number7, 0
SECURITY_J, keyboard, Number8, 0
SECURITY_K, keyboard, Number9, 0
SECURITY_L, keyboard, Number0, 0
CAMERA_INTERIOR, keyboard, F1, 0
CAMERA_HEADOUT_LEFT, keyboard, Left, 2
CAMERA_HEADOUT_RIGHT, keyboard, Right, 2
CAMERA_EXTERIOR, keyboard, F2, 0
CAMERA_TRACK, keyboard, F3, 0
CAMERA_FLYBY, keyboard, F4, 0
CAMERA_MOVE_FORWARD, keyboard, Keypad9, 0
CAMERA_MOVE_BACKWARD, keyboard, Keypad3, 0
CAMERA_MOVE_LEFT, keyboard, Keypad4, 0
CAMERA_MOVE_RIGHT, keyboard, Keypad6, 0
CAMERA_MOVE_UP, keyboard, Keypad8, 0
CAMERA_MOVE_DOWN, keyboard, Keypad2, 0
CAMERA_ROTATE_LEFT, keyboard, Left, 0
CAMERA_ROTATE_RIGHT, keyboard, Right, 0
CAMERA_ROTATE_UP, keyboard, Up, 0
CAMERA_ROTATE_DOWN, keyboard, Down, 0
CAMERA_ROTATE_CCW, keyboard, KeypadDivide, 0
CAMERA_ROTATE_CW, keyboard, KeypadMultiply, 0
CAMERA_ZOOM_IN, keyboard, KeypadPeriod, 0
CAMERA_ZOOM_OUT, keyboard, Keypad0, 0
CAMERA_POI_PREVIOUS, keyboard, Keypad1, 0
CAMERA_POI_NEXT, keyboard, Keypad7, 0
CAMERA_RESET, keyboard, Keypad5, 0
CAMERA_RESTRICTION, keyboard, R, 2
TIMETABLE_TOGGLE, keyboard, T, 2
MENU_ACTIVATE, keyboard, Escape, 0
MISC_CLOCK, keyboard, C, 2
MISC_SPEED, keyboard, V, 2
MISC_GRADIENT, keyboard, N, 2
MISC_DIST_NEXT_STATION, keyboard, D, 2
MISC_TIMEFACTOR, keyboard, J, 2
MISC_FPS, keyboard, X, 2
MISC_PAUSE, keyboard, Pause, 0
MISC_QUIT, keyboard, Q, 2
MISC_FULLSCREEN, keyboard, F, 2
MISC_MUTE, keyboard, M, 2
MISC_INTERFACE, keyboard, F10, 0
MISC_BACKFACE, keyboard, F11, 0
ROUTE_INFORMATION, keyboard, F8, 0
)";
    bindings_.clear();
    for (const std::string& line : splitLines(kDefault)) {
        auto f = split(line, ',');
        if (f.size() < 4) continue;
        for (const CommandInfo& i : kCommands)
            if (iequals(trim(f[0]), i.name)) {
                int mods = 0;
                parseIntVb6(f[3], mods);
                bindings_.push_back({i.cmd, glfwKeyFromName(f[2]), mods});
            }
    }
}

std::vector<KeyBinding> Controls::parse(const std::string& text, int* unknownKeys) {
    std::vector<KeyBinding> b;
    for (const std::string& raw : splitLines(text)) {
        std::string_view line = trim(raw);
        if (line.empty() || line.front() == ';') continue;
        auto f = split(line, ',');
        if (f.size() < 3 || !iequals(trim(f[1]), "keyboard")) continue;
        const CommandInfo* info = nullptr;
        for (const CommandInfo& i : kCommands)
            if (iequals(trim(f[0]), i.name)) info = &i;
        if (!info) continue;
        const int key = glfwKeyFromName(f[2]);
        if (key < 0) {
            if (unknownKeys) ++*unknownKeys;
            continue;
        }
        int mods = 0;
        if (f.size() >= 4) parseIntVb6(f[3], mods);
        b.push_back({info->cmd, key, mods});
    }
    return b;
}

bool Controls::loadFile(const std::filesystem::path& file) {
    std::string text;
    if (!readTextFile(file, text)) return false;
    int unknownKeys = 0;
    std::vector<KeyBinding> b = parse(text, &unknownKeys);
    if (b.empty()) return false;
    bindings_ = std::move(b);
    if (unknownKeys) phx::log::warn("{}: {} bindings with unknown key names ignored", file.string(), unknownKeys);
    return true;
}

int Controls::mergeFile(const std::filesystem::path& file) {
    std::string text;
    if (!readTextFile(file, text)) return 0;
    const std::vector<KeyBinding> extra = parse(text, nullptr);
    for (const KeyBinding& e : extra)
        std::erase_if(bindings_, [&](const KeyBinding& b) { return b.glfwKey == e.glfwKey && b.modifiers == e.modifiers; });
    bindings_.insert(bindings_.end(), extra.begin(), extra.end());
    return int(extra.size());
}

std::vector<std::filesystem::path> Controls::findContentControls(const std::filesystem::path& train, const std::filesystem::path& route) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    auto add = [&](const std::filesystem::path& p) {
        if (!p.empty() && std::filesystem::is_regular_file(p, ec) && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    };
    if (!train.empty()) {
        add(resolvePath(train, "controls.cfg"));
        for (const auto& e : std::filesystem::directory_iterator(train, ec))
            if (iequals(e.path().extension().string(), ".controls")) add(e.path());
    }
    if (!route.empty()) {
        add(resolvePath(route.parent_path(), route.stem().string() + ".controls"));
        add(resolvePath(route.parent_path(), "controls.cfg"));
    }
    return out;
}

bool Controls::saveFile(const std::filesystem::path& file) const {
    std::ofstream f(file);
    if (!f) return false;
    f << "; openTrainSim control configuration (openBVE format)\n";
    for (const KeyBinding& k : bindings_) f << commandName(k.command) << ", Keyboard, " << keyName(k.glfwKey) << ", " << k.modifiers << ", 0\n";
    return true;
}

void Controls::commandsFor(int key, int glfwMods, std::vector<Command>& out) const {
    int mods = 0;
    if (glfwMods & GLFW_MOD_SHIFT) mods |= 1;
    if (glfwMods & GLFW_MOD_CONTROL) mods |= 2;
    if (glfwMods & GLFW_MOD_ALT) mods |= 4;
    for (const KeyBinding& b : bindings_)
        if (b.glfwKey == key && b.modifiers == mods) out.push_back(b.command);
}

std::filesystem::path Controls::findOpenBveControls() {
    const char* home = std::getenv("HOME");
    if (!home) return {};
    std::filesystem::path best;
    std::filesystem::file_time_type bestTime{};
    std::error_code ec;
    for (const char* base : {".config/OpenBve/Settings", ".config/openBVE/Settings"}) {
        const std::filesystem::path dir = std::filesystem::path(home) / base;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            const std::filesystem::path c = e.path() / "controls.cfg";
            if (!std::filesystem::exists(c, ec)) continue;
            const auto t = std::filesystem::last_write_time(c, ec);
            if (best.empty() || t > bestTime) best = c, bestTime = t;
        }
    }
    return best;
}

} // namespace ots
