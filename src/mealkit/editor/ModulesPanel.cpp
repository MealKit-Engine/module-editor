#include "mealkit/editor/ModulesPanel.h"

#include "mealkit/core/Log.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace mealkit::editor {
namespace {

bool containsNoCase(const std::string& text, const char* needle) {
    if (!needle[0]) return true;
    const std::string n(needle);
    return std::search(text.begin(), text.end(), n.begin(), n.end(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           }) != text.end();
}

std::string quoted(const std::string& s) { return "\"" + s + "\""; }

const ImVec4 kWarn(1.0f, 0.8f, 0.3f, 1.0f);
const ImVec4 kError(1.0f, 0.45f, 0.4f, 1.0f);

} // namespace

void ModulesPanel::run(const std::string& engineDirectory, const ProjectInfo& project, const std::string& label,
                       const std::string& arguments, bool changes) {
    if (m_process.running()) return;
    const std::string script = engineDirectory + "\\tools\\module.ps1";
    if (!fileExists(script)) {
        m_status = trf("Module tool not found: %s", script.c_str());
        return;
    }
    const std::string command = "powershell -NoProfile -ExecutionPolicy Bypass -File " + quoted(script) + " " + arguments +
                                " -Project " + quoted(project.directory);
    m_output.clear();
    m_label = label;
    m_listing = !changes;
    m_changes = changes;
    if (!m_process.start(command, project.directory)) {
        m_status = trf("Could not start: %s", label.c_str());
        return;
    }
    MEALKIT_INFO("Module tool: %s", arguments.c_str());
}

void ModulesPanel::poll() {
    if (!m_process.running()) return;
    if (m_process.poll(m_output)) return;
    const bool ok = m_process.exitCode() == 0;
    if (m_listing) {
        m_list = parseModuleList(m_output);
        if (!ok) m_list.error = m_output;
        return;
    }
    m_log = m_output;
    m_logFailed = !ok;
    m_status = ok ? trf("%s: done", m_label.c_str()) : trf("%s: failed - see the Modules window", m_label.c_str());
    if (ok && m_changes) m_changed = true;
    m_needList = true; // recheck the current state whether it succeeded or not
}

void ModulesPanel::drawContent(const ProjectInfo& project, const std::string& engineDirectory, bool editing) {
    if (project.directory.empty()) return;
    if (m_projectDir != project.directory) {
        m_projectDir = project.directory;
        m_list = {};
        m_log.clear();
        m_needList = true;
    }
    if (m_needList && !m_process.running()) {
        m_needList = false;
        run(engineDirectory, project, tr("Module list"), "search -Tsv", false);
    }

    const bool busy = m_process.running();
    const bool canChange = editing && !busy;
    if (ImGui::Button(tr("Refresh")) && !busy) m_needList = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##search", tr("Search"), m_search, sizeof(m_search));
    ImGui::SameLine();
    if (busy) ImGui::TextDisabled("%s...", m_label.c_str());
    else if (!editing) ImGui::TextDisabled("%s", tr("Stop playing to change modules."));

    // Installed modules
    ImGui::SeparatorText(tr("In this project"));
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (m_list.installed.empty()) ImGui::TextDisabled("%s", tr("No modules. Add one from the list below."));
    else if (ImGui::BeginTable("##installed", 4, flags)) {
        ImGui::TableSetupColumn(tr("Module"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(tr("Version"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(tr("Changes"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 230);
        ImGui::TableHeadersRow();
        for (const InstalledModule& m : m_list.installed) {
            ImGui::PushID(m.name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(m.name.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.source.c_str());
            ImGui::TableNextColumn();
            const auto reg = std::find_if(m_list.registry.begin(), m_list.registry.end(),
                                          [&](const RegistryModule& r) { return r.name == m.name; });
            const bool fromRegistry = m.source.rfind("registry ", 0) == 0;
            const bool newer = fromRegistry && reg != m_list.registry.end() && !reg->version.empty() && reg->version != m.version;
            if (newer) ImGui::TextColored(kWarn, "%s -> %s", m.version.c_str(), reg->version.c_str());
            else ImGui::TextUnformatted(m.version.c_str());
            ImGui::TableNextColumn();
            if (m.pending > 0) ImGui::TextColored(kWarn, trf("%d .new file(s) to merge", m.pending).c_str());
            else if (m.modified > 0) ImGui::TextUnformatted(trf("%d file(s) changed here", m.modified).c_str());
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!canChange);
            if (ImGui::SmallButton(tr("Update")))
                run(engineDirectory, project, trf("Update %s", m.name.c_str()), "update " + m.name, true);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", tr("Files you changed are kept; the new version is written next to them as .new"));
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Remove"))) m_confirmRemove = m.name;
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Docs"))) openInExplorer(project.directory + "\\modules\\" + m.name + "\\docs");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Registry
    ImGui::SeparatorText(tr("Registry (MealKit-Modules)"));
    if (!m_list.error.empty()) ImGui::TextColored(kError, "%s", m_list.error.c_str());
    const float logHeight = m_log.empty() ? 0.0f : ImGui::GetTextLineHeightWithSpacing() * 7;
    if (ImGui::BeginTable("##registry", 3, flags | ImGuiTableFlags_ScrollY, ImVec2(0, -logHeight - 4))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Module"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(tr("Description"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("##add", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();
        for (const RegistryModule& r : m_list.registry) {
            if (!containsNoCase(r.name + " " + r.title + " " + r.summary + " " + r.tags + " " + r.author, m_search)) continue;
            ImGui::PushID(r.name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.name.c_str());
            if (r.version.empty()) ImGui::TextColored(kWarn, trf("%s (engine %s)", r.latest.c_str(), r.latestEngine.c_str()).c_str());
            else ImGui::TextDisabled("%s", r.version.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.title.c_str());
            ImGui::TextWrapped("%s", r.summary.c_str());
            ImGui::TextDisabled("%s  %s", trf("by %s", r.author.c_str()).c_str(), r.tags.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.repo.c_str());
            ImGui::TableNextColumn();
            const bool installed = std::any_of(m_list.installed.begin(), m_list.installed.end(),
                                               [&](const InstalledModule& m) { return m.name == r.name; });
            if (installed) ImGui::TextDisabled("%s", tr("added"));
            else {
                ImGui::BeginDisabled(!canChange);
                if (ImGui::SmallButton(tr("Add")))
                    run(engineDirectory, project, trf("Add %s", r.name.c_str()), "add " + r.name, true);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", tr("Module code runs inside the editor and the game. The registry checks that it builds, not what it does - read what you add."));
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!m_log.empty()) {
        if (m_logFailed) ImGui::TextColored(kError, "%s", tr("The last change failed:"));
        ImGui::InputTextMultiline("##log", m_log.data(), m_log.size() + 1, ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
    }

    if (!m_confirmRemove.empty()) ImGui::OpenPopup("##remove");
    if (ImGui::BeginPopupModal("##remove", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(trf("Remove modules/%s from the project?", m_confirmRemove.c_str()).c_str());
        ImGui::TextDisabled("%s", tr("It is refused while game code includes it. Undo with version control."));
        if (ImGui::Button(tr("Remove"))) {
            run(engineDirectory, project, trf("Remove %s", m_confirmRemove.c_str()), "remove " + m_confirmRemove, true);
            m_confirmRemove.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) {
            m_confirmRemove.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace mealkit::editor
