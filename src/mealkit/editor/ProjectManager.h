#pragma once

#include <string>

namespace mealkit {
class Window;
namespace render {
class Renderer;
}
} // namespace mealkit

namespace mealkit::editor {

// Project manager: shown when the editor runs without a project (recent projects, open, new project).
// Returns the chosen project folder. Empty string if the window is closed.
// engineDirectory: engine folder containing tools\new_project.ps1, used when creating a new project.
// maxFrames/capturePath: for automated tests (draws that many frames, saves a screenshot, then returns an empty string).
std::string runProjectManager(Window& window, render::Renderer& renderer, const std::string& engineDirectory,
                              unsigned maxFrames = 0, const std::string& capturePath = {});

} // namespace mealkit::editor
