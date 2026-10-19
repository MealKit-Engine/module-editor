#include "mealkit/editor/BuildPanel.h"

#include "mealkit/core/Log.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"

#include <imgui.h>

namespace mealkit::editor {

// 프로젝트의 build.bat을 창 없이 실행한다.
//   GameCode: build.bat Editor → build\Editor\bin\<이름>_game.dll (엔진 mealkit.dll은 이미 빌드되어 있으면 건드리지 않는다)
//   Export:   build.bat Export → 출시용 exe만 다시 빌드하고 export\<이름>에 모은다
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
        const std::string script = project.directory + "\\build.bat";
        if (!fileExists(script)) {
            m_status = trf("build.bat not found: %s", script.c_str());
            return false;
        }
        const char* arg = kind == BuildKind::GameCode ? "Editor" : "Export";
        command = "\"" + script + "\" " + arg;
        m_log = std::string("> build.bat ") + arg + "\n";
    }
    m_kind = kind;
    m_scroll = true;
    if (kind != BuildKind::GameCode) m_open = true; // 게임 코드 빌드는 실패할 때만 연다
    if (!m_process.start(command, project.directory)) {
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
    // 실패한 명령은 ninja가 /showIncludes 줄까지 그대로 보여 준다. 에러를 찾기 쉽게 그 줄은 뺀다 (영문/한국어 MSVC).
    std::string fresh;
    const bool running = m_process.poll(fresh);
    size_t start = 0;
    for (size_t nl = fresh.find('\n'); nl != std::string::npos; nl = fresh.find('\n', start)) {
        const std::string line = fresh.substr(start, nl + 1 - start);
        start = nl + 1;
        if (line.find("Note: including file:") != std::string::npos ||
            line.find("참고: 포함 파일:") != std::string::npos) // i18n-ignore (한국어 MSVC 출력)
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

bool BuildPanel::drawLog(bool editing, const ProjectInfo& project, f64 now, BuildKind* request) {
    if (!m_open) return false;
    ImGui::SetNextWindowSize(ImVec2(720, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(trl(kWindow), &m_open)) {
        ImGui::End();
        return false;
    }
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
        ImGui::TextUnformatted(m_log.data(), m_log.data() + m_log.size());
        if (m_scroll) {
            ImGui::SetScrollHereY(1.0f);
            m_scroll = false;
        }
    }
    ImGui::EndChild();
    ImGui::End();
    return requested;
}

} // namespace mealkit::editor
