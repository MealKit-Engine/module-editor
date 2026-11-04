#include "mealkit/editor/TestsPanel.h"

#include "mealkit/editor/GameModuleHost.h"
#include "mealkit/editor/SharedBuild.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"

#include <imgui.h>

#include <algorithm>

namespace mealkit::editor {
namespace {

std::string headlessExe(const ProjectInfo& project) { return project.buildRoot() + "\\build\\Release\\bin\\" + project.name() + "_headless.exe"; }

} // namespace

void TestsPanel::refresh(const ProjectInfo& project) {
    std::vector<Result> next;
    for (const std::string& file : listFiles(project.directory + "/tests", ".scenario")) {
        const auto old = std::find_if(m_results.begin(), m_results.end(), [&](const Result& r) { return r.file == file; });
        if (old != m_results.end()) next.push_back(std::move(*old));
        else next.push_back({file});
    }
    m_results = std::move(next);
}

void TestsPanel::runAll(const ProjectInfo& project) {
    if (running() || project.directory.empty()) return;
    refresh(project);
    for (Result& r : m_results) {
        r.state = State::Queued;
        r.update = false;
    }
    queue(project);
}

// The headless exe has the rules compiled in; data files are read at run time, so only code changes need a build
void TestsPanel::queue(const ProjectInfo& project) {
    m_status.clear();
    m_output.clear();
    const std::string exe = headlessExe(project);
    if (fileExists(exe) && !GameModuleHost::olderThanSources(exe, project.directory)) {
        startNext(project);
        return;
    }
    if (!hasCompiler()) { // designer PC: only what the shared folder has
        if (fileExists(exe)) {
            startNext(project);
        } else {
            m_status = tr("No test program: ask a programmer to publish a Release build.");
            for (Result& r : m_results)
                if (r.state == State::Queued) r.state = State::NotRun;
        }
        return;
    }
    m_status = tr("Building the test program...");
    if (m_process.start("\"" + project.buildRoot() + "\\build.bat\"", project.buildRoot())) {
        m_stage = Stage::Building;
    } else {
        m_status = tr("Could not start the build.");
        for (Result& r : m_results)
            if (r.state == State::Queued) r.state = State::NotRun;
    }
}

void TestsPanel::startNext(const ProjectInfo& project) {
    m_stage = Stage::Idle;
    m_current = -1;
    for (size_t i = 0; i < m_results.size(); ++i) {
        Result& r = m_results[i];
        if (r.state != State::Queued) continue;
        m_output.clear();
        std::string command = "\"" + headlessExe(project) + "\" --test tests\\" + r.file;
        if (r.update) command += " --update-expected";
        if (!m_process.start(command, project.directory)) {
            r.state = State::Failed;
            r.summary = tr("Could not start the test program.");
            continue;
        }
        r.state = State::Running;
        m_current = static_cast<int>(i);
        m_stage = Stage::Testing;
        return;
    }
}

void TestsPanel::finishTest() {
    Result& r = m_results[static_cast<size_t>(m_current)];
    r.failures.clear();
    r.summary.clear();
    bool passed = false;
    std::string last;
    size_t pos = 0;
    while (pos < m_output.size()) {
        size_t end = m_output.find('\n', pos);
        if (end == std::string::npos) end = m_output.size();
        std::string line = m_output.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) last = line;
        if (line.rfind("SCENARIO ok ", 0) == 0) {
            passed = true;
            if (const size_t c = line.rfind("checks="); c != std::string::npos) r.summary = trf("%s checks passed", line.substr(c + 7).c_str());
        } else if (line.rfind("SCENARIO FAIL ", 0) == 0) {
            // "SCENARIO FAIL <file>:<line> tick ..." per check, "SCENARIO FAIL <file> failed=K/N" at the end
            if (const size_t tick = line.find(" tick "); tick != std::string::npos) {
                const size_t colon = line.rfind(':', tick);
                r.failures.push_back(trf("line %s:", line.substr(colon + 1, tick - colon - 1).c_str()) + line.substr(tick));
            } else if (const size_t f = line.find("failed="); f != std::string::npos) {
                r.summary = trf("%s checks failed", line.substr(f + 7).c_str());
            } else {
                r.summary = line;
            }
        }
    }
    r.state = passed && m_process.exitCode() == 0 ? State::Passed : State::Failed;
    if (r.state == State::Failed && r.summary.empty()) r.summary = last; // crashed or refused the file
    r.update = false;
}

void TestsPanel::poll(const ProjectInfo& project, f64 dt) {
    if (m_stage == Stage::Idle) {
        m_refresh -= dt;
        if (m_refresh <= 0.0 && !project.directory.empty()) {
            m_refresh = 1.0;
            refresh(project);
        }
        return;
    }
    if (m_process.poll(m_output)) return;
    if (m_stage == Stage::Building) {
        if (m_process.exitCode() != 0) {
            m_status = tr("The test program didn't build: the game code needs fixing. See the output below.");
            m_stage = Stage::Idle;
            for (Result& r : m_results)
                if (r.state == State::Queued) r.state = State::NotRun;
            return;
        }
        m_status.clear();
    } else {
        finishTest();
    }
    startNext(project);
}

u32 TestsPanel::failedCount() const {
    return static_cast<u32>(std::count_if(m_results.begin(), m_results.end(), [](const Result& r) { return r.state == State::Failed; }));
}

void TestsPanel::drawContent(const ProjectInfo& project) {
    ImGui::BeginDisabled(running());
    if (ImGui::Button(tr("Run All Tests"))) runAll(project);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr("Uses saved files: save tuning and scenes first."));
    if (!m_status.empty()) ImGui::TextUnformatted(m_status.c_str());
    if (m_results.empty()) ImGui::TextDisabled("%s", tr("No tests yet. During play, Save Scenario on the play bar makes one."));

    for (size_t i = 0; i < m_results.size(); ++i) {
        Result& r = m_results[i];
        ImGui::PushID(static_cast<int>(i));
        ImVec4 color(0.6f, 0.6f, 0.6f, 1.0f);
        const char* state = tr("Not run");
        if (r.state == State::Queued) state = tr("Waiting");
        if (r.state == State::Running) state = tr("Running...");
        if (r.state == State::Passed) { state = tr("Passed"); color = ImVec4(0.4f, 0.85f, 0.45f, 1.0f); }
        if (r.state == State::Failed) { state = tr("Failed"); color = ImVec4(1.0f, 0.38f, 0.38f, 1.0f); }
        ImGui::TextColored(color, "%s", state);
        ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
        ImGui::TextUnformatted(r.file.c_str());
        if (!r.summary.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", r.summary.c_str());
        }
        if (r.state == State::Failed && !r.failures.empty()) {
            ImGui::SameLine();
            ImGui::BeginDisabled(running());
            if (ImGui::SmallButton(tr("Accept as Intended"))) {
                r.state = State::Queued;
                r.update = true;
                queue(project);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", tr("The change is what you wanted: writes the current results into this test as the new expected values."));
            for (const std::string& f : r.failures) ImGui::BulletText("%s", f.c_str());
        }
        ImGui::PopID();
    }
    if (m_stage == Stage::Building || (!m_status.empty() && !m_output.empty())) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_output.c_str());
    }
}

} // namespace mealkit::editor
