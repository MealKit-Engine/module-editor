#include "mealkit/editor/FieldEditor.h"

#include "mealkit/core/Hash.h"

#include "mealkit/editor/Strings.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <imgui.h>

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

// std::string을 직접 고치는 입력 칸 (버퍼가 모자라면 문자열을 늘린다)
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

void nameTooltip(const char* name) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", name);
}

// 살아 있는 슬롯 목록 (Pool 표시용)
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
        drawReadOnlyValue(trData(f.label), f.name, *f.value, at(data, f.offset));
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

// 행 참조: 가리키는 표의 행 id를 고른다 (표를 모르면 글자로)
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

bool drawValue(const char* label, const char* name, const ValueInfo& v, void* data, const FieldContext* context) {
    bool changed = false;
    switch (v.kind) {
    case Kind::Bool: changed = ImGui::Checkbox(label, static_cast<bool*>(data)); break;
    case Kind::Int:
    case Kind::UInt:
        if (!v.enumNames.empty()) { // MEALKIT_ENUM: 이름 고르기
            u64 value = 0;
            std::memcpy(&value, data, v.size);
            const std::string preview = reflect::formatScalar(v, data);
            if (ImGui::BeginCombo(label, preview.c_str())) {
                for (size_t i = 0; i < v.enumNames.size(); ++i)
                    if (ImGui::Selectable(v.enumNames[i].c_str(), value == i)) {
                        const u64 next = i;
                        std::memcpy(data, &next, v.size); // 리틀 엔디언: 아래 바이트부터
                        changed = true;
                    }
                ImGui::EndCombo();
            }
        } else {
            changed = ImGui::DragScalar(label, dataType(v), data, 0.2f);
        }
        break;
    case Kind::Float: changed = ImGui::DragFloat(label, static_cast<f32*>(data), 0.01f, 0.0f, 0.0f, "%.3f"); break;
    case Kind::Vec3: changed = ImGui::DragFloat3(label, &static_cast<Vec3*>(data)->x, 0.01f, 0.0f, 0.0f, "%.3f"); break;
    case Kind::Quat: {
        Quat& q = *static_cast<Quat*>(data);
        if (ImGui::DragFloat4(label, &q.x, 0.005f, -1.0f, 1.0f, "%.3f")) {
            q = normalize(q);
            changed = true;
        }
        break;
    }
    case Kind::String: changed = inputString(label, *static_cast<std::string*>(data)); break;
    case Kind::RowRef: changed = drawRowRef(label, v, data, context); break;
    case Kind::Ticks: { // 초로 고치고 틱으로 반올림한다
        Ticks& t = *static_cast<Ticks*>(data);
        f32 seconds = t.toSeconds();
        const f32 step = 1.0f / static_cast<f32>(reflect::tickRate());
        if (ImGui::DragFloat(label, &seconds, step, 0.0f, 3600.0f, "%.3f s")) {
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
                changed |= drawValue(item.c_str(), name, *v.element, at(data, i * v.element->size), context);
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

bool drawValueWidget(const char* label, const char* name, const ValueInfo& value, void* data,
                     const FieldContext* context) {
    return drawValue(label, name, value, data, context);
}

FieldLabel splitFieldLabel(const char* label) {
    const char* bar = strchr(label, '|');
    if (!bar) return {label, {}};
    return {std::string(label, bar), bar + 1};
}

namespace {

// 필드 하나의 텍스트 줄들 (경로가 name이거나 name. / name[ 로 시작)
reflect::TextFields fieldText(const TypeInfo& type, const void* data, const char* name) {
    reflect::TextFields all, out;
    reflect::toText(type, data, all);
    const std::string n = name;
    for (auto& [path, value] : all)
        if (path == n || path.rfind(n + ".", 0) == 0 || path.rfind(n + "[", 0) == 0) out.emplace_back(path, value);
    return out;
}

struct FieldClipboard {
    const reflect::ValueInfo* value = nullptr; // 같은 모양의 필드에만 붙여 넣는다
    std::string name;
    reflect::TextFields lines;
};
FieldClipboard g_clipboard;

} // namespace

bool drawFields(const TypeInfo& type, void* data, const FieldContext* context) {
    bool changed = false;
    for (const FieldInfo& f : type.fields) {
        ImGui::PushID(f.name);
        const bool readOnly = (f.flags & reflect::kFieldReadOnly) != 0;
        ImGui::BeginDisabled(readOnly);
        const FieldLabel label = splitFieldLabel(f.label);
        changed |= drawValue(trData(label.name.c_str()), f.name, *f.value, at(data, f.offset), context);
        ImGui::EndDisabled();
        if (!label.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", trData(label.tooltip.c_str()));
        // 기본값과 다르면 왼쪽에 점
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
        // 오른쪽 클릭: 기본값으로 / 값 복사 / 붙여넣기
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

void drawFieldsReadOnly(const TypeInfo& type, const void* data) {
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg;
    if (!ImGui::BeginTable("##fields", 2, flags)) return;
    ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch, 0.45f);
    ImGui::TableSetupColumn(tr("Value"), ImGuiTableColumnFlags_WidthStretch, 0.55f);
    drawReadOnlyStruct(type, data);
    ImGui::EndTable();
}

} // namespace mealkit::editor
