// Checkbox and radio button of the suite (docs/TOOL_DESIGN.md 3.2): an outlined empty box, an accent fill with a light mark when on.
// Same drawing as MkRender, MkAudio, MkUI, QA and the Manager (each copies it, DECISIONS 2026-11-12).
#include "mealkit/editor/ImGuiLayer.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace mealkit::editor {
namespace {

ImVec4 rgb(u32 c) { return {((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, 1.0f}; }

const ImVec4 kControlOutline = rgb(0x8A8A8A);      // gray 9
const ImVec4 kControlOutlineHover = rgb(0xAFAFAF); // gray 11
const ImVec4 kControlMark = rgb(0xEBEBEB);         // gray 12
const ImVec4 kAccent = rgb(0x3B63FB);
const ImVec4 kAccentHover = rgb(0x4B75FF);
const ImVec4 kAccentDown = rgb(0x274DEA);

// Fill and mark of the control in its state; popControlColors after
void pushControlColors(bool on) {
    const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
    const ImGuiStyle& s = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, on ? kAccent : clear);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, on ? kAccentHover : s.Colors[ImGuiCol_FrameBg]);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, on ? kAccentDown : s.Colors[ImGuiCol_FrameBgHovered]);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, kControlMark);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, s.FrameRounding * 0.5f); // a 4 px box, not the round 8 px of fields
}

void popControlColors() {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

f32 outlineWidth() { return std::max(1.0f, ImGui::GetStyle().FontScaleDpi); }
ImU32 outlineColor() { return ImGui::GetColorU32(ImGui::IsItemHovered() ? kControlOutlineHover : kControlOutline); }

} // namespace

bool checkbox(const char* label, bool* v) {
    const bool on = *v;
    pushControlColors(on);
    const bool changed = ImGui::Checkbox(label, v);
    const f32 rounding = ImGui::GetStyle().FrameRounding;
    popControlColors();
    if (!on) {
        // The box is the square at the start of the item (ImGui::Checkbox check_bb)
        const ImVec2 p = ImGui::GetItemRectMin();
        const f32 size = ImGui::GetFrameHeight();
        const f32 width = outlineWidth();
        const f32 inset = width * 0.5f;
        ImGui::GetWindowDrawList()->AddRect({p.x + inset, p.y + inset}, {p.x + size - inset, p.y + size - inset}, outlineColor(), rounding, 0, width);
    }
    return changed;
}

bool radioButton(const char* label, bool active) {
    pushControlColors(active);
    const bool pressed = ImGui::RadioButton(label, active);
    popControlColors();
    if (!active) {
        // Same circle as ImGui::RadioButton: radius (square - 1) / 2 around the square's center
        const ImVec2 p = ImGui::GetItemRectMin();
        const f32 size = ImGui::GetFrameHeight();
        const f32 width = outlineWidth();
        const ImVec2 center(std::floor(p.x + size * 0.5f + 0.5f), std::floor(p.y + size * 0.5f + 0.5f));
        ImGui::GetWindowDrawList()->AddCircle(center, (size - 1.0f) * 0.5f - width * 0.5f, outlineColor(), 0, width);
    }
    return pressed;
}

} // namespace mealkit::editor
