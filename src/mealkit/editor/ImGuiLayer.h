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
inline constexpr const char* kView = "\xEE\xA2\x90";       // U+E890 (보이기)
inline constexpr const char* kHide = "\xEE\xB4\x9A";       // U+ED1A
inline constexpr const char* kLock = "\xEE\x9C\xAE";       // U+E72E
inline constexpr const char* kUnlock = "\xEE\x9E\x85";     // U+E785
// 씬 트리 종류 아이콘
inline constexpr const char* kBox = "\xEE\x9C\xB9";        // U+E739 (상자)
inline constexpr const char* kModel = "\xEE\x9E\xB8";      // U+E7B8 (모델)
inline constexpr const char* kPin = "\xEE\x9C\x87";        // U+E707 (스폰·표시)
inline constexpr const char* kArea = "\xEE\xA0\x8A";       // U+E80A (영역)
inline constexpr const char* kLight = "\xEE\xAA\x80";      // U+EA80 (점광원)
inline constexpr const char* kProbe = "\xEE\xA4\x89";      // U+E909 (반사 프로브)
inline constexpr const char* kFlag = "\xEE\x9F\x81";       // U+E7C1 (그 밖의 게임 종류)
} // namespace icon

using ImGuiLayer = debug::ImGuiLayer;

} // namespace mealkit::editor
