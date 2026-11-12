#pragma once

#include <imgui.h>

#include "mealkit/core/Reflect.h"
#include "mealkit/data/Tables.h"

#include <functional>
#include <set>
#include <string>
#include <vector>

// Draws field declarations (reflect::TypeInfo) as ImGui widgets. Shared by inspector object fields, the tuning panel, the [Tables] panel, and the live state view.
namespace mealkit::editor {

// Editor data the widgets refer to: table list for picking row references (RowRef) (if absent, ids are edited as text)
struct FieldContext {
    const data::TableIndexList* tables = nullptr;
    // Default instance (same type). If present, dots mark fields different from the default and right-click "Reset to default" is enabled
    const void* defaults = nullptr;
    // Multi-selection: names of top-level fields whose values differ between the selected objects (marked with a dash)
    const std::set<std::string>* mixed = nullptr;
    // Search: only fields whose name or description contains this text (empty = all); onlyChanged: only those different from defaults
    const char* filter = nullptr;
    bool onlyChanged = false;
};

// MEALKIT_FIELD display name: "name|description|group=Pushing|range=0..20|step=0.1|unit=m|pick=effects:.fx". Everything after the name is optional.
// Only the editor reads these, and labels stay out of the content hash, so changing them doesn't change simulation results.
//   group  starts a collapsible section that lasts until the next group in the same struct
//   range  slider bounds for numbers (seconds for Ticks). Soft: Ctrl+click can type a value outside them
//   step   drag speed. Without it, the speed follows the size of the value
//   unit   text after a number
//   gizmo  "radius" on an f32 field of a scene object: a sphere around the selected object in the scene view, with a handle to drag
//   showIf "teams>0", "teams" or "sound": hides the field unless the other top-level field passes (> < = !, or not zero/empty)
//   required  an empty string is marked and goes to the [Problems] list
//   pick   searchable list for a string field: "sound" (sound events) or "<folder under assets>:<extension>" (file names without the extension)
struct FieldLabel {
    std::string name;
    std::string tooltip;
    std::string group;
    std::string unit;
    std::string pick;
    std::string gizmo;
    std::string showIf;
    bool required = false;
    f32 min = 0.0f, max = 0.0f, step = 0.0f;
    bool hasRange = false;
};
FieldLabel splitFieldLabel(const char* label);

// Editable field widget. true if the value changed. Containers (Pool) are shown read-only.
bool drawFields(const reflect::TypeInfo& type, void* data, const FieldContext* context = nullptr);

// Widget for one value (a table cell). If label starts with "##", no name. true if changed.
bool drawValueWidget(const char* label, const char* name, const reflect::ValueInfo& value, void* data,
                     const FieldContext* context = nullptr, const FieldLabel* attributes = nullptr);

// Where pick=sound gets its names (the editor's audio engine). Empty function = typed by hand.
void setSoundEventSource(std::function<std::vector<std::string>()> source);
// The play button next to a pick= field: (pick, value). Empty function = no button.
void setPickPreview(std::function<void(const std::string&, const std::string&)> preview);
// Adds a line to out for each required field that is empty and not hidden by showIf
void collectRequired(const reflect::TypeInfo& type, const void* data, const std::string& where, std::vector<std::string>& out);

// Input field that edits a std::string directly (grows the string if the buffer is too small)
bool inputString(const char* label, std::string& text);

// Read-only table (name | value). Structs/arrays/Pools can be expanded. Large Pools draw only visible rows.
void drawFieldsReadOnly(const reflect::TypeInfo& type, const void* data);

// One "name | value" row: draws the name in the left column (shortened with its full text in a tooltip), makes the next
// widget fill the value column, and returns the hidden label to give that widget:
//   ImGui::DragFloat(propertyRow(tr("Speed")).c_str(), &speed);
std::string propertyRow(const char* label, const char* tooltip = nullptr);

// Inspector section: space above, a small chevron and dimmed text instead of a bar. Opens and closes like ImGui::CollapsingHeader
bool sectionHeader(const char* label, int treeNodeFlags = 0);

// ImGui::BeginCombo with a small dim chevron instead of the large arrow button. Every editor combo goes through this
bool beginCombo(const char* label, const char* preview, int comboFlags = 0);

// Square button showing only an icon glyph, centered by the glyph's own bounds; the tooltip names it. Clear until hovered, or filled with the accent when active
bool iconButton(const char* glyph, const char* tooltip, bool active = false);
// A flat button with an icon and a short value after it (a count, a version); the tooltip names it (engine docs/TOOL_DESIGN.md)
bool iconValueButton(const char* glyph, const std::string& value, const char* tooltip);
// The same button with a drawn magnet (no such glyph in the icon fonts)
bool magnetButton(const char* tooltip, bool active = false);
// Draws one icon glyph centered in a rect. A button's label won't do: it starts at the frame padding when the glyph is wider than the room inside
void drawGlyph(ImDrawList* draw, const char* glyph, ImVec2 min, ImVec2 max, f32 size, unsigned int color);

// Color row: input fields and a swatch opening a compact picker (RGB, HSV, hex; resizable). flags: extra ImGuiColorEditFlags. Every editor color goes through this
bool colorEdit(const char* label, f32* color, bool alpha = false, int flags = 0);
// Picker width in font heights, shared by all pickers (the editor keeps it in its preferences)
f32& colorPickerSize();

// Left page/folder list of a window: resizable, no border, the selection bar has room around its text. Close with ImGui::EndChild()
bool beginListPane(const char* id, f32 width);

// "<icon>  <translated title>###<id>" for a window title, from a trl() style label
std::string iconTitle(const char* glyph, const char* label);

} // namespace mealkit::editor
