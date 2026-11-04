#include "mealkit/editor/FieldEditor.h"

#include "mealkit/core/Hash.h"

#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h> // RenderTextEllipsis

namespace mealkit::editor {
namespace {

using reflect::FieldInfo;
using reflect::Kind;
using reflect::TypeInfo;
using reflect::ValueInfo;

const u8* at(const void* data, u32 offset) { return static_cast<const u8*>(data) + offset; }
u8* at(void* data, u32 offset) { return static_cast<u8*>(data) + offset; }

ImGuiDataType dataType(const ValueInfo& v) {
    const bool s = v.kind == Kind::Int;
    switch (v.size) {
    case 1: return s ? ImGuiDataType_S8 : ImGuiDataType_U8;
    case 2: return s ? ImGuiDataType_S16 : ImGuiDataType_U16;
    case 4: return s ? ImGuiDataType_S32 : ImGuiDataType_U32;
    default: return s ? ImGuiDataType_S64 : ImGuiDataType_U64;
    }
}

void nameTooltip(const char* name) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", name);
}

// List of live slots (for Pool display)
std::vector<u32> liveSlots(const ValueInfo& v, const void* data) {
    std::vector<u32> slots;
    const u32 capacity = v.ops->capacity(data);
    for (u32 slot = 0; slot < capacity; ++slot)
        if (v.ops->live(data, slot)) slots.push_back(slot);
    return slots;
}

void drawReadOnlyValue(const char* label, const char* name, const ValueInfo& v, const void* data);

void drawReadOnlyStruct(const TypeInfo& type, const void* data) {
    for (const FieldInfo& f : type.fields) {
        ImGui::PushID(f.name);
        drawReadOnlyValue(trData(splitFieldLabel(f.label).name.c_str()), f.name, *f.value, at(data, f.offset));
        ImGui::PopID();
    }
}

bool treeRow(const char* label, const char* name, const char* summary) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    const bool open = ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_SpanFullWidth);
    nameTooltip(name);
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", summary);
    return open;
}

void drawReadOnlyValue(const char* label, const char* name, const ValueInfo& v, const void* data) {
    switch (v.kind) {
    case Kind::Struct:
        if (treeRow(label, name, v.type->name)) {
            drawReadOnlyStruct(*v.type, data);
            ImGui::TreePop();
        }
        break;
    case Kind::Array: {
        const std::string summary = "[" + std::to_string(v.count) + "]";
        if (treeRow(label, name, summary.c_str())) {
            for (u32 i = 0; i < v.count; ++i) {
                const std::string item = "[" + std::to_string(i) + "]";
                ImGui::PushID(static_cast<int>(i));
                drawReadOnlyValue(item.c_str(), name, *v.element, at(data, i * v.element->size));
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        break;
    }
    case Kind::Container: {
        const std::vector<u32> slots = liveSlots(v, data);
        const std::string summary = std::to_string(slots.size()) + " / " + std::to_string(v.ops->capacity(data));
        if (treeRow(label, name, summary.c_str())) {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(slots.size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const u32 slot = slots[static_cast<size_t>(row)];
                    const std::string item =
                        "#" + trf("%u (generation %u)", static_cast<unsigned>(slot), static_cast<unsigned>(v.ops->generation(data, slot)));
                    ImGui::PushID(static_cast<int>(slot));
                    drawReadOnlyValue(item.c_str(), name, *v.element, v.ops->item(data, slot));
                    ImGui::PopID();
                }
            }
            ImGui::TreePop();
        }
        break;
    }
    default: {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                     ImGuiTreeNodeFlags_SpanFullWidth);
        nameTooltip(name);
        ImGui::TableNextColumn();
        const std::string text = reflect::formatScalar(v, data);
        ImGui::TextUnformatted(text.c_str());
        break;
    }
    }
}

std::function<std::vector<std::string>()> g_soundEvents;

std::string withUnit(std::string format, const FieldLabel* attributes) {
    if (!attributes || attributes->unit.empty()) return format;
    format += ' ';
    for (char c : attributes->unit) {
        if (c == '%') format += '%';
        format += c;
    }
    return format;
}

const char* intFormat(const ValueInfo& v) {
    if (v.kind == Kind::Int) return v.size == 8 ? "%lld" : "%d";
    return v.size == 8 ? "%llu" : "%u";
}

f32 dragSpeed(f32 value, const FieldLabel* attributes) {
    if (attributes && attributes->step > 0.0f) return attributes->step;
    return std::max(0.01f, std::fabs(value) * 0.01f);
}

// Choices for pick=: read at most once a second per source, since a table can draw this in many cells
const std::vector<std::string>& pickChoices(const std::string& pick) {
    struct Entry {
        double time = -1000.0;
        std::vector<std::string> names;
    };
    static std::map<std::string, Entry> cache;
    Entry& entry = cache[pick];
    const double now = ImGui::GetTime();
    if (now - entry.time < 1.0) return entry.names;
    entry.time = now;
    entry.names.clear();
    if (pick == "sound") {
        if (g_soundEvents) entry.names = g_soundEvents();
    } else {
        const size_t colon = pick.find(':');
        const std::string folder = pick.substr(0, colon), extension = colon == std::string::npos ? std::string() : pick.substr(colon + 1);
        for (const std::string& file : listFiles(projectRoot() + "\\assets\\" + folder)) {
            if (extension.empty()) entry.names.push_back(file);
            else if (file.size() > extension.size() && file.compare(file.size() - extension.size(), extension.size(), extension) == 0)
                entry.names.push_back(file.substr(0, file.size() - extension.size()));
        }
    }
    std::sort(entry.names.begin(), entry.names.end());
    return entry.names;
}

// String chosen from a list, with a search box. A value not in the list is kept and shown in red
bool drawPicker(const char* label, std::string& value, const std::string& pick) {
    const std::vector<std::string>& names = pickChoices(pick);
    if (names.empty()) return inputString(label, value); // nothing to pick from (no audio, no folder)
    const bool found = value.empty() || std::find(names.begin(), names.end(), value) != names.end();
    if (!found) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
    const std::string preview = value.empty() ? std::string(tr("(none)")) : value + (found ? "" : " (?)");
    const bool open = ImGui::BeginCombo(label, preview.c_str());
    if (!found) ImGui::PopStyleColor();
    if (!open) return false;
    static char search[64] = {};
    if (ImGui::IsWindowAppearing()) {
        search[0] = 0;
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", tr("Search"), search, sizeof(search));
    bool changed = false;
    if (ImGui::Selectable(tr("(none)"), value.empty())) {
        value.clear();
        changed = true;
    }
    const auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string needle = lower(search);
    for (const std::string& name : names) {
        if (!needle.empty() && lower(name).find(needle) == std::string::npos) continue;
        if (ImGui::Selectable(name.c_str(), name == value)) {
            value = name;
            changed = true;
        }
    }
    ImGui::EndCombo();
    return changed;
}

// Row reference: pick a row id of the referenced table (as text if the table is unknown)
bool drawRowRef(const char* label, const ValueInfo& v, void* data, const FieldContext* context) {
    const reflect::RowRefView ref = reflect::rowRefOf(data);
    const std::vector<std::string>* ids = nullptr;
    if (context && context->tables)
        for (const data::TableIndex& t : *context->tables)
            if (t.rowType == v.type || std::strcmp(t.rowType->name, v.type->name) == 0) ids = t.ids;
    if (!ids) return inputString(label, *ref.id);
    bool found = ref.id->empty();
    for (const std::string& id : *ids) found = found || id == *ref.id;
    if (!found) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
    const std::string preview = ref.id->empty() ? std::string(tr("(none)")) : *ref.id + (found ? "" : " (?)");
    bool changed = false;
    if (ImGui::BeginCombo(label, preview.c_str())) {
        if (!found) ImGui::PopStyleColor();
        if (ImGui::Selectable(tr("(none)"), ref.id->empty())) {
            ref.id->clear();
            changed = true;
        }
        for (const std::string& id : *ids) {
            if (ImGui::Selectable(id.c_str(), id == *ref.id)) {
                *ref.id = id;
                changed = true;
            }
        }
        ImGui::EndCombo();
    } else if (!found) {
        ImGui::PopStyleColor();
    }
    if (!found && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", trf("'%s' is not a row of the %s table", ref.id->c_str(), v.type->name).c_str());
    return changed;
}

bool drawValue(const char* label, const char* name, const ValueInfo& v, void* data, const FieldContext* context,
               const char* tooltip = nullptr, const FieldLabel* attributes = nullptr) {
    bool changed = false;
    // Scalars get a name column; "##" labels are table cells, and structs/arrays/Pools have their own rows
    std::string hidden;
    if (label[0] != '#' && v.kind != Kind::Struct && v.kind != Kind::Array && v.kind != Kind::Container) {
        hidden = propertyRow(label, tooltip);
        label = hidden.c_str();
    }
    switch (v.kind) {
    case Kind::Bool: changed = ImGui::Checkbox(label, static_cast<bool*>(data)); break;
    case Kind::Int:
    case Kind::UInt:
        if (!v.enumNames.empty()) { // MEALKIT_ENUM: pick a name
            u64 value = 0;
            std::memcpy(&value, data, v.size);
            const std::string preview = reflect::formatScalar(v, data);
            if (ImGui::BeginCombo(label, preview.c_str())) {
                for (size_t i = 0; i < v.enumNames.size(); ++i)
                    if (ImGui::Selectable(v.enumNames[i].c_str(), value == i)) {
                        const u64 next = i;
                        std::memcpy(data, &next, v.size); // little endian: from the low bytes
                        changed = true;
                    }
                ImGui::EndCombo();
            }
        } else {
            const std::string format = withUnit(intFormat(v), attributes);
            if (attributes && attributes->hasRange) {
                // Bounds as raw bytes of the field's own type (little endian: the low bytes)
                u64 low = static_cast<u64>(static_cast<i64>(attributes->min)), high = static_cast<u64>(static_cast<i64>(attributes->max));
                changed = ImGui::SliderScalar(label, dataType(v), data, &low, &high, format.c_str());
            } else {
                changed = ImGui::DragScalar(label, dataType(v), data, attributes && attributes->step > 0.0f ? attributes->step : 0.2f, nullptr, nullptr, format.c_str());
            }
        }
        break;
    case Kind::Float: {
        f32& value = *static_cast<f32*>(data);
        const std::string format = withUnit("%.3f", attributes);
        if (attributes && attributes->hasRange) changed = ImGui::SliderFloat(label, &value, attributes->min, attributes->max, format.c_str());
        else changed = ImGui::DragFloat(label, &value, dragSpeed(value, attributes), 0.0f, 0.0f, format.c_str());
        break;
    }
    case Kind::Vec3: {
        Vec3& value = *static_cast<Vec3*>(data);
        const f32 largest = std::max({std::fabs(value.x), std::fabs(value.y), std::fabs(value.z)});
        changed = ImGui::DragFloat3(label, &value.x, dragSpeed(largest, attributes), 0.0f, 0.0f, withUnit("%.3f", attributes).c_str());
        break;
    }
    case Kind::Quat: {
        Quat& q = *static_cast<Quat*>(data);
        if (ImGui::DragFloat4(label, &q.x, 0.005f, -1.0f, 1.0f, "%.3f")) {
            q = normalize(q);
            changed = true;
        }
        break;
    }
    case Kind::String:
        if (attributes && !attributes->pick.empty()) changed = drawPicker(label, *static_cast<std::string*>(data), attributes->pick);
        else changed = inputString(label, *static_cast<std::string*>(data));
        break;
    case Kind::RowRef: changed = drawRowRef(label, v, data, context); break;
    case Kind::Ticks: { // edited in seconds and rounded to ticks
        Ticks& t = *static_cast<Ticks*>(data);
        f32 seconds = t.toSeconds();
        const f32 step = 1.0f / static_cast<f32>(reflect::tickRate());
        const bool ranged = attributes && attributes->hasRange;
        if (ImGui::DragFloat(label, &seconds, step, ranged ? attributes->min : 0.0f, ranged ? attributes->max : 3600.0f, "%.3f s")) {
            const Ticks next = Ticks::seconds(seconds);
            changed = next.value != t.value;
            t = next;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s", trf("%s: %u ticks at %u Hz", name, static_cast<unsigned>(t.value),
                                        static_cast<unsigned>(reflect::tickRate()))
                                        .c_str());
        return changed;
    }
    case Kind::Color: {
        Color& c = *static_cast<Color*>(data);
        f32 rgba[4] = {static_cast<f32>(c.value & 0xFF) / 255.0f, static_cast<f32>((c.value >> 8) & 0xFF) / 255.0f,
                       static_cast<f32>((c.value >> 16) & 0xFF) / 255.0f, static_cast<f32>(c.value >> 24) / 255.0f};
        if (ImGui::ColorEdit4(label, rgba, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) {
            auto byte = [](f32 v) { return static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
            const Color next = Color::rgb(byte(rgba[0]), byte(rgba[1]), byte(rgba[2]), byte(rgba[3]));
            changed = next != c;
            c = next;
        }
        break;
    }
    case Kind::Id: {
        const std::string text = reflect::formatScalar(v, data);
        ImGui::LabelText(label, "%s", text.c_str());
        break;
    }
    case Kind::Struct:
        if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) {
            changed = drawFields(*v.type, data, context);
            ImGui::TreePop();
        }
        break;
    case Kind::Array:
        if (ImGui::TreeNode(label)) {
            for (u32 i = 0; i < v.count; ++i) {
                const std::string item = "[" + std::to_string(i) + "]";
                ImGui::PushID(static_cast<int>(i));
                FieldContext element = context ? *context : FieldContext{};
                if (element.defaults) element.defaults = at(element.defaults, i * v.element->size);
                changed |= drawValue(item.c_str(), name, *v.element, at(data, i * v.element->size), context ? &element : nullptr);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        break;
    case Kind::Container:
        if (ImGui::BeginTable("##pool", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
            drawReadOnlyValue(label, name, v, data);
            ImGui::EndTable();
        }
        break;
    }
    if (v.kind != Kind::Struct && v.kind != Kind::Array && v.kind != Kind::Container) nameTooltip(name);
    return changed;
}

} // namespace

// Input field that edits a std::string directly (grows the string if the buffer is too small)
bool inputString(const char* label, std::string& text) {
    auto resize = [](ImGuiInputTextCallbackData* data) {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            std::string& s = *static_cast<std::string*>(data->UserData);
            s.resize(static_cast<size_t>(data->BufTextLen));
            data->Buf = s.data();
        }
        return 0;
    };
    return ImGui::InputText(label, text.data(), text.capacity() + 1, ImGuiInputTextFlags_CallbackResize, resize, &text);
}

bool drawValueWidget(const char* label, const char* name, const ValueInfo& value, void* data,
                     const FieldContext* context, const FieldLabel* attributes) {
    return drawValue(label, name, value, data, context, nullptr, attributes);
}

FieldLabel splitFieldLabel(const char* label) {
    FieldLabel out;
    const char* bar = strchr(label, '|');
    if (!bar) {
        out.name = label;
        return out;
    }
    out.name.assign(label, bar);
    while (bar) {
        const char* start = bar + 1;
        bar = strchr(start, '|');
        const std::string part = bar ? std::string(start, bar) : std::string(start);
        const size_t equals = part.find('=');
        const std::string key = part.substr(0, equals == std::string::npos ? 0 : equals);
        const std::string value = equals == std::string::npos ? std::string() : part.substr(equals + 1);
        if (key == "group") out.group = value;
        else if (key == "unit") out.unit = value;
        else if (key == "pick") out.pick = value;
        else if (key == "step") out.step = std::strtof(value.c_str(), nullptr);
        else if (key == "range" && value.find("..") != std::string::npos) {
            out.min = std::strtof(value.c_str(), nullptr);
            out.max = std::strtof(value.c_str() + value.find("..") + 2, nullptr);
            out.hasRange = out.max > out.min;
        } else if (out.tooltip.empty()) {
            out.tooltip = part; // anything else is the description, which may contain '='
        }
    }
    return out;
}

void setSoundEventSource(std::function<std::vector<std::string>()> source) { g_soundEvents = std::move(source); }

namespace {

// Text lines of one field (paths equal to name or starting with name. / name[)
reflect::TextFields fieldText(const TypeInfo& type, const void* data, const char* name) {
    reflect::TextFields all, out;
    reflect::toText(type, data, all);
    const std::string n = name;
    for (auto& [path, value] : all)
        if (path == n || path.rfind(n + ".", 0) == 0 || path.rfind(n + "[", 0) == 0) out.emplace_back(path, value);
    return out;
}

struct FieldClipboard {
    const reflect::ValueInfo* value = nullptr; // paste only into fields of the same shape
    std::string name;
    reflect::TextFields lines;
};
FieldClipboard g_clipboard;

} // namespace

bool drawFields(const TypeInfo& type, void* data, const FieldContext* context) {
    bool changed = false;
    bool groupOpen = true;
    for (const FieldInfo& f : type.fields) {
        const FieldLabel label = splitFieldLabel(f.label);
        if (!label.group.empty()) groupOpen = sectionHeader(trData(label.group.c_str()), ImGuiTreeNodeFlags_DefaultOpen);
        if (!groupOpen) continue;
        ImGui::PushID(f.name);
        const bool readOnly = (f.flags & reflect::kFieldReadOnly) != 0;
        ImGui::BeginDisabled(readOnly);
        // For inner structs/arrays, defaults are moved to the same field position too
        FieldContext inner = context ? *context : FieldContext{};
        if (inner.defaults) inner.defaults = at(inner.defaults, f.offset);
        const char* description = label.tooltip.empty() ? nullptr : trData(label.tooltip.c_str());
        changed |= drawValue(trData(label.name.c_str()), f.name, *f.value, at(data, f.offset), context ? &inner : nullptr, description, &label);
        ImGui::EndDisabled();
        if (description && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", description);
        // Dot on the left if different from the default
        const bool hasDefaults = context && context->defaults;
        bool differs = false;
        if (hasDefaults) {
            Hasher a, b;
            reflect::hashValue(*f.value, at(data, f.offset), a);
            reflect::hashValue(*f.value, at(context->defaults, f.offset), b);
            differs = a.value != b.value;
            if (differs) {
                const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(min.x - 5.0f, (min.y + std::min(max.y, min.y + ImGui::GetFrameHeight())) * 0.5f),
                                                            2.5f, ImGui::GetColorU32(ImVec4(0.45f, 0.70f, 1.0f, 1.0f)));
            }
        }
        // Right click: reset to default / copy value / paste
        if (!readOnly && ImGui::BeginPopupContextItem("##fieldMenu")) {
            if (ImGui::MenuItem(tr("Reset to Default"), nullptr, false, hasDefaults && differs)) {
                for (auto& [path, value] : fieldText(type, context->defaults, f.name)) reflect::setFromText(type, data, path, value);
                changed = true;
            }
            if (ImGui::MenuItem(tr("Copy Value"))) {
                g_clipboard = {f.value, f.name, fieldText(type, data, f.name)};
                std::string text;
                for (auto& [path, value] : g_clipboard.lines) text += path + " " + value + "\n";
                ImGui::SetClipboardText(text.c_str());
            }
            if (ImGui::MenuItem(tr("Paste Value"), nullptr, false, g_clipboard.value == f.value)) {
                for (auto& [path, value] : g_clipboard.lines)
                    reflect::setFromText(type, data, f.name + path.substr(g_clipboard.name.size()), value);
                changed = true;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    return changed;
}

std::string propertyRow(const char* label, const char* tooltip) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const f32 font = ImGui::GetFontSize();
    // The value column starts at the same x in every row, including rows indented inside a tree
    const f32 left = ImGui::GetCursorPosX();
    const f32 start = ImGui::GetCursorStartPos().x;
    const f32 region = left - start + ImGui::GetContentRegionAvail().x;
    const f32 valueX = std::max(start + std::floor(std::clamp(region * 0.4f, font * 5.0f, font * 14.0f)), left + font * 3.0f);

    const f32 width = valueX - left - style.ItemInnerSpacing.x;
    const f32 height = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const char* end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 size = ImGui::CalcTextSize(label, end);
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(pos.x, pos.y + style.FramePadding.y), ImVec2(pos.x + width, pos.y + height),
                              pos.x + width, label, end, &size);
    ImGui::Dummy(ImVec2(width, height));
    const bool cut = size.x > width;
    if ((cut || tooltip) && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (cut && tooltip) ImGui::SetTooltip("%.*s\n%s", static_cast<int>(end - label), label, tooltip);
        else if (cut) ImGui::SetTooltip("%.*s", static_cast<int>(end - label), label);
        else ImGui::SetTooltip("%s", tooltip);
    }
    ImGui::SameLine(valueX);
    ImGui::SetNextItemWidth(-FLT_MIN);
    return std::string("##") + label;
}

bool sectionHeader(const char* label, int treeNodeFlags) {
    ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive));
    const bool open = ImGui::CollapsingHeader(label, treeNodeFlags);
    ImGui::PopStyleColor(3);
    return open;
}

void drawFieldsReadOnly(const TypeInfo& type, const void* data) {
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg;
    if (!ImGui::BeginTable("##fields", 2, flags)) return;
    ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch, 0.45f);
    ImGui::TableSetupColumn(tr("Value"), ImGuiTableColumnFlags_WidthStretch, 0.55f);
    drawReadOnlyStruct(type, data);
    ImGui::EndTable();
}

} // namespace mealkit::editor
