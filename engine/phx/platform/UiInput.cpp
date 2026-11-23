#include "phx/platform/UiInput.hpp"

#include <GLFW/glfw3.h>

namespace phx {

ui::Key uiKeyFromGlfw(int k) {
    switch (k) {
    case GLFW_KEY_TAB: return ui::Key::Tab;
    case GLFW_KEY_ENTER:
    case GLFW_KEY_KP_ENTER: return ui::Key::Enter;
    case GLFW_KEY_ESCAPE: return ui::Key::Escape;
    case GLFW_KEY_LEFT: return ui::Key::Left;
    case GLFW_KEY_RIGHT: return ui::Key::Right;
    case GLFW_KEY_UP: return ui::Key::Up;
    case GLFW_KEY_DOWN: return ui::Key::Down;
    case GLFW_KEY_HOME: return ui::Key::Home;
    case GLFW_KEY_END: return ui::Key::End;
    case GLFW_KEY_BACKSPACE: return ui::Key::Backspace;
    case GLFW_KEY_DELETE: return ui::Key::Delete;
    case GLFW_KEY_SPACE: return ui::Key::Space;
    case GLFW_KEY_PAGE_UP: return ui::Key::PageUp;
    case GLFW_KEY_PAGE_DOWN: return ui::Key::PageDown;
    default: return ui::Key::Other;
    }
}

ui::MouseButton uiButtonFromGlfw(int b) {
    return b == GLFW_MOUSE_BUTTON_RIGHT ? ui::MouseButton::Right
         : b == GLFW_MOUSE_BUTTON_MIDDLE ? ui::MouseButton::Middle
                                         : ui::MouseButton::Left;
}

bool feedUi(ui::Ui& ui, const InputEvent& e) {
    switch (e.type) {
    case InputEvent::Type::Key:
        return ui.key(uiKeyFromGlfw(e.code), e.down, e.mods & GLFW_MOD_SHIFT, e.mods & GLFW_MOD_CONTROL);
    case InputEvent::Type::Button: {
        const bool moved = ui.mouseMove(f32(e.x), f32(e.y));
        return ui.mouseButton(uiButtonFromGlfw(e.code), e.down) || moved;
    }
    case InputEvent::Type::Move: return ui.mouseMove(f32(e.x), f32(e.y));
    case InputEvent::Type::Scroll: return ui.mouseWheel(f32(e.y));
    case InputEvent::Type::Char: return ui.character(u32(e.code));
    }
    return false;
}

} // namespace phx
