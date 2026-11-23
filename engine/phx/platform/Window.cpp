#include "phx/platform/Window.hpp"

#include "phx/core/Log.hpp"

#include <GLFW/glfw3.h>

namespace phx {

Window::Window(u32 width, u32 height, const char* title) {
    glfwSetErrorCallback([](int code, const char* msg) { log::error("GLFW error {}: {}", code, msg); });
    if (!glfwInit()) PHX_FATAL("glfwInit failed");
    if (!glfwVulkanSupported()) PHX_FATAL("GLFW reports no Vulkan loader");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    window_ = glfwCreateWindow(int(width), int(height), title, nullptr, nullptr);
    if (!window_) PHX_FATAL("glfwCreateWindow failed");
    glfwSetWindowUserPointer(window_, this);

    glfwSetKeyCallback(window_, [](GLFWwindow* w, int key, int, int action, int mods) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
        if (action == GLFW_PRESS || action == GLFW_REPEAT) self->pending_.keysPressed.push_back(key);
        self->pending_.events.push_back({InputEvent::Type::Key, key, mods, action != GLFW_RELEASE});
    });
    glfwSetCharCallback(window_, [](GLFWwindow* w, unsigned int cp) {
        static_cast<Window*>(glfwGetWindowUserPointer(w))->pending_.events.push_back({InputEvent::Type::Char, int(cp)});
    });
    glfwSetScrollCallback(window_, [](GLFWwindow* w, double, double dy) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
        self->pending_.scroll += dy;
        self->pending_.events.push_back({InputEvent::Type::Scroll, 0, 0, false, 0, dy});
    });
    glfwSetMouseButtonCallback(window_, [](GLFWwindow* w, int button, int action, int mods) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
        self->dragging_ = action == GLFW_PRESS;
        glfwGetCursorPos(w, &self->lastX_, &self->lastY_);
        InputEvent e = self->cursorEvent(InputEvent::Type::Button, self->lastX_, self->lastY_);
        e.code = button, e.mods = mods, e.down = action == GLFW_PRESS;
        self->pending_.events.push_back(e);
    });
    glfwSetCursorPosCallback(window_, [](GLFWwindow* w, double x, double y) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
        if (!self->captured_) self->pending_.events.push_back(self->cursorEvent(InputEvent::Type::Move, x, y));
        if (self->dragging_ || self->captured_) {
            self->pending_.mouseDx += x - self->lastX_;
            self->pending_.mouseDy += y - self->lastY_;
        }
        self->lastX_ = x;
        self->lastY_ = y;
    });
}

Window::~Window() {
    if (window_) glfwDestroyWindow(window_);
    glfwTerminate();
}

InputEvent Window::cursorEvent(InputEvent::Type t, f64 x, f64 y) const {
    int ww = 0, wh = 0, fw = 0, fh = 0; // HiDPI: GLFW cursor is in window coordinates
    glfwGetWindowSize(window_, &ww, &wh);
    glfwGetFramebufferSize(window_, &fw, &fh);
    InputEvent e{t};
    e.x = ww > 0 ? x * f64(fw) / f64(ww) : x;
    e.y = wh > 0 ? y * f64(fh) / f64(wh) : y;
    return e;
}

bool Window::shouldClose() const { return glfwWindowShouldClose(window_); }
void Window::setCursorCaptured(bool on) {
    captured_ = on;
    glfwSetInputMode(window_, GLFW_CURSOR, on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (on && glfwRawMouseMotionSupported()) glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    glfwGetCursorPos(window_, &lastX_, &lastY_);
}

void Window::requestClose() { glfwSetWindowShouldClose(window_, GLFW_TRUE); }
void Window::pollEvents() { glfwPollEvents(); }
void Window::waitEvents(double timeoutSeconds) { glfwWaitEventsTimeout(timeoutSeconds); }
void Window::setTitle(const std::string& title) { glfwSetWindowTitle(window_, title.c_str()); }

bool Window::keyDown(int key) const { return glfwGetKey(window_, key) == GLFW_PRESS; }

InputFrame Window::drainInput() {
    InputFrame out = std::move(pending_);
    pending_ = {};
    return out;
}

Extent2D Window::framebufferSize() const {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    return {u32(w > 0 ? w : 0), u32(h > 0 ? h : 0)};
}

} // namespace phx
