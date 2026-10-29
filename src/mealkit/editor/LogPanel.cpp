#include "mealkit/editor/LogPanel.h"

#include "mealkit/editor/CodeLink.h"
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

void LogPanel::draw() {
    if (!m_open) return;
    ImGui::SetNextWindowSize(ImVec2(720, 300), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(trl(kWindow), &m_open)) {
        ImGui::End();
        return;
    }
    std::lock_guard<std::mutex> guard(m_lock);
    if (ImGui::Button(tr("Clear"))) {
        m_entries.clear();
        for (u32& c : m_counts) c = 0;
    }
    const char* names[3] = {tr("Info"), tr("Warnings"), tr("Errors")};
    for (int i = 0; i < 3; ++i) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kColors[i]);
        char label[96];
        snprintf(label, sizeof(label), "%s (%u)###level%d", names[i], m_counts[i], i);
        ImGui::Checkbox(label, &m_show[i]);
        ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search", tr("Search"), m_search, sizeof(m_search));
    ImGui::Separator();

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
    ImGui::End();
}

} // namespace mealkit::editor
