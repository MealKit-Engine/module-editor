#include "mealkit/editor/BalancePanel.h"

#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/GameModuleHost.h"
#include "mealkit/editor/SharedBuild.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace mealkit::editor {
namespace {

std::string headlessExe(const ProjectInfo& project) { return project.buildRoot() + "/build/Release/bin/" + project.name() + "_headless.exe"; }

struct Summary {
    f64 mean = 0.0, low = 0.0, high = 0.0;
};
Summary summarize(const std::vector<f64>& v) {
    Summary s;
    if (v.empty()) return s;
    s.low = s.high = v[0];
    for (f64 x : v) {
        s.mean += x;
        s.low = std::min(s.low, x);
        s.high = std::max(s.high, x);
    }
    s.mean /= static_cast<f64>(v.size());
    return s;
}

} // namespace

void BalancePanel::run(const ProjectInfo& project) {
    if (running() || project.directory.empty()) return;
    m_stats.clear();
    m_done = m_failed = 0;
    m_lastError.clear();
    m_status.clear();
    m_output.clear();
    const std::string exe = headlessExe(project);
    const bool compiler = hasCompiler();
    if (fileExists(exe) && (!compiler || !GameModuleHost::olderThanSources(exe, project.directory))) {
        startMatch(project);
    } else if (!compiler) {
        m_status = tr("No test program: ask a programmer to publish a Release build.");
    } else if (m_process.start("\"" + project.buildRoot() + "\\build.bat\"", project.buildRoot())) {
        m_stage = Stage::Building;
        m_status = tr("Building the test program...");
    } else {
        m_status = tr("Could not start the build.");
    }
}

void BalancePanel::startMatch(const ProjectInfo& project) {
    const int sets = m_compare.empty() ? 1 : 2;
    if (m_done >= m_matches * sets) {
        m_stage = Stage::Idle;
        return;
    }
    const int seed = m_done % m_matches + 1;
    const u32 ticks = static_cast<u32>(std::max(1.0f, m_seconds * static_cast<f32>(project.settings.tickRate)));
    std::string exe = headlessExe(project);
    std::replace(exe.begin(), exe.end(), '/', '\\');
    std::string command = "\"" + exe + "\" --seed " + std::to_string(seed) + " --exit-tick " + std::to_string(ticks) + " --final-state";
    if (m_done >= m_matches) command += " --tuning assets\\" + m_compare; // no spaces: cmd.exe keeps one quoted pair only
    m_output.clear();
    if (m_process.start(command, project.directory)) {
        m_stage = Stage::Running;
    } else {
        m_status = tr("Could not start the test program.");
        m_stage = Stage::Idle;
    }
}

void BalancePanel::finishMatch() {
    const int set = m_done >= m_matches ? 1 : 0;
    ++m_done;
    size_t pos = 0;
    bool any = false;
    std::map<std::string, f64> match;
    while (pos < m_output.size()) {
        size_t end = m_output.find('\n', pos);
        if (end == std::string::npos) end = m_output.size();
        const std::string line = m_output.substr(pos, end - pos);
        pos = end + 1;
        if (line.rfind("STATE ", 0) != 0) continue;
        const size_t space = line.find(' ', 6);
        if (space == std::string::npos) continue;
        const f64 value = std::strtod(line.c_str() + space + 1, nullptr);
        if (value >= 4294967295.0) continue; // "none" marks (kNoSlot and the like) aren't numbers
        match[line.substr(6, space - 6)] += value;
        any = true;
    }
    if (!any || m_process.exitCode() != 0) {
        ++m_failed;
        m_lastError = m_output.size() > 400 ? m_output.substr(m_output.size() - 400) : m_output; // the program's own reason
        return;
    }
    for (const auto& [name, value] : match) m_stats[name].values[set].push_back(value);
}

void BalancePanel::poll(const ProjectInfo& project) {
    if (m_stage == Stage::Idle || m_process.poll(m_output)) return;
    if (m_stage == Stage::Building) {
        if (m_process.exitCode() != 0) {
            m_status = tr("The test program didn't build: the game code needs fixing. See the output below.");
            m_stage = Stage::Idle;
            return;
        }
        m_status.clear();
    } else {
        finishMatch();
    }
    startMatch(project);
}

void BalancePanel::drawContent(const ProjectInfo& project) {
    ImGui::BeginDisabled(running());
    ImGui::DragInt(propertyRow(tr("Matches")).c_str(), &m_matches, 1.0f, 1, 500);
    ImGui::DragFloat(propertyRow(tr("Match length (s)")).c_str(), &m_seconds, 5.0f, 5.0f, 3600.0f, "%.0f");
    if (ImGui::BeginCombo(propertyRow(tr("Compare with tuning")).c_str(), m_compare.empty() ? tr("(none)") : m_compare.c_str())) {
        if (ImGui::Selectable(tr("(none)"), m_compare.empty())) m_compare.clear();
        for (const std::string& f : listFiles(project.directory + "/assets", ".tuning"))
            if (f != "view.tuning" && ImGui::Selectable(f.c_str(), f == m_compare)) m_compare = f;
        ImGui::EndCombo();
    }
    if (ImGui::Button(tr("Run Bot Matches"))) run(project);
    ImGui::EndDisabled();
    ImGui::SameLine();
    const int total = m_matches * (m_compare.empty() ? 1 : 2);
    if (m_stage == Stage::Running) ImGui::Text("%d / %d", m_done + 1, total);
    else ImGui::TextDisabled("%s", tr("Uses saved files: save tuning and scenes first."));
    if (!m_status.empty()) ImGui::TextUnformatted(m_status.c_str());
    if (m_failed > 0) ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.38f, 1.0f), "%s", trf("%d match(es) failed", m_failed).c_str());
    if (m_failed > 0 && !m_lastError.empty()) ImGui::TextWrapped("%s", m_lastError.c_str());
    if (m_stage == Stage::Building || (!m_status.empty() && !m_output.empty())) ImGui::TextWrapped("%s", m_output.c_str());
    if (m_stats.empty()) return;

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    inputString("##find", m_filter);
    ImGui::SameLine();
    ImGui::Checkbox(tr("Only values that change"), &m_changedOnly);
    const bool ab = !m_compare.empty();
    if (!ImGui::BeginTable("##balance", ab ? 6 : 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Value"));
    ImGui::TableSetupColumn(tr("Average"));
    ImGui::TableSetupColumn(tr("Lowest"));
    ImGui::TableSetupColumn(tr("Highest"));
    if (ab) {
        ImGui::TableSetupColumn(tr("Compared average"));
        ImGui::TableSetupColumn(tr("Difference"));
    }
    ImGui::TableHeadersRow();
    for (const auto& [name, stats] : m_stats) {
        if (!m_filter.empty() && name.find(m_filter) == std::string::npos) continue;
        const Summary a = summarize(stats.values[0]), b = summarize(stats.values[1]);
        if (m_changedOnly && a.low == a.high && (!ab || a.mean == b.mean)) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(name.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", a.mean);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", a.low);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", a.high);
        if (ab) {
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", b.mean);
            ImGui::TableNextColumn();
            ImGui::Text("%+.2f", b.mean - a.mean);
        }
    }
    ImGui::EndTable();
}

} // namespace mealkit::editor
