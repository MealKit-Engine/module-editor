#include "mealkit/editor/LogPanel.h"

#include "mealkit/editor/CodeLink.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Strings.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace mealkit::editor {
namespace {

constexpr size_t kMaxEntries = 5000;
LogPanel* g_panel = nullptr; // the log sink is a function pointer, so one window is held globally

const ImVec4 kColors[3] = {ImVec4(0.80f, 0.80f, 0.80f, 1.0f), ImVec4(0.95f, 0.80f, 0.35f, 1.0f),
                           ImVec4(1.0f, 0.45f, 0.40f, 1.0f)};

bool contains(const std::string& text, const char* needle) {
    if (!needle[0]) return true;
    const size_t n = strlen(needle);
    for (size_t i = 0; i + n <= text.size(); ++i)
        if (_strnicmp(text.data() + i, needle, n) == 0) return true;
    return false;
}

} // namespace

LogPanel::LogPanel() {
    g_panel = this;
    log::setSink(&LogPanel::sink);
}

LogPanel::~LogPanel() {
    log::setSink(nullptr);
    g_panel = nullptr;
}

void LogPanel::sink(log::Level level, double seconds, const char* message) {
    LogPanel* p = g_panel;
    if (!p) return;
    std::lock_guard<std::mutex> guard(p->m_lock);
    if (p->m_entries.size() >= kMaxEntries) {
        --p->m_counts[static_cast<int>(p->m_entries.front().level)];
        p->m_entries.pop_front();
    }
    p->m_entries.push_back({level, seconds, message});
    ++p->m_counts[static_cast<int>(level)];
    p->m_added = true;
}

void LogPanel::drawFilters() {
    std::lock_guard<std::mutex> guard(m_lock);
    // Level filters: icon and count, filled when shown
    const char* names[3] = {tr("Info"), tr("Warnings"), tr("Errors")};
    const char* glyphs[3] = {icon::kInfo, icon::kWarning, icon::kError};
    for (int i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine(0.0f, 2.0f);
        ImVec4 text = kColors[i];
        if (!m_show[i]) text.w = 0.45f;
        ImGui::PushStyleColor(ImGuiCol_Text, text);
        ImGui::PushStyleColor(ImGuiCol_Button, m_show[i] ? ImGui::GetStyleColorVec4(ImGuiCol_FrameBg) : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        char label[96];
        snprintf(label, sizeof(label), "%s  %u###level%d", glyphs[i], m_counts[i], i);
        if (ImGui::Button(label)) m_show[i] = !m_show[i];
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", names[i]);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x));
    ImGui::InputTextWithHint("##search", (std::string(icon::kSearch) + "  " + tr("Search")).c_str(), m_search, sizeof(m_search));
    ImGui::SameLine();
    if (iconButton(icon::kTrash, tr("Clear"))) {
        m_entries.clear();
        for (u32& c : m_counts) c = 0;
    }
}

void LogPanel::drawLines() {
    std::lock_guard<std::mutex> guard(m_lock);
    std::vector<u32> rows;
    rows.reserve(m_entries.size());
    for (u32 i = 0; i < m_entries.size(); ++i) {
        const Entry& e = m_entries[i];
        if (m_show[static_cast<int>(e.level)] && contains(e.message, m_search)) rows.push_back(i);
    }
    if (ImGui::BeginChild("##lines", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const Entry& e = m_entries[rows[r]];
                ImGui::TextDisabled("%8.3f", e.seconds);
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, kColors[static_cast<int>(e.level)]);
                ImGui::TextUnformatted(e.message.c_str());
                ImGui::PopStyleColor();
                FileLine at;
                if (ImGui::IsItemHovered() && parseFileLine(e.message, &at)) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (ImGui::IsItemClicked()) openInCodeEditor(m_codeEditor, at.file, at.line);
                }
            }
        }
        if (m_added && m_follow) ImGui::SetScrollHereY(1.0f);
        m_follow = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
        m_added = false;
    }
    ImGui::EndChild();
}

} // namespace mealkit::editor
