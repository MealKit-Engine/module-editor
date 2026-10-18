#include "mealkit/editor/FieldEditor.h"

#include "mealkit/editor/Strings.h"

#include <algorithm>
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

bool drawValue(const char* label, const char* name, const ValueInfo& v, void* data) {
    bool changed = false;
    switch (v.kind) {
    case Kind::Bool: changed = ImGui::Checkbox(label, static_cast<bool*>(data)); break;
    case Kind::Int:
    case Kind::UInt: changed = ImGui::DragScalar(label, dataType(v), data, 0.2f); break;
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
            changed = drawFields(*v.type, data);
            ImGui::TreePop();
        }
        break;
    case Kind::Array:
        if (ImGui::TreeNode(label)) {
            for (u32 i = 0; i < v.count; ++i) {
                const std::string item = "[" + std::to_string(i) + "]";
                ImGui::PushID(static_cast<int>(i));
                changed |= drawValue(item.c_str(), name, *v.element, at(data, i * v.element->size));
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

bool drawFields(const TypeInfo& type, void* data) {
    bool changed = false;
    for (const FieldInfo& f : type.fields) {
        ImGui::PushID(f.name);
        const bool readOnly = (f.flags & reflect::kFieldReadOnly) != 0;
        ImGui::BeginDisabled(readOnly);
        changed |= drawValue(trData(f.label), f.name, *f.value, at(data, f.offset));
        ImGui::EndDisabled();
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
