#pragma once

#include <imgui.h>

// One meaning, one color across the editor panels.
namespace mealkit::editor::color {

inline const ImVec4 kDanger(1.0f, 0.45f, 0.4f, 1.0f);    // errors, missing references, over budget
inline const ImVec4 kWarning(0.95f, 0.75f, 0.4f, 1.0f);  // warnings, things that won't apply yet
inline const ImVec4 kGood(0.55f, 0.85f, 0.55f, 1.0f);    // in sync, applied at once
inline const ImVec4 kChanged(0.45f, 0.70f, 1.0f, 1.0f);  // differs from the default
inline const ImVec4 kPlay(0.95f, 0.55f, 0.15f, 1.0f);    // playing

} // namespace mealkit::editor::color
