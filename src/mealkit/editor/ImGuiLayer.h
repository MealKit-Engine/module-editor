#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/debug/ImGuiLayer.h"

namespace mealkit::editor {

// Title bar color of editor windows (0xRRGGBB). Matches the ImGui menu bar color (MenuBarBg in applyStyle)
constexpr u32 kEditorCaptionColor = 0x111111; // gray 1 of docs/TOOL_DESIGN.md 3.1, the gutter

// Icon glyphs: Fluent UI System Icons (third_party/fluent-icons, 20 px regular faces) merged into the default font on every OS.
// Comments give the Fluent icon name (ic_fluent_<name>_20_regular). Mixed with text: icon::kAdd + std::string(" New")
namespace icon {
inline constexpr const char* kAdd = "\xEF\x84\x89";        // add
inline constexpr const char* kFolder = "\xEF\x90\x98";     // folder
inline constexpr const char* kSearch = "\xEF\x9A\x8F";     // search
inline constexpr const char* kGlobe = "\xEF\x91\x9A";      // globe
inline constexpr const char* kStar = "\xEF\x9C\x8F";       // star (empty star)
inline constexpr const char* kStarFilled = "\xEE\xAE\xAA"; // star_emphasis
inline constexpr const char* kView = "\xEE\x97\xB2";       // eye (visible)
inline constexpr const char* kHide = "\xEE\x97\xB5";       // eye_off
inline constexpr const char* kLock = "\xEE\x9E\x8F";       // lock_closed
inline constexpr const char* kUnlock = "\xEE\x9E\x95";     // lock_open
// Scene tree kind icons
inline constexpr const char* kBox = "\xEF\x8A\x91";        // checkbox_unchecked (box)
inline constexpr const char* kModel = "\xEF\x8C\xB5";      // cube (model)
inline constexpr const char* kPin = "\xEF\x93\xB8";        // location (spawn/marker)
inline constexpr const char* kArea = "\xEE\xAA\x85";       // select_all_off (area)
inline constexpr const char* kLight = "\xEF\x93\x96";      // lightbulb (point light)
inline constexpr const char* kCamera = "\xEF\x89\x94";     // camera (virtual camera)
inline constexpr const char* kProbe = "\xEF\x8A\xBD";      // circle_half_fill (reflection probe)
inline constexpr const char* kFlag = "\xEF\x90\x8B";       // flag (other game kinds)
inline constexpr const char* kPrefab = "\xEE\x9D\xA2";    // library (prefab file, library)
// Toolbars, tabs and page lists
inline constexpr const char* kChevronDown = "\xEF\x8A\xA3"; // chevron_down (collapsible open, combo)
inline constexpr const char* kChevronRight = "\xEF\x8A\xB0"; // chevron_right (collapsible closed)
inline constexpr const char* kPlay = "\xEF\x98\x85"; // play
inline constexpr const char* kStop = "\xEF\x9C\xAA"; // stop
inline constexpr const char* kPeople = "\xEF\x96\xA8"; // people (player count)
inline constexpr const char* kCopy = "\xEF\x8C\xAB"; // copy (duplicate)
inline constexpr const char* kTrash = "\xEF\x8D\x8C"; // delete
inline constexpr const char* kSettings = "\xEF\x9A\xA9"; // settings
inline constexpr const char* kMove = "\xEF\xA3\xA5"; // arrow_move (gizmo)
inline constexpr const char* kRotate = "\xEF\x86\x85"; // arrow_rotate_clockwise (gizmo)
inline constexpr const char* kScale = "\xEF\x99\xAC"; // resize (gizmo)
inline constexpr const char* kCube = "\xEE\x90\xBB"; // cube_tree (local axes)
inline constexpr const char* kSnap = "\xEE\x9B\x84"; // grid_dots
inline constexpr const char* kFocus = "\xEE\xA9\x94"; // scan (frame the selection)
inline constexpr const char* kPerspective = "\xEE\x90\xB5"; // cube_quick
inline constexpr const char* kOrtho = "\xEE\xAD\xB5"; // square
inline constexpr const char* kLit = "\xEF\xA2\xA1"; // weather_sunny (view mode)
inline constexpr const char* kSpeed = "\xEE\xB6\x9B"; // top_speed (camera speed)
inline constexpr const char* kTree = "\xEF\x9E\xAE"; // text_bullet_list_tree (scene tree)
inline constexpr const char* kInspector = "\xEE\xA2\xBD"; // panel_right
inline constexpr const char* kData = "\xEF\x9D\x9D"; // table
inline constexpr const char* kPreview = "\xEF\x8D\x99"; // desktop
inline constexpr const char* kDebug = "\xEE\x88\x86"; // bug
inline constexpr const char* kLog = "\xEE\xB3\x99"; // text_bullet_list_ltr
inline constexpr const char* kBuild = "\xEE\xBA\x86"; // wrench
inline constexpr const char* kPerformance = "\xEF\x93\x87"; // gauge
inline constexpr const char* kSound = "\xEE\xAD\x82"; // speaker_2
inline constexpr const char* kState = "\xEE\x91\x93"; // data_area
inline constexpr const char* kTests = "\xEF\x87\x97"; // beaker
inline constexpr const char* kBalance = "\xEE\xA9\x90"; // scales
inline constexpr const char* kInfo = "\xEF\x92\xA3"; // info
inline constexpr const char* kWarning = "\xEF\xA1\xA9"; // warning
inline constexpr const char* kError = "\xEF\x8F\xB1"; // error_circle
// Actions, one glyph per action across the suite (engine docs/TOOL_DESIGN.md "Glyphs")
inline constexpr const char* kPause = "\xEF\x96\xA1"; // pause
inline constexpr const char* kStep = "\xEF\x95\xA9"; // next (one tick forward)
inline constexpr const char* kSave = "\xEF\x99\xBF"; // save
inline constexpr const char* kUndo = "\xEF\x86\x99"; // arrow_undo (undo, revert)
inline constexpr const char* kRedo = "\xEF\x85\xAE"; // arrow_redo
inline constexpr const char* kRefresh = "\xEF\x84\xBD"; // arrow_clockwise (reload, rescan)
inline constexpr const char* kFolderOpen = "\xEF\x90\xAE"; // folder_open (show in Explorer)
inline constexpr const char* kLaunch = "\xEF\x96\x82"; // open (open in its program)
inline constexpr const char* kSync = "\xEF\x86\x90"; // arrow_sync (update)
inline constexpr const char* kDocument = "\xEF\x8D\xB8"; // document (docs, log)
inline constexpr const char* kClose = "\xEF\x8D\xA9"; // dismiss (close, dismiss)
inline constexpr const char* kRemove = "\xEE\xAF\x90"; // subtract (remove from a list)
inline constexpr const char* kClear = "\xEE\x97\xA4"; // eraser
inline constexpr const char* kAccept = "\xEF\x8A\x94"; // checkmark
inline constexpr const char* kMaximize = "\xEE\x9A\x84"; // full_screen_maximize (the viewport fills the window)
inline constexpr const char* kRestore = "\xEE\x9A\x87"; // full_screen_minimize (back to the layout)
} // namespace icon

using ImGuiLayer = debug::ImGuiLayer;

// Every checkbox and radio button goes through these, never ImGui::Checkbox/RadioButton (docs/TOOL_DESIGN.md 3.2, Controls.cpp)
bool checkbox(const char* label, bool* v);
bool radioButton(const char* label, bool active);

} // namespace mealkit::editor
