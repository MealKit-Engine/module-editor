#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/debug/ImGuiLayer.h"

namespace mealkit::editor {

// 에디터 창의 제목 표시줄 색 (0xRRGGBB). ImGui 메뉴 막대 색(applyStyle의 MenuBarBg)과 맞춘다
constexpr u32 kEditorCaptionColor = 0x1A1C1F;

// 아이콘 글자 (Windows 기본 글꼴 Segoe Fluent Icons / Segoe MDL2 Assets를 기본 글꼴에 합친다). 글자와 섞어 쓴다: "\xEE\x9C\x90 New"
namespace icon {
inline constexpr const char* kAdd = "\xEE\x9C\x90";        // U+E710
inline constexpr const char* kFolder = "\xEE\xA2\xB7";     // U+E8B7
inline constexpr const char* kSearch = "\xEE\x9C\xA1";     // U+E721
inline constexpr const char* kGlobe = "\xEE\x9D\xB4";      // U+E774
inline constexpr const char* kStar = "\xEE\x9C\xB4";       // U+E734 (빈 별)
inline constexpr const char* kStarFilled = "\xEE\x9C\xB5"; // U+E735
} // namespace icon

using ImGuiLayer = debug::ImGuiLayer;

} // namespace mealkit::editor
