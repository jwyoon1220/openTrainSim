#pragma once
#include "phx/core/Types.hpp"

#include <string>
#include <vector>

struct GLFWwindow;

namespace phx {

struct Extent2D {
    u32 width = 0;
    u32 height = 0;
    [[nodiscard]] bool empty() const { return width == 0 || height == 0; }
    bool operator==(const Extent2D&) const = default;
};

// One input event in arrival order (UI needs ordering: press, move, release...).
struct InputEvent {
    enum class Type : u8 { Key, Button, Move, Scroll, Char } type = Type::Key;
    int code = 0;        // GLFW key / mouse button, or Unicode code point
    int mods = 0;        // GLFW modifier bits
    bool down = false;   // Key (press or repeat) / Button
    f64 x = 0, y = 0;    // cursor in framebuffer pixels (Move, Button), or scroll (Scroll: y)
};

// Input gathered between two drainInput() calls.
struct InputFrame {
    std::vector<InputEvent> events;
    std::vector<int> keysPressed; // GLFW key codes (press + repeat)
    f64 mouseDx = 0, mouseDy = 0; // while a mouse button is held
    f64 scroll = 0;
};

// GLFW window. Every method must be called from the main (supervisor) thread.
class Window {
public:
    Window(u32 width, u32 height, const char* title);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] GLFWwindow* handle() const { return window_; }
    [[nodiscard]] bool shouldClose() const;
    void requestClose();
    void pollEvents();
    void waitEvents(double timeoutSeconds);
    [[nodiscard]] Extent2D framebufferSize() const;
    void setTitle(const std::string& title);
    InputFrame drainInput();
    [[nodiscard]] bool keyDown(int glfwKey) const;
    // First-person mouse look: hidden cursor, raw motion, deltas without a held button.
    void setCursorCaptured(bool on);
    [[nodiscard]] bool cursorCaptured() const { return captured_; }

private:
    GLFWwindow* window_ = nullptr;
    InputFrame pending_;
    f64 lastX_ = 0, lastY_ = 0;
    bool dragging_ = false;
    bool captured_ = false;
    [[nodiscard]] InputEvent cursorEvent(InputEvent::Type t, f64 x, f64 y) const; // window → framebuffer px
};

} // namespace phx
