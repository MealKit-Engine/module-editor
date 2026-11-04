#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/EditorColors.h"

#include "mealkit/core/Hash.h"

#include "mealkit/editor/ImGuiLayer.h"
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

constexpr f32 kIconScale = 0.82f; // of the font size: the icon fonts fill their whole em box

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
std::function<void(const std::string&, const std::string&)> g_pickPreview;

bool containsText(const std::string& text, const char* needle) {
    const auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    const std::string n = needle;
    return std::search(text.begin(), text.end(), n.begin(), n.end(), [&](char a, char b) { return lower(static_cast<unsigned char>(a)) == lower(static_cast<unsigned char>(b)); }) != text.end();
}

// showIf text against the type's other fields
bool showIfPasses(const std::string& condition, const TypeInfo& type, const void* data) {
    const size_t op = condition.find_first_of("><=!");
    const std::string name = condition.substr(0, op);
    for (const FieldInfo& f : type.fields) {
        if (name != f.name) continue;
        const void* value = at(data, f.offset);
        const std::string text = f.value->kind == Kind::String ? *static_cast<const std::string*>(value) : reflect::formatScalar(*f.value, value);
        if (op == std::string::npos) {
            if (text.empty() || text == "false") return false;
            char* end = nullptr;
            const f32 number = std::strtof(text.c_str(), &end);
            return *end != 0 || number != 0.0f; // text that isn't a number counts as set
        }
        const f32 left = std::strtof(text.c_str(), nullptr), right = std::strtof(condition.c_str() + op + 1, nullptr);
        switch (condition[op]) {
        case '>': return left > right;
        case '<': return left < right;
        case '=': return left == right;
        default: return left != right;
        }
    }
    return true; // unknown name: don't hide
}

bool fieldMatches(const FieldInfo& f, const char* filter) {
    if (containsText(f.name, filter) || containsText(f.label, filter) || containsText(trData(splitFieldLabel(f.label).name.c_str()), filter)) return true;
    if (f.value->kind == Kind::Struct)
        for (const FieldInfo& inner : f.value->type->fields)
            if (fieldMatches(inner, filter)) return true;
    return false;
}

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
    bool dropped = false;
    if (g_pickPreview) ImGui::SetNextItemWidth(-(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x));
    const bool found = value.empty() || std::find(names.begin(), names.end(), value) != names.end();
    if (!found) ImGui::PushStyleColor(ImGuiCol_Text, color::kDanger);
    const std::string preview = value.empty() ? std::string(tr("(none)")) : value + (found ? "" : " (?)");
    const bool open = beginCombo(label, preview.c_str());
    if (!found) ImGui::PopStyleColor();
    if (ImGui::BeginDragDropTarget()) { // a file dragged from [Assets], when it is in this field's folder
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MK_ASSET")) {
            const std::string file = static_cast<const char*>(payload->Data);
            const size_t colon = pick.find(':');
            const std::string lead = pick.substr(0, colon) + "/", extension = colon == std::string::npos ? std::string() : pick.substr(colon + 1);
            if (colon != std::string::npos && file.rfind(lead, 0) == 0 && file.size() > extension.size() && file.compare(file.size() - extension.size(), extension.size(), extension) == 0) {
                value = file.substr(lead.size(), file.size() - lead.size() - extension.size());
                dropped = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    const auto previewButton = [&] {
        if (!g_pickPreview) return;
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::BeginDisabled(value.empty());
        if (ImGui::ArrowButton("##pickPreview", ImGuiDir_Right)) g_pickPreview(pick, value);
        ImGui::EndDisabled();
    };
    if (!open) {
        previewButton();
        return dropped;
    }
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
    previewButton();
    return changed || dropped;
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
    if (!found) ImGui::PushStyleColor(ImGuiCol_Text, color::kDanger);
    const std::string preview = ref.id->empty() ? std::string(tr("(none)")) : *ref.id + (found ? "" : " (?)");
    bool changed = false;
    if (beginCombo(label, preview.c_str())) {
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
            if (beginCombo(label, preview.c_str())) {
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
        if (colorEdit(label, rgba, true)) {
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

void swapElements(const ValueInfo& v, void* a, void* b) {
    if (v.kind == Kind::Struct && v.type && v.type->copy) {
        reflect::Instance temp(*v.type);
        v.type->copy(temp.data(), a);
        v.type->copy(a, b);
        v.type->copy(b, temp.data());
    } else if (v.kind == Kind::String) {
        std::swap(*static_cast<std::string*>(a), *static_cast<std::string*>(b));
    } else {
        std::vector<u8> temp(v.size);
        memcpy(temp.data(), a, v.size);
        memcpy(a, b, v.size);
        memcpy(b, temp.data(), v.size);
    }
}

// MEALKIT_LIST: a count field + an array. Only the used part shows, with add, remove and reorder
bool drawList(const char* label, const char* name, const ValueInfo& v, void* data, u32& count, const FieldContext* context) {
    bool changed = false;
    count = std::min(count, v.count);
    const std::string title = std::string(label) + " (" + std::to_string(count) + "/" + std::to_string(v.count) + ")###list";
    if (!ImGui::TreeNode(title.c_str())) return false;
    int remove = -1, up = -1;
    const u32 size = v.element->size;
    for (u32 i = 0; i < count; ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton("x")) remove = static_cast<int>(i);
        ImGui::SameLine();
        ImGui::BeginDisabled(i == 0);
        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) up = static_cast<int>(i);
        ImGui::EndDisabled();
        ImGui::SameLine();
        FieldContext element = context ? *context : FieldContext{};
        if (element.defaults) element.defaults = at(element.defaults, i * size);
        const std::string item = "[" + std::to_string(i) + "]";
        ImGui::BeginGroup();
        changed |= drawValue(item.c_str(), name, *v.element, at(data, i * size), context ? &element : nullptr);
        ImGui::EndGroup();
        ImGui::PopID();
    }
    if (up > 0) {
        swapElements(*v.element, at(data, static_cast<u32>(up) * size), at(data, static_cast<u32>(up - 1) * size));
        changed = true;
    }
    if (remove >= 0) {
        for (u32 i = static_cast<u32>(remove); i + 1 < count; ++i) swapElements(*v.element, at(data, i * size), at(data, (i + 1) * size));
        --count;
        changed = true;
    }
    ImGui::BeginDisabled(count >= v.count);
    if (ImGui::SmallButton(tr("+ Add Item"))) {
        ++count; // keeps what was in the slot; a struct starts from its defaults
        if (v.element->kind == Kind::Struct && v.element->type && v.element->type->copy) {
            reflect::Instance fresh(*v.element->type);
            v.element->type->copy(at(data, (count - 1) * size), fresh.data());
        }
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::TreePop();
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
        else if (key == "gizmo") out.gizmo = value;
        else if (key == "showIf") out.showIf = value;
        else if (part == "required") out.required = true;
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
void setPickPreview(std::function<void(const std::string&, const std::string&)> preview) { g_pickPreview = std::move(preview); }

void collectRequired(const TypeInfo& type, const void* data, const std::string& where, std::vector<std::string>& out) {
    for (const FieldInfo& f : type.fields) {
        const FieldLabel label = splitFieldLabel(f.label);
        if (!label.required || f.value->kind != Kind::String) continue;
        if (!label.showIf.empty() && !showIfPasses(label.showIf, type, data)) continue;
        if (static_cast<const std::string*>(static_cast<const void*>(at(data, f.offset)))->empty())
            out.push_back(where + trf("%s is required", trData(label.name.c_str())));
    }
}

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
        // MEALKIT_LIST: its count field is edited through the list below (Add, x)
        const FieldInfo* next = &f + 1 != type.fields.data() + type.fields.size() ? &f + 1 : nullptr;
        if (next && next->value->kind == Kind::Array && f.value->kind == Kind::UInt && f.value->size == sizeof(u32) && std::string(next->name) + "Count" == f.name) continue;
        const FieldInfo* countField = &f != type.fields.data() ? &f - 1 : nullptr;
        const bool isList = countField && f.value->kind == Kind::Array && countField->value->kind == Kind::UInt && countField->value->size == sizeof(u32) && std::string(f.name) + "Count" == countField->name;
        const FieldLabel label = splitFieldLabel(f.label);
        const bool searching = context && ((context->filter && context->filter[0]) || context->onlyChanged);
        if (!searching && !label.group.empty()) groupOpen = sectionHeader(trData(label.group.c_str()), ImGuiTreeNodeFlags_DefaultOpen);
        if (!searching && !groupOpen) continue;
        if (!label.showIf.empty() && !showIfPasses(label.showIf, type, data)) continue;
        if (context && context->filter && context->filter[0] && !fieldMatches(f, context->filter)) continue;
        if (context && context->onlyChanged && context->defaults) {
            Hasher a, b;
            reflect::hashValue(*f.value, at(data, f.offset), a);
            reflect::hashValue(*f.value, at(context->defaults, f.offset), b);
            if (a.value == b.value) continue;
        }
        ImGui::PushID(f.name);
        const bool readOnly = (f.flags & reflect::kFieldReadOnly) != 0;
        ImGui::BeginDisabled(readOnly);
        // For inner structs/arrays, defaults are moved to the same field position too
        FieldContext inner = context ? *context : FieldContext{};
        if (inner.defaults) inner.defaults = at(inner.defaults, f.offset);
        inner.mixed = nullptr; // inner paths are marked by the top-level field
        inner.filter = nullptr; // a matching struct shows all of its fields
        inner.onlyChanged = false;
        const char* description = label.tooltip.empty() ? nullptr : trData(label.tooltip.c_str());
        const bool mixedValue = context && context->mixed && context->mixed->count(f.name) != 0;
        const std::string shown = std::string(trData(label.name.c_str())) + (mixedValue ? " \xE2\x80\x94" : "");
        const bool missing = label.required && f.value->kind == Kind::String && static_cast<const std::string*>(static_cast<const void*>(at(data, f.offset)))->empty();
        if (mixedValue) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.30f, 0.26f, 0.16f, 1.0f));
        if (missing) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.40f, 0.16f, 0.15f, 1.0f));
        if (isList) changed |= drawList(shown.c_str(), f.name, *f.value, at(data, f.offset), *static_cast<u32*>(static_cast<void*>(at(data, countField->offset))), context ? &inner : nullptr);
        else changed |= drawValue(shown.c_str(), f.name, *f.value, at(data, f.offset), context ? &inner : nullptr, description, &label);
        if (missing) ImGui::PopStyleColor();
        if (mixedValue) ImGui::PopStyleColor();
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
                                                            2.5f, ImGui::GetColorU32(color::kChanged));
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
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y));
    const ImGuiID id = ImGui::GetID(label);
    bool open = ImGui::TreeNodeUpdateNextOpen(id, treeNodeFlags);
    const f32 font = ImGui::GetFontSize();
    const f32 height = font + style.FramePadding.y * 2.0f;
    if (ImGui::InvisibleButton(label, ImVec2(std::max(ImGui::GetContentRegionAvail().x, 1.0f), height))) {
        open = !open;
        ImGui::TreeNodeSetOpen(id, open);
    }
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered()) draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_HeaderHovered), style.FrameRounding);
    const f32 glyph = std::floor(font * 0.62f);
    drawGlyph(draw, open ? icon::kChevronDown : icon::kChevronRight, ImVec2(min.x + style.ItemInnerSpacing.x * 0.5f, min.y),
              ImVec2(min.x + style.ItemInnerSpacing.x * 0.5f + glyph, max.y), glyph, ImGui::GetColorU32(ImGuiCol_TextDisabled));
    draw->AddText(ImVec2(min.x + style.ItemInnerSpacing.x * 0.5f + glyph + style.ItemInnerSpacing.x, min.y + style.FramePadding.y),
                  ImGui::GetColorU32(ImGuiCol_Text, 0.72f), label, ImGui::FindRenderedTextEnd(label));
    return open;
}

bool beginCombo(const char* label, const char* preview, int comboFlags) {
    const ImGuiStyle& style = ImGui::GetStyle();
    // Taken before BeginCombo: when open, it switches to the popup window
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const f32 width = ImGui::CalcItemWidth(), height = ImGui::GetFrameHeight();
    const ImVec4 frame = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    const bool open = ImGui::BeginCombo(label, preview, comboFlags | ImGuiComboFlags_NoArrowButton);
    const f32 glyph = std::floor(ImGui::GetFontSize() * 0.62f);
    const f32 glyphX = pos.x + width - style.FramePadding.x - glyph;
    if (frame.w > 0.0f) { // long previews end before the chevron
        const ImVec2 cover(glyphX - style.ItemInnerSpacing.x, pos.y), end(pos.x + width, pos.y + height);
        draw->AddRectFilled(cover, end, ImGui::GetColorU32(ImGui::IsMouseHoveringRect(ImVec2(pos.x, pos.y), end) ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg),
                            style.FrameRounding, ImDrawFlags_RoundCornersRight);
    }
    drawGlyph(draw, icon::kChevronDown, ImVec2(glyphX, pos.y), ImVec2(glyphX + glyph, pos.y + height), glyph, ImGui::GetColorU32(ImGuiCol_TextDisabled));
    return open;
}

namespace {

// The button behind an icon: its rect is left as the last item. Clear until hovered, or filled with the accent when active
bool iconButtonFrame(const char* tooltip, bool active) {
    ImGui::PushID(tooltip);
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
    }
    const f32 side = ImGui::GetFrameHeight();
    const bool pressed = ImGui::Button("##icon", ImVec2(side, side));
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return pressed;
}

} // namespace

void drawGlyph(ImDrawList* draw, const char* glyph, ImVec2 min, ImVec2 max, f32 size, unsigned int color) {
    ImFont* font = ImGui::GetFont();
    unsigned int codepoint = 0;
    ImTextCharFromUtf8(&codepoint, glyph, nullptr);
    const ImFontGlyph* g = font->GetFontBaked(size)->FindGlyph(static_cast<ImWchar>(codepoint));
    const ImVec2 at(std::round((min.x + max.x - g->X0 - g->X1) * 0.5f), std::round((min.y + max.y - g->Y0 - g->Y1) * 0.5f));
    draw->AddText(font, size, at, color, glyph);
}

bool iconButton(const char* glyph, const char* tooltip, bool active) {
    const bool pressed = iconButtonFrame(tooltip, active);
    drawGlyph(ImGui::GetWindowDrawList(), glyph, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetFontSize() * kIconScale, ImGui::GetColorU32(ImGuiCol_Text));
    return pressed;
}

// The icon fonts have no magnet: a horseshoe outline open at the top, with its tips marked off
bool magnetButton(const char* tooltip, bool active) {
    const bool pressed = iconButtonFrame(tooltip, active);
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const f32 size = std::floor((max.y - min.y) * 0.52f);
    const f32 outer = size * 0.5f, inner = size * 0.2f, tip = size * 0.22f;
    const ImVec2 center((min.x + max.x) * 0.5f, std::floor((min.y + max.y) * 0.5f + size * 0.02f));
    const f32 top = center.y - size * 0.5f, thickness = std::max(1.0f, size * 0.085f);
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PathLineTo(ImVec2(center.x - outer, top));
    draw->PathArcTo(center, outer, IM_PI, 0.0f, 16); // through the bottom: y points down
    draw->PathLineTo(ImVec2(center.x + outer, top));
    draw->PathLineTo(ImVec2(center.x + inner, top));
    draw->PathArcTo(center, inner, 0.0f, IM_PI, 10);
    draw->PathLineTo(ImVec2(center.x - inner, top));
    draw->PathStroke(color, ImDrawFlags_Closed, thickness);
    draw->AddLine(ImVec2(center.x - outer, top + tip), ImVec2(center.x - inner, top + tip), color, thickness);
    draw->AddLine(ImVec2(center.x + inner, top + tip), ImVec2(center.x + outer, top + tip), color, thickness);
    return pressed;
}

f32& colorPickerSize() {
    static f32 size = 10.0f;
    return size;
}

// A square swatch. It opens a small picker with RGB, HSV and hex rows, which moves by the handle on top and resizes by its corner
bool colorEdit(const char* label, f32* color, bool alpha, int flags) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const f32 font = ImGui::GetFontSize(), side = ImGui::GetFrameHeight();
    const int base = flags | (alpha ? ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf : ImGuiColorEditFlags_NoAlpha);
    const auto edit = [&](const char* id, int extra) {
        return alpha ? ImGui::ColorEdit4(id, color, base | extra) : ImGui::ColorEdit3(id, color, base | extra);
    };
    ImGui::PushID(label);
    bool changed = false;
    const ImVec4 shown(color[0], color[1], color[2], alpha ? color[3] : 1.0f);
    // A faint border, so even a dark color doesn't read as an empty checkbox
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.2f));
    const bool swatchPressed = ImGui::ColorButton("##swatch", shown, (flags & ImGuiColorEditFlags_HDR) | (alpha ? ImGuiColorEditFlags_AlphaPreviewHalf : ImGuiColorEditFlags_NoAlpha), ImVec2(side, side));
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (swatchPressed) ImGui::OpenPopup("##colorPopup");
    if (const char* end = ImGui::FindRenderedTextEnd(label); end != label) { // a label not given through propertyRow
        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        ImGui::TextUnformatted(label, end);
    }
    if (ImGui::BeginPopup("##colorPopup")) {
        const f32 total = std::floor(colorPickerSize() * font);
        // Move handle: a press on a Dummy reaches the window, which starts moving
        ImGui::Dummy(ImVec2(total, std::floor(font * 0.6f)));
        const bool handleHot = ImGui::IsItemHovered();
        if (handleHot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        {
            const ImVec2 handleMin = ImGui::GetItemRectMin(), handleMax = ImGui::GetItemRectMax();
            const f32 middleX = (handleMin.x + handleMax.x) * 0.5f, middleY = (handleMin.y + handleMax.y) * 0.5f, halfWidth = font * 1.2f;
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(middleX - halfWidth, middleY - 1.5f), ImVec2(middleX + halfWidth, middleY + 1.5f),
                                                      ImGui::GetColorU32(handleHot ? ImGuiCol_Text : ImGuiCol_TextDisabled), 1.5f);
        }
        ImGui::SetNextItemWidth(total);
        const int picker = base | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview;
        changed |= alpha ? ImGui::ColorPicker4("##picker", color, picker) : ImGui::ColorPicker3("##picker", color, picker);
        struct Row {
            const char* name;
            int display;
        };
        const f32 nameWidth = font * 2.4f;
        for (const Row& row : {Row{"RGB", ImGuiColorEditFlags_DisplayRGB}, Row{"HSV", ImGuiColorEditFlags_DisplayHSV}, Row{"Hex", ImGuiColorEditFlags_DisplayHex}}) {
            if (row.display == ImGuiColorEditFlags_DisplayHex && (flags & ImGuiColorEditFlags_HDR)) continue; // can't hold values over 1
            ImGui::PushID(row.name);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", row.name);
            ImGui::SameLine(nameWidth + style.WindowPadding.x);
            ImGui::SetNextItemWidth(total - nameWidth);
            changed |= edit("##value", row.display | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoOptions | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoSmallPreview);
            ImGui::PopID();
        }
        // Resize corner: outside the layout, so it can't feed back into the popup's auto size
        // Growing near a screen edge pushes the window inward instead of out; it never grows past the screen height
        const ImGuiViewport* screen = ImGui::GetMainViewport();
        ImVec2 windowPos = ImGui::GetWindowPos();
        const ImVec2 windowSize = ImGui::GetWindowSize();
        const ImVec2 clamped(std::max(std::min(windowPos.x, screen->WorkPos.x + screen->WorkSize.x - windowSize.x), screen->WorkPos.x),
                             std::max(std::min(windowPos.y, screen->WorkPos.y + screen->WorkSize.y - windowSize.y), screen->WorkPos.y));
        if (clamped.x != windowPos.x || clamped.y != windowPos.y) {
            ImGui::SetWindowPos(clamped);
            windowPos = clamped;
        }
        const f32 largest = std::clamp((std::min(screen->WorkSize.x, screen->WorkSize.y - (windowSize.y - total)) - style.WindowPadding.x * 2.0f) / font, 9.0f, 24.0f);
        const ImVec2 corner(windowPos.x + windowSize.x, windowPos.y + windowSize.y);
        const f32 grip = std::floor(font * 0.9f);
        const ImRect gripRect(ImVec2(corner.x - grip, corner.y - grip), corner);
        // Followed by hand, from the press until the release. It holds the hovered and active ids so the press doesn't start moving the window
        static bool held = false;
        const ImGuiID gripId = ImGui::GetID("##resize");
        ImGui::KeepAliveID(gripId);
        const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(gripRect.Min, gripRect.Max, false);
        if (hovered) ImGui::SetHoveredID(gripId);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            held = true;
            ImGui::SetActiveID(gripId, ImGui::GetCurrentContext()->CurrentWindow); // not GetCurrentWindow(): inline, it reads DLL data
        }
        if (held && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            held = false;
            ImGui::ClearActiveID();
        }
        ImGui::PushClipRect(gripRect.Min, gripRect.Max, false); // the window's own clip rect stops short of its corner
        if (hovered || held) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
        if (held) {
            const ImVec2 delta = ImGui::GetIO().MouseDelta;
            colorPickerSize() = std::clamp(colorPickerSize() + (delta.x + delta.y) * 0.5f / font, 9.0f, largest);
        }
        const ImU32 gripColor = ImGui::GetColorU32(held || hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        for (f32 inset : {3.0f, 7.0f})
            draw->AddLine(ImVec2(corner.x - inset - 1.0f, corner.y - 3.0f), ImVec2(corner.x - 3.0f, corner.y - inset - 1.0f), gripColor);
        ImGui::PopClipRect();
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

bool beginListPane(const char* id, f32 width) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetFontSize() * 0.4f, 0.0f));
    const bool open = ImGui::BeginChild(id, ImVec2(width, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    return open;
}

std::string iconTitle(const char* glyph, const char* label) { return std::string(glyph) + "  " + trl(label); }

void drawFieldsReadOnly(const TypeInfo& type, const void* data) {
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg;
    if (!ImGui::BeginTable("##fields", 2, flags)) return;
    ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch, 0.45f);
    ImGui::TableSetupColumn(tr("Value"), ImGuiTableColumnFlags_WidthStretch, 0.55f);
    drawReadOnlyStruct(type, data);
    ImGui::EndTable();
}

} // namespace mealkit::editor
