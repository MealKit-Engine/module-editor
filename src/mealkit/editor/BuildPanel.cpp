#include "mealkit/editor/BuildPanel.h"

#include "mealkit/core/Log.h"
#include "mealkit/editor/CodeLink.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"

#include <imgui.h>

#include <vector>

namespace mealkit::editor {

// Runs the project's build.bat without a window.
//   GameCode: build.bat Editor → build\Editor\bin\<name>_game.dll (the engine mealkit.dll isn't touched if already built)
//   Export:   build.bat Export → rebuilds only the shipping exe and gathers it in export\<name>
bool BuildPanel::start(BuildKind kind, const ProjectInfo& project, const std::string& engineDirectory, f64 now) {
    if (m_process.running() || project.directory.empty()) return false;
    std::string command;
    if (kind == BuildKind::Upgrade) {
        const std::string script = engineDirectory + "\\tools\\upgrade_project.ps1";
        if (!fileExists(script)) {
            m_status = trf("Upgrade script not found: %s", script.c_str());
            return false;
        }
        command = "powershell -NoProfile -ExecutionPolicy Bypass -File \"" + script + "\" -Project \"" + project.directory + "\"";
        m_log = "> upgrade_project.ps1\n";
    } else {
        const std::string script = project.buildRoot() + "\\build.bat";
        if (!fileExists(script)) {
            m_status = trf("build.bat not found: %s", script.c_str());
            return false;
        }
        const std::string arg = kind == BuildKind::GameCode ? editorBuildName() : std::string("Export");
        command = "\"" + script + "\" " + arg;
        m_log = std::string("> build.bat ") + arg + "\n";
    }
    m_kind = kind;
    m_scroll = true;
    if (kind != BuildKind::GameCode) m_open = true; // game code builds open only on failure
    if (!m_process.start(command, project.buildRoot())) {
        m_log += std::string(tr("Could not start the build.")) + "\n";
        m_open = true;
        return false;
    }
    m_start = now;
    MEALKIT_INFO("Build started: %s", command.c_str());
    return true;
}

bool BuildPanel::poll(const ProjectInfo& project, f64 now) {
    if (!m_process.running()) return false;
    // For a failed command ninja shows the /showIncludes lines as is. Those lines are dropped to make errors easier to find (English/Korean MSVC).
    std::string fresh;
    const bool running = m_process.poll(fresh);
    size_t start = 0;
    for (size_t nl = fresh.find('\n'); nl != std::string::npos; nl = fresh.find('\n', start)) {
        const std::string line = fresh.substr(start, nl + 1 - start);
        start = nl + 1;
        if (line.find("Note: including file:") != std::string::npos ||
            line.find("참고: 포함 파일:") != std::string::npos) // i18n-ignore (Korean MSVC output)
            continue;
        m_log += line;
        m_scroll = true;
    }
    if (running) return false;
    m_scroll = true;
    m_succeeded = m_process.exitCode() == 0;
    const std::string seconds = std::to_string(static_cast<int>(now - m_start));
    if (!m_succeeded) {
        m_log += "\n" + trf("Failed (exit code %d)", m_process.exitCode()) + "\n";
        m_open = true;
        m_status = m_kind == BuildKind::GameCode  ? tr("Game code build failed - see the build log")
                   : m_kind == BuildKind::Upgrade ? tr("Upgrade failed - see the build log")
                                                  : tr("Release build failed - see the build log");
        MEALKIT_ERROR("Build failed with exit code %d", m_process.exitCode());
    } else if (m_kind == BuildKind::Upgrade) {
        m_log += std::string("\n") + tr("Upgrade done - rebuilding the game code") + "\n";
    } else if (m_kind == BuildKind::GameCode) {
        m_log += "\n" + trf("Done (%ss): game code", seconds.c_str()) + "\n";
        MEALKIT_INFO("Game code build finished");
    } else {
        m_log += "\n" + trf("Done (%ss): %s", seconds.c_str(), project.exportDirectory().c_str()) + "\n";
        m_status = trf("Release build done: %s", ("export\\" + project.name()).c_str());
        MEALKIT_INFO("Export finished: %s", project.exportDirectory().c_str());
    }
    return true;
}

bool BuildPanel::drawStatus(f64 now) const {
    if (!m_process.running()) return false;
    ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.35f, 1.0f),
                       m_kind == BuildKind::GameCode  ? tr("Building game code... %.0fs")
                       : m_kind == BuildKind::Upgrade ? tr("Upgrading project... %.0fs")
                                                      : tr("Release build... %.0fs"),
                       now - m_start);
    return true;
}

bool BuildPanel::drawContent(bool editing, const ProjectInfo& project, f64 now, BuildKind* request) {
    bool requested = false;
    ImGui::BeginDisabled(m_process.running() || project.directory.empty() || !editing);
    if (ImGui::Button(tr("Build Game Code"))) {
        *request = BuildKind::GameCode;
        requested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Release Build"))) {
        *request = BuildKind::Export;
        requested = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!directoryExists(project.exportDirectory()));
    if (ImGui::Button(tr("Open Release Folder"))) openInExplorer(project.exportDirectory());
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (m_process.running())
        ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.35f, 1.0f), tr("Building... %.0fs"), now - m_start);
    ImGui::Separator();
    if (ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
        // Per line: if there's a "file(line)", make it clickable (opens in the code editor; relative paths are relative to the project folder)
        std::vector<std::pair<size_t, size_t>> lines;
        for (size_t pos = 0; pos < m_log.size();) {
            size_t end = m_log.find('\n', pos);
            if (end == std::string::npos) end = m_log.size();
            lines.emplace_back(pos, end);
            pos = end + 1;
        }
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(lines.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const std::string line = m_log.substr(lines[i].first, lines[i].second - lines[i].first);
                FileLine at;
                if (!parseFileLine(line, &at)) {
                    ImGui::TextUnformatted(line.c_str());
                    continue;
                }
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.75f, 1.0f, 1.0f));
                ImGui::TextUnformatted(line.c_str());
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("%s", tr("Click to open in the code editor"));
                }
                if (ImGui::IsItemClicked()) {
                    const bool absolute = at.file.size() > 1 && (at.file[1] == ':' || at.file[0] == '\\');
                    openInCodeEditor(m_codeEditor, absolute ? at.file : project.directory + "\\" + at.file, at.line);
                }
            }
        }
        if (m_scroll) {
            ImGui::SetScrollHereY(1.0f);
            m_scroll = false;
        }
    }
    ImGui::EndChild();
    return requested;
}

} // namespace mealkit::editor
