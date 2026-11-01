#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/debug/ImGuiLayer.h"

namespace mealkit::editor {

// Title bar color of editor windows (0xRRGGBB). Matches the ImGui menu bar color (MenuBarBg in applyStyle)
constexpr u32 kEditorCaptionColor = 0x1A1C1F;

// Icon glyphs (Windows default fonts Segoe Fluent Icons / Segoe MDL2 Assets merged into the default font). Mixed with text: "\xEE\x9C\x90 New"
namespace icon {
inline constexpr const char* kAdd = "\xEE\x9C\x90";        // U+E710
inline constexpr const char* kFolder = "\xEE\xA2\xB7";     // U+E8B7
inline constexpr const char* kSearch = "\xEE\x9C\xA1";     // U+E721
inline constexpr const char* kGlobe = "\xEE\x9D\xB4";      // U+E774
inline constexpr const char* kStar = "\xEE\x9C\xB4";       // U+E734 (empty star)
inline constexpr const char* kStarFilled = "\xEE\x9C\xB5"; // U+E735
inline constexpr const char* kView = "\xEE\xA2\x90";       // U+E890 (visible)
inline constexpr const char* kHide = "\xEE\xB4\x9A";       // U+ED1A
inline constexpr const char* kLock = "\xEE\x9C\xAE";       // U+E72E
inline constexpr const char* kUnlock = "\xEE\x9E\x85";     // U+E785
// Scene tree kind icons
inline constexpr const char* kBox = "\xEE\x9C\xB9";        // U+E739 (box)
inline constexpr const char* kModel = "\xEE\x9E\xB8";      // U+E7B8 (model)
inline constexpr const char* kPin = "\xEE\x9C\x87";        // U+E707 (spawn/marker)
inline constexpr const char* kArea = "\xEE\xA0\x8A";       // U+E80A (area)
inline constexpr const char* kLight = "\xEE\xAA\x80";      // U+EA80 (point light)
inline constexpr const char* kCamera = "î¢";     // U+E722 (virtual camera)
inline constexpr const char* kProbe = "\xEE\xA4\x89";      // U+E909 (reflection probe)
inline constexpr const char* kFlag = "\xEE\x9F\x81";       // U+E7C1 (other game kinds)
} // namespace icon

using ImGuiLayer = debug::ImGuiLayer;

} // namespace mealkit::editor
