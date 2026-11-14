#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/debug/ImGuiLayer.h"

namespace mealkit::editor {

// Title bar color of editor windows (0xRRGGBB). Matches the ImGui menu bar color (MenuBarBg in applyStyle)
constexpr u32 kEditorCaptionColor = 0x111111; // gray 1 of docs/TOOL_DESIGN.md 3.1, the gutter

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
inline constexpr const char* kCamera = "\xEE\x9C\xA2";     // U+E722 (virtual camera)
inline constexpr const char* kProbe = "\xEE\xA4\x89";      // U+E909 (reflection probe)
inline constexpr const char* kFlag = "\xEE\x9F\x81";       // U+E7C1 (other game kinds)
inline constexpr const char* kPrefab = "\xEE\xA3\xB1";    // U+E8F1 (prefab file, library)
// Toolbars, tabs and page lists
inline constexpr const char* kChevronDown = "\xEE\x9C\x8D"; // U+E70D (collapsible open, combo)
inline constexpr const char* kChevronRight = "\xEE\x9D\xAC"; // U+E76C (collapsible closed)
inline constexpr const char* kPlay = "\xEE\x9D\xA8"; // U+E768
inline constexpr const char* kStop = "\xEE\x9C\x9A"; // U+E71A
inline constexpr const char* kPeople = "\xEE\x9C\x96"; // U+E716 (player count)
inline constexpr const char* kCopy = "\xEE\xA3\x88"; // U+E8C8 (duplicate)
inline constexpr const char* kTrash = "\xEE\x9D\x8D"; // U+E74D
inline constexpr const char* kSettings = "\xEE\x9C\x93"; // U+E713
inline constexpr const char* kMove = "\xEE\x9F\x82"; // U+E7C2 (gizmo)
inline constexpr const char* kRotate = "\xEE\x9E\xAD"; // U+E7AD (gizmo)
inline constexpr const char* kScale = "\xEE\x9D\x80"; // U+E740 (gizmo)
inline constexpr const char* kCube = "\xEF\x85\x98"; // U+F158 (local axes)
inline constexpr const char* kSnap = "\xEE\xB3\xA9"; // U+ECE9
inline constexpr const char* kFocus = "\xEE\xA6\xA6"; // U+E9A6 (frame the selection)
inline constexpr const char* kPerspective = "\xEE\xA0\x89"; // U+E809
inline constexpr const char* kOrtho = "\xEE\xA2\xA9"; // U+E8A9
inline constexpr const char* kLit = "\xEE\x9E\x93"; // U+E793 (view mode)
inline constexpr const char* kSpeed = "\xEE\xB1\x8A"; // U+EC4A (camera speed)
inline constexpr const char* kTree = "\xEE\xA3\xBD"; // U+E8FD (scene tree)
inline constexpr const char* kInspector = "\xEE\xA7\xA9"; // U+E9E9
inline constexpr const char* kData = "\xEE\xA3\xAF"; // U+E8EF
inline constexpr const char* kPreview = "\xEE\x9F\xB4"; // U+E7F4
inline constexpr const char* kDebug = "\xEE\xAF\xA8"; // U+EBE8
inline constexpr const char* kLog = "\xEE\xA8\xB7"; // U+EA37
inline constexpr const char* kBuild = "\xEE\xA4\x8F"; // U+E90F
inline constexpr const char* kPerformance = "\xEE\xA7\x99"; // U+E9D9
inline constexpr const char* kSound = "\xEE\x9D\xA7"; // U+E767
inline constexpr const char* kState = "\xEE\xA7\xB5"; // U+E9F5
inline constexpr const char* kTests = "\xEE\xA7\x95"; // U+E9D5
inline constexpr const char* kBalance = "\xEE\xA7\x92"; // U+E9D2
inline constexpr const char* kInfo = "\xEE\xA5\x86"; // U+E946
inline constexpr const char* kWarning = "\xEE\x9E\xBA"; // U+E7BA
inline constexpr const char* kError = "\xEE\xA8\xB9"; // U+EA39
// Actions, one glyph per action across the suite (engine docs/TOOL_DESIGN.md "Glyphs")
inline constexpr const char* kPause = "\xEE\x9D\xA9"; // U+E769
inline constexpr const char* kStep = "\xEE\xA2\x93"; // U+E893 (one tick forward)
inline constexpr const char* kSave = "\xEE\x9D\x8E"; // U+E74E
inline constexpr const char* kUndo = "\xEE\x9E\xA7"; // U+E7A7 (undo, revert)
inline constexpr const char* kRedo = "\xEE\x9E\xA6"; // U+E7A6
inline constexpr const char* kRefresh = "\xEE\x9C\xAC"; // U+E72C (reload, rescan)
inline constexpr const char* kFolderOpen = "\xEE\xA0\xB8"; // U+E838 (show in Explorer)
inline constexpr const char* kLaunch = "\xEE\xA2\xA7"; // U+E8A7 (open in its program)
inline constexpr const char* kSync = "\xEE\xA2\x95"; // U+E895 (update)
inline constexpr const char* kDocument = "\xEE\xA2\xA5"; // U+E8A5 (docs, log)
inline constexpr const char* kClose = "\xEE\x9C\x91"; // U+E711 (close, dismiss)
inline constexpr const char* kRemove = "\xEE\x9C\xB8"; // U+E738 (remove from a list)
inline constexpr const char* kClear = "\xEE\xA2\x94"; // U+E894
inline constexpr const char* kAccept = "\xEE\x9C\xBE"; // U+E73E
inline constexpr const char* kMaximize = "\xEE\xA4\xA2"; // U+E922 (the viewport fills the window)
inline constexpr const char* kRestore = "\xEE\xA4\xA3"; // U+E923 (back to the layout)
} // namespace icon

using ImGuiLayer = debug::ImGuiLayer;

// Every checkbox and radio button goes through these, never ImGui::Checkbox/RadioButton (docs/TOOL_DESIGN.md 3.2, Controls.cpp)
bool checkbox(const char* label, bool* v);
bool radioButton(const char* label, bool active);

} // namespace mealkit::editor
