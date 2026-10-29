#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/data/Tables.h"

// Draws field declarations (reflect::TypeInfo) as ImGui widgets. Shared by inspector object fields, the tuning panel, the [Tables] panel, and the live state view.
namespace mealkit::editor {

// Editor data the widgets refer to: table list for picking row references (RowRef) (if absent, ids are edited as text)
struct FieldContext {
    const data::TableIndexList* tables = nullptr;
    // Default instance (same type). If present, dots mark fields different from the default and right-click "Reset to default" is enabled
    const void* defaults = nullptr;
};

// Field display name "name|description": the description shows on hover (units/range). Used in MEALKIT_FIELD display names
struct FieldLabel {
    std::string name;
    std::string tooltip;
};
FieldLabel splitFieldLabel(const char* label);

// Editable field widget. true if the value changed. Containers (Pool) are shown read-only.
bool drawFields(const reflect::TypeInfo& type, void* data, const FieldContext* context = nullptr);

// Widget for one value (a table cell). If label starts with "##", no name. true if changed.
bool drawValueWidget(const char* label, const char* name, const reflect::ValueInfo& value, void* data,
                     const FieldContext* context = nullptr);

// Read-only table (name | value). Structs/arrays/Pools can be expanded. Large Pools draw only visible rows.
void drawFieldsReadOnly(const reflect::TypeInfo& type, const void* data);

} // namespace mealkit::editor
