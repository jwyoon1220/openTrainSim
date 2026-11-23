#pragma once
// Bridges window input (GLFW codes, phx::InputEvent) to the UI library (phx::ui), which is
// itself platform-free.
#include "phx/platform/Window.hpp"
#include "phx/ui/Ui.hpp"

namespace phx {

[[nodiscard]] ui::Key uiKeyFromGlfw(int glfwKey);
[[nodiscard]] ui::MouseButton uiButtonFromGlfw(int glfwButton);

// Feeds one window event (framebuffer pixels) into `ui`; true when the UI consumed it.
bool feedUi(ui::Ui& ui, const InputEvent& event);

} // namespace phx
