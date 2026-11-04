#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/data/Tables.h"

#include <functional>
#include <string>
#include <vector>

// Draws field declarations (reflect::TypeInfo) as ImGui widgets. Shared by inspector object fields, the tuning panel, the [Tables] panel, and the live state view.
namespace mealkit::editor {

// Editor data the widgets refer to: table list for picking row references (RowRef) (if absent, ids are edited as text)
struct FieldContext {
    const data::TableIndexList* tables = nullptr;
    // Default instance (same type). If present, dots mark fields different from the default and right-click "Reset to default" is enabled
    const void* defaults = nullptr;
};

// MEALKIT_FIELD display name: "name|description|group=Pushing|range=0..20|step=0.1|unit=m|pick=effects:.fx". Everything after the name is optional.
// Only the editor reads these, and labels stay out of the content hash, so changing them doesn't change simulation results.
//   group  starts a collapsible section that lasts until the next group in the same struct
//   range  slider bounds for numbers (seconds for Ticks). Soft: Ctrl+click can type a value outside them
//   step   drag speed. Without it, the speed follows the size of the value
//   unit   text after a number
//   pick   searchable list for a string field: "sound" (sound events) or "<folder under assets>:<extension>" (file names without the extension)
struct FieldLabel {
    std::string name;
    std::string tooltip;
    std::string group;
    std::string unit;
    std::string pick;
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

// Input field that edits a std::string directly (grows the string if the buffer is too small)
bool inputString(const char* label, std::string& text);

// Read-only table (name | value). Structs/arrays/Pools can be expanded. Large Pools draw only visible rows.
void drawFieldsReadOnly(const reflect::TypeInfo& type, const void* data);

// One "name | value" row: draws the name in the left column (shortened with its full text in a tooltip), makes the next
// widget fill the value column, and returns the hidden label to give that widget:
//   ImGui::DragFloat(propertyRow(tr("Speed")).c_str(), &speed);
std::string propertyRow(const char* label, const char* tooltip = nullptr);

// Inspector section: a neutral bar, so the accent color stays for selection and active states
bool sectionHeader(const char* label, int treeNodeFlags = 0);

} // namespace mealkit::editor
