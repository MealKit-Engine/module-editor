#include "mealkit/editor/Editor.h"
#include "mealkit/editor/EditorColors.h"

#include "mealkit/asset/ModelCache.h"
#include "mealkit/core/Memory.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Window.h"
#include "mealkit/rhi/GpuCapture.h"
#include "mealkit/platform/Process.h"

#include <ImGuizmo.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <set>

#include <imgui.h>
#include <imgui_internal.h>

// [Data] window (tuning, tables, feedback and sounds under one list) and [Problems] window (everything that needs fixing in one list)
namespace mealkit::editor {
namespace {

using scene::Entity;

constexpr const char* kDataWindow = "Data###Data";

} // namespace

void Editor::openDataPage(DataPage page, int index) {
    m_dataOpen = true;
    m_dataPage = page;
    m_dataIndex = index;
    m_dataFrontFrames = 3;
    if (page == DataPage::Table) m_tablesPanel.setCurrent(index);
}

void Editor::toggleDataPage(DataPage page, int index) {
    if (m_dataOpen && m_dataPage == page && (page == DataPage::Table || m_dataIndex == index)) m_dataOpen = false;
    else openDataPage(page, page == DataPage::Table ? m_tablesPanel.current() : index);
}

bool Editor::dataCanStep(bool back) const {
    switch (m_dataPage) {
    case DataPage::Tuning: return m_tuningPanel.canStep(m_dataIndex, back);
    case DataPage::Table: return m_tablesPanel.canStep(m_dataIndex, back);
    case DataPage::Feedback: return m_feedbackPanel.canStep(back);
    case DataPage::Effects: return m_feedbackPanel.canStepEffect(back);
    case DataPage::Sounds: return false;
    }
    return false;
}

void Editor::dataStep(bool back) {
    switch (m_dataPage) {
    case DataPage::Tuning: m_tuningPanel.step(m_dataIndex, back); break;
    case DataPage::Table: m_tablesPanel.step(m_dataIndex, back); break;
    case DataPage::Feedback: m_feedbackPanel.step(back); break;
    case DataPage::Effects: m_feedbackPanel.stepEffect(back); break;
    case DataPage::Sounds: break;
    }
}

void Editor::drawDataHub(f64 dt, bool editing) {
    m_feedbackPanel.update(dt); // autosave continues after the window closes
    m_dataFocused = false;
    if (!m_dataOpen) return;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 62.0f, ImGui::GetFontSize() * 36.0f), ImGuiCond_FirstUseEver);
    if (m_dataFrontFrames > 0) {
        --m_dataFrontFrames;
        ImGui::SetNextWindowFocus();
    }
    if (!ImGui::Begin(iconTitle(icon::kData, kDataWindow).c_str(), &m_dataOpen)) {
        ImGui::End();
        return;
    }
    m_dataFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // ---- Left: every data document, with unsaved marks
    if (beginListPane("##dataList", ImGui::GetFontSize() * 15.0f)) {
        auto entry = [this](const std::string& label, DataPage page, int index, bool dirty) {
            const std::string text = label + (dirty ? " *" : "") + "##" + std::to_string(static_cast<int>(page)) + "_" + std::to_string(index);
            if (ImGui::Selectable(text.c_str(), m_dataPage == page && m_dataIndex == index)) openDataPage(page, index);
        };
        ImGui::SeparatorText(tr("Tuning"));
        entry(tr("Game"), DataPage::Tuning, 0, m_tuningPanel.docDirty(0));
        entry(tr("Display"), DataPage::Tuning, 1, m_tuningPanel.docDirty(1));
        ImGui::SeparatorText(tr("Tables"));
        for (int i = 0; i < m_tablesPanel.docCount(); ++i) entry(m_tablesPanel.docName(i), DataPage::Table, i, m_tablesPanel.docDirty(i));
        if (m_tablesPanel.docCount() == 0) ImGui::TextDisabled("%s", tr("Tables appear when the game code is loaded (MEALKIT_TABLE)."));
        ImGui::SeparatorText(tr("Reactions"));
        entry(tr("Feedback"), DataPage::Feedback, 0, false);
        entry(tr("Effects"), DataPage::Effects, 0, false);
        if (m_audio) entry(tr("Sounds"), DataPage::Sounds, 0, false);
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ---- Right: the chosen document
    if (ImGui::BeginChild("##dataPage")) {
        ImGui::BeginDisabled(!dataCanStep(true));
        if (ImGui::Button(tr("Undo"))) dataStep(true);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!dataCanStep(false));
        if (ImGui::Button(tr("Redo"))) dataStep(false);
        ImGui::EndDisabled();
        switch (m_dataPage) {
        case DataPage::Tuning:
            m_tuningPanel.drawContent(m_dataIndex, m_mode == Mode::Play, m_playPlayers > 1, m_tablesPanel.index());
            break;
        case DataPage::Table: m_tablesPanel.drawContent(m_dataIndex, m_window->nativeHandle(), editing); break;
        case DataPage::Feedback: m_feedbackPanel.drawContent(m_feedback, m_cameraPos + cameraForward() * 10.0f); break;
        case DataPage::Effects: m_feedbackPanel.drawEffects(m_feedback, m_cameraPos + cameraForward() * 10.0f); break;
        case DataPage::Sounds:
            if (m_audio) m_soundPanel.drawPreview(*m_audio);
            break;
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void Editor::setTestInput(const std::string& script) {
    m_testInput.clear();
    size_t start = 0;
    while (start <= script.size()) {
        const size_t end = std::min(script.find(';', start), script.size());
        std::string step = script.substr(start, end - start);
        while (!step.empty() && step.front() == ' ') step.erase(0, 1);
        if (!step.empty()) m_testInput.push_back(std::move(step));
        start = end + 1;
    }
}

// One step per frame. A mouse move stays for later frames, because the platform layer also reports the real mouse each frame
void Editor::runTestInput() {
    if (m_testInput.empty()) return;
    ImGuiIO& io = ImGui::GetIO();
    io.AddFocusEvent(true);
    if (m_testStep > 0 || m_testX != 0.0f) io.AddMousePosEvent(m_testX, m_testY);
    if (m_testWait > 0) {
        --m_testWait;
        return;
    }
    if (m_testStep >= m_testInput.size()) return;
    const std::string step = m_testInput[m_testStep++];
    const size_t space = step.find(' ');
    const std::string verb = step.substr(0, space), rest = space == std::string::npos ? std::string() : step.substr(space + 1);
    if (verb == "wait") {
        m_testWait = std::atoi(rest.c_str());
    } else if (verb == "move") {
        std::sscanf(rest.c_str(), "%f %f", &m_testX, &m_testY);
        io.AddMousePosEvent(m_testX, m_testY);
    } else if (verb == "down" || verb == "up") {
        io.AddMouseButtonEvent(rest == "right" ? ImGuiMouseButton_Right : ImGuiMouseButton_Left, verb == "down");
    } else if (verb == "key") {
        const bool ctrl = rest.rfind("ctrl+", 0) == 0;
        const std::string name = ctrl ? rest.substr(5) : rest;
        ImGuiKey key = ImGuiKey_None;
        if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') key = static_cast<ImGuiKey>(ImGuiKey_A + (name[0] - 'a'));
        else if (name == "enter") key = ImGuiKey_Enter;
        else if (name == "escape") key = ImGuiKey_Escape;
        else if (name == "delete") key = ImGuiKey_Delete;
        else if (name == "f5") key = ImGuiKey_F5;
        // Pressed and released in one frame: ImGui's event queue keeps them apart
        if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(key, true);
        io.AddKeyEvent(key, false);
        if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
    } else if (verb == "type") {
        io.AddInputCharactersUTF8(rest.c_str());
    } else if (verb == "command") {
        queueCommand(rest);
    } else if (verb == "print") {
        const Entity* e = selected();
        std::printf("INPUT selected=%u multi=%zu sceneDirty=%d undo=%zu tuningDirty=%d folder=%s x=%.3f viewport=%.0f,%.0f,%.0f,%.0f fields=", m_selected, m_multi.size(), m_dirty ? 1 : 0, m_undo.size(),
                    m_tuningPanel.docDirty(0) ? 1 : 0, e ? e->folder.c_str() : "", e ? static_cast<double>(e->position.x) : 0.0, static_cast<double>(m_viewport.x),
                    static_cast<double>(m_viewport.y), static_cast<double>(m_viewport.width), static_cast<double>(m_viewport.height));
        if (e)
            for (const auto& [key, value] : e->fields) std::printf("%s=%s,", key.c_str(), value.c_str());
        std::printf("\n");
        std::fflush(stdout);
    }
}

void Editor::updateStateGraphs() {
    if (m_mode != Mode::Play || !m_stateView.type || !m_stateView.data) return;
    constexpr size_t kSamples = 600; // ten seconds at 60 fps
    for (StateGraph& g : m_graphs) {
        const reflect::ValueInfo* value = nullptr;
        void* data = nullptr;
        if (!reflect::resolvePath(*m_stateView.type, const_cast<void*>(m_stateView.data), g.path, value, data)) continue;
        const float sample = std::strtof(reflect::formatScalar(*value, data).c_str(), nullptr);
        if (g.samples.size() < kSamples) g.samples.push_back(sample);
        else g.samples[g.next] = sample;
        g.next = (g.next + 1) % kSamples;
    }
}

void Editor::drawStateGraphs() {
    if (!sectionHeader(tr("Graphs"))) return;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    const bool enter = ImGui::InputTextWithHint("##graphPath", tr("Field path, such as tick"), m_graphPath, sizeof(m_graphPath), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const reflect::ValueInfo* value = nullptr;
    void* data = nullptr;
    const bool valid = reflect::resolvePath(*m_stateView.type, const_cast<void*>(m_stateView.data), m_graphPath, value, data) && !reflect::formatScalar(*value, data).empty();
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button(tr("Add Graph")) || (enter && valid)) {
        m_graphs.push_back({m_graphPath, {}, 0});
        m_graphPath[0] = 0;
    }
    ImGui::EndDisabled();
    int remove = -1;
    for (size_t i = 0; i < m_graphs.size(); ++i) {
        const StateGraph& g = m_graphs[i];
        ImGui::PushID(static_cast<int>(i));
        const size_t offset = g.samples.size() < 600 ? 0 : g.next;
        const std::string last = g.samples.empty() ? std::string() : std::to_string(g.samples[(g.next + g.samples.size() - 1) % g.samples.size()]);
        ImGui::PlotLines("##plot", g.samples.data(), static_cast<int>(g.samples.size()), static_cast<int>(offset), last.c_str(), FLT_MAX, FLT_MAX,
                         ImVec2(ImGui::GetContentRegionAvail().x * 0.7f, ImGui::GetFontSize() * 3.0f));
        ImGui::SameLine();
        ImGui::TextUnformatted(g.path.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = static_cast<int>(i);
        ImGui::PopID();
    }
    if (remove >= 0) m_graphs.erase(m_graphs.begin() + remove);
}

void Editor::openDebugPage(DebugPage page) {
    m_debugOpen = true;
    m_debugPage = page;
    m_debugFrontFrames = 3;
}

void Editor::toggleDebugPage(DebugPage page) {
    if (m_debugOpen && m_debugPage == page) m_debugOpen = false;
    else openDebugPage(page);
}

// [Debug] window: everything for finding out what went wrong, one list on the left. It starts as a tab in the bottom strip
void Editor::drawDebugHub() {
    if (!m_debugOpen) return;
    if (ImGuiWindow* assets = ImGui::FindWindowByName("Assets###Assets"); assets && assets->DockId)
        ImGui::SetNextWindowDockID(assets->DockId, ImGuiCond_FirstUseEver); // old layout files don't have this window
    if (m_debugFrontFrames > 0) {
        --m_debugFrontFrames;
        ImGui::SetNextWindowFocus();
    }
    // The problem count is in the tab, so problems show while another tab is in front
    const std::string title = std::string(icon::kDebug) + "  " + (m_problems.empty() ? std::string(tr("Debug")) : trf("Debug (%zu)", m_problems.size())) + "###Debug";
    if (!ImGui::Begin(title.c_str(), &m_debugOpen)) {
        ImGui::End();
        return;
    }
    if (beginListPane("##debugList", ImGui::GetFontSize() * 9.0f)) {
        const auto entry = [this](const char* glyph, const std::string& label, DebugPage page) {
            if (ImGui::Selectable((std::string(glyph) + "  " + label + "##" + std::to_string(static_cast<int>(page))).c_str(), m_debugPage == page)) openDebugPage(page);
        };
        entry(icon::kLog, tr("Log"), DebugPage::Log);
        entry(icon::kBuild, m_buildPanel.running() ? std::string(tr("Build Log")) + " ..." : std::string(tr("Build Log")), DebugPage::Build);
        entry(icon::kPerformance, tr("Performance"), DebugPage::Performance);
        entry(icon::kPerformance, tr("Memory"), DebugPage::Memory);
        if (m_audio) entry(icon::kSound, tr("Sounds"), DebugPage::Sound);
        entry(icon::kState, tr("State"), DebugPage::State);
        const u32 failedTests = m_testsPanel.failedCount();
        entry(icon::kTests, failedTests ? trf("Tests (%u failed)", failedTests) : std::string(tr("Tests")), DebugPage::Tests);
        entry(icon::kBalance, tr("Balance"), DebugPage::Balance);
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##debugPage")) {
        switch (m_debugPage) {
        case DebugPage::Log: drawLogPage(); break;
        case DebugPage::Build: {
            BuildKind request = BuildKind::GameCode;
            if (m_buildPanel.drawContent(m_mode == Mode::Edit, m_project, m_time, &request)) startBuild(request);
            break;
        }
        case DebugPage::Performance: {
            // Off shows the real frame cost instead of the monitor refresh interval
            bool vsync = m_renderer->vsync();
            if (ImGui::Checkbox(tr("Vertical Sync"), &vsync)) m_renderer->setVsync(vsync);
            // Only when RenderDoc/PIX was loaded with --gpu-capture
            if (const char* tool = rhi::gpuCaptureTool()) {
                ImGui::SameLine();
                if (ImGui::Button(trf("GPU Capture (%s)", tool).c_str())) captureGpuFrame();
            }
            m_perfPanel.drawContent();
            break;
        }
        case DebugPage::Memory: drawMemoryPage(); break;
        case DebugPage::Sound:
            if (m_audio) m_soundPanel.drawMonitor(*m_audio);
            break;
        case DebugPage::State: drawState(); break;
        case DebugPage::Tests: m_testsPanel.drawContent(m_project); break;
        case DebugPage::Balance: m_balancePanel.drawContent(m_project); break;
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// Memory tags (core/Memory.h): bytes now and peak per engine module, allocation calls since start
void Editor::drawMemoryPage() {
    ImGui::TextDisabled("%s", tr("Memory counted by engine module tags. Process total is on the Performance page."));
    if (!ImGui::BeginTable("##memTags", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn(tr("Tag"));
    ImGui::TableSetupColumn(tr("Now"));
    ImGui::TableSetupColumn(tr("Peak"));
    ImGui::TableSetupColumn(tr("Allocations"));
    ImGui::TableHeadersRow();
    const auto mb = [](u64 bytes) { return static_cast<double>(bytes) / 1048576.0; };
    u64 total = 0, totalPeak = 0;
    for (u32 i = 0; i < mem::tagCount(); ++i) {
        const mem::Tag tag{static_cast<u16>(i)};
        const mem::TagStats s = mem::stats(tag);
        total += s.bytes;
        totalPeak += s.peak;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(mem::tagName(tag)); // tag names are code identifiers, not translated
        ImGui::TableNextColumn();
        ImGui::Text("%.2f MB", mb(s.bytes));
        ImGui::TableNextColumn();
        ImGui::Text("%.2f MB", mb(s.peak));
        ImGui::TableNextColumn();
        ImGui::Text("%llu", static_cast<unsigned long long>(s.allocations));
    }
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(tr("Total"));
    ImGui::TableNextColumn();
    ImGui::Text("%.2f MB", mb(total));
    ImGui::TableNextColumn();
    ImGui::Text("%.2f MB", mb(totalPeak));
    ImGui::EndTable();
}

void Editor::openSettingsPage(SettingsPage page) {
    m_settingsOpen = true;
    m_settingsPage = page;
    m_settingsFrontFrames = 3;
}

void Editor::openPreviewPage(PreviewPage page) {
    m_previewOpen = true;
    m_previewPage = page;
    m_previewFrontFrames = 3;
    if (page == PreviewPage::Cutscene) m_previewCutscenes.load(); // assets/tables/cutscenes.table as it is now
}

// [Settings] window: project settings, preferences, modules, tools and shortcuts under one list
void Editor::drawSettingsHub() {
    if (!m_settingsOpen) return;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 54.0f, ImGui::GetFontSize() * 34.0f), ImGuiCond_FirstUseEver);
    if (m_settingsFrontFrames > 0) {
        --m_settingsFrontFrames;
        ImGui::SetNextWindowFocus();
    }
    if (!ImGui::Begin(iconTitle(icon::kSettings, "Settings###Settings").c_str(), &m_settingsOpen)) {
        ImGui::End();
        return;
    }
    if (beginListPane("##settingsList", ImGui::GetFontSize() * 10.0f)) {
        const auto entry = [this](const std::string& label, SettingsPage page) {
            if (ImGui::Selectable((label + "##" + std::to_string(static_cast<int>(page))).c_str(), m_settingsPage == page)) m_settingsPage = page;
        };
        entry(std::string(tr("Project Settings")) + (m_projectPanel.dirty() ? " *" : ""), SettingsPage::Project);
        entry(tr("Preferences"), SettingsPage::Preferences);
        entry(tr("Tools"), SettingsPage::Tools);
        entry(tr("Keyboard Shortcuts"), SettingsPage::Shortcuts);
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##settingsPage")) {
        switch (m_settingsPage) {
        case SettingsPage::Project:
            ImGui::BeginDisabled(m_mode != Mode::Edit);
            m_projectPanel.drawContent(m_project, relativeTo(m_project.directory + "\\assets", m_scenePath));
            ImGui::EndDisabled();
            break;
        case SettingsPage::Preferences: m_prefsPanel.drawContent(m_prefs); break;
        case SettingsPage::Tools:
            if (m_tools.empty()) ImGui::TextDisabled("%s", tr("No tools installed"));
            for (const ToolInfo& t : m_tools) {
                ImGui::PushID(t.folder.c_str());
                ImGui::BeginDisabled(t.problem != ToolInfo::Problem::None);
                if (ImGui::SmallButton(tr("Open"))) launchTool(t, m_project.directory);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextUnformatted((t.version.empty() ? t.name : t.name + "  " + t.version).c_str());
                ImGui::SameLine();
                if (t.problem == ToolInfo::Problem::Protocol) ImGui::TextColored(color::kDanger, "%s", trf("Needs tool protocol %u, this editor has %u", t.protocol, live::kProtocol).c_str());
                else if (t.problem == ToolInfo::Problem::MissingExe) ImGui::TextColored(color::kDanger, "%s", trf("Program not found: %s", t.exe.c_str()).c_str());
                else ImGui::TextDisabled("%s", t.folder.c_str());
                ImGui::PopID();
            }
            ImGui::Separator();
            if (ImGui::Button(tr("Open Tools Folder"))) {
                createDirectories(defaultToolsFolder());
                openInExplorer(defaultToolsFolder());
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Rescan Tools"))) m_tools = findTools(m_toolPaths);
            break;
        case SettingsPage::Shortcuts: drawShortcutHelp(); break;
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// [Previews] window: game screens, models and cutscenes. The UI overlay and look dev rendering run only while their page is chosen
void Editor::drawPreviewHub() {
    m_uiPanelOpen = m_previewOpen && m_previewPage == PreviewPage::Ui;
    m_lookdevOpen = m_previewOpen && m_previewPage == PreviewPage::LookDev;
    if (!m_previewOpen) return;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 40.0f, ImGui::GetFontSize() * 34.0f), ImGuiCond_FirstUseEver);
    if (m_previewFrontFrames > 0) {
        --m_previewFrontFrames;
        ImGui::SetNextWindowFocus();
    }
    if (!ImGui::Begin(iconTitle(icon::kPreview, "Previews###Previews").c_str(), &m_previewOpen)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabBar("##previewPages")) {
        // Follows m_previewPage when a command picks the page
        const auto flagged = [this](const char* label, PreviewPage page) {
            bool chosen = ImGui::BeginTabItem(label, nullptr, m_previewFrontFrames > 0 && m_previewPage == page ? ImGuiTabItemFlags_SetSelected : 0);
            if (chosen) {
                if (m_previewPage != page && m_previewFrontFrames == 0) openPreviewPage(page);
                ImGui::EndTabItem();
            }
        };
        flagged(tr("UI"), PreviewPage::Ui);
        flagged(tr("Look Dev"), PreviewPage::LookDev);
        flagged(tr("Preview Cutscene"), PreviewPage::Cutscene);
        ImGui::EndTabBar();
    }
    if (ImGui::BeginChild("##previewPage")) {
        switch (m_previewPage) {
        case PreviewPage::Ui: m_uiPanel.drawContent(); break;
        case PreviewPage::LookDev:
            if (m_renderer) m_lookdev.drawContent(*m_renderer);
            break;
        case PreviewPage::Cutscene:
            ImGui::BeginDisabled(m_mode != Mode::Edit);
            for (u32 i = 0; i < m_previewCutscenes.count(); ++i) {
                const asset::Cutscenes::Cutscene& c = m_previewCutscenes.at(i);
                char label[160];
                std::snprintf(label, sizeof(label), "%s (%.1f s)", c.name.c_str(), c.clip ? static_cast<double>(c.clip->duration) : 0.0);
                if (ImGui::Selectable(label, c.name == m_cutscenePreview, c.clip ? 0 : ImGuiSelectableFlags_Disabled)) previewCutscene(c.name);
            }
            if (m_previewCutscenes.count() == 0) ImGui::TextDisabled("%s", tr("No rows in tables/cutscenes.table"));
            ImGui::EndDisabled();
            break;
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// Status bar: how many documents differ from their files; clicking lists them with save and revert
void Editor::drawUnsavedStatus() {
    int count = (m_dirty ? 1 : 0) + (m_tuningPanel.docDirty(0) ? 1 : 0) + (m_tuningPanel.docDirty(1) ? 1 : 0) + (m_uiPanel.dirty() ? 1 : 0) + (m_projectPanel.dirty() ? 1 : 0);
    for (int i = 0; i < m_tablesPanel.docCount(); ++i) count += m_tablesPanel.docDirty(i) ? 1 : 0;
    if (count > 0) {
        ImGui::TextColored(color::kWarning, "%s", trf("Unsaved: %d", count).c_str());
        if (ImGui::IsItemClicked()) ImGui::OpenPopup("##unsaved");
        ImGui::SameLine();
    }
    if (!ImGui::BeginPopup("##unsaved")) {
        m_revertSceneArmed = false;
        return;
    }
    if (count == 0) ImGui::TextDisabled("%s", tr("Everything is saved."));
    const auto row = [](const std::string& name, const std::function<void()>& save, const std::function<void()>& revert, const char* revertLabel) {
        ImGui::PushID(name.c_str());
        if (ImGui::SmallButton(tr("Save"))) save();
        ImGui::SameLine();
        ImGui::BeginDisabled(!revert);
        if (ImGui::SmallButton(revertLabel) && revert) revert();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted(name.c_str());
        ImGui::PopID();
    };
    if (m_dirty)
        row(m_scenePath.empty() ? std::string(tr("Untitled")) : fileName(m_scenePath), [this] { saveScene(); },
            m_scenePath.empty() || m_mode != Mode::Edit ? std::function<void()>() : [this] {
                if (m_revertSceneArmed) loadScene(m_scenePath);
                m_revertSceneArmed = !m_revertSceneArmed;
            }, m_revertSceneArmed ? tr("Really revert?") : tr("Revert"));
    for (int i = 0; i < 2; ++i)
        if (m_tuningPanel.docDirty(i))
            row(i == 0 ? "game.tuning" : "view.tuning", [this, i] { m_tuningPanel.saveDoc(i); }, [this, i] { m_tuningPanel.revertDoc(i); }, tr("Revert"));
    for (int i = 0; i < m_tablesPanel.docCount(); ++i)
        if (m_tablesPanel.docDirty(i))
            row(m_tablesPanel.docName(i), [this, i] { m_tablesPanel.saveDoc(i); }, [this, i] { m_tablesPanel.revertDoc(i); }, tr("Revert"));
    if (m_uiPanel.dirty()) row(tr("UI"), [this] { m_uiPanel.save(); }, {}, tr("Revert"));
    if (m_projectPanel.dirty())
        row(tr("Project Settings"), [this] { m_projectPanel.save(m_project); }, [this] { m_projectPanel.revert(m_project); }, tr("Revert"));
    ImGui::EndPopup();
}

// Gathers problems twice a second: missing models, models over budget, and row references in tables, tuning and scene objects
void Editor::updateProblems(f64 dt) {
    m_problemTimer += dt;
    if (m_problemTimer < 0.5) return;
    m_problemTimer = 0.0;
    m_problems.clear();
    m_badges.clear();
    m_differsFromDefault.clear();
    for (const Entity& e : m_scene.entities) { // field changes from the kind's defaults
        const reflect::TypeInfo* type = entityType(e);
        if (!type || type->fields.empty()) continue;
        const reflect::TextFields defaults = scene::defaultFields(*type);
        for (const auto& field : e.fields)
            if (std::find(defaults.begin(), defaults.end(), field) == defaults.end()) {
                m_differsFromDefault.insert(e.id);
                break;
            }
    }

    const std::string root = asset::ModelCache::assetsRoot();
    std::set<std::string> seen;
    for (const Entity& e : m_scene.entities) {
        if (e.model.empty()) continue;
        const bool first = seen.insert(e.model).second; // one list line per model, a badge on every object using it
        std::string native = e.model;
        std::replace(native.begin(), native.end(), '/', '\\');
        if (!fileExists(root + "\\" + native) && !fileExists(e.model)) {
            std::string text = trf("Missing model: %s (%s)", e.model.c_str(), e.name.c_str());
            m_badges[e.id] = {true, text};
            if (first) m_problems.push_back({true, std::move(text), DataPage::Sounds, -1, e.id});
            continue;
        }
        const asset::Model* model = m_models ? m_models->get(e.model) : nullptr;
        if (!model) continue;
        for (const std::string& over : model->budgetProblems) {
            std::string text = trf("Over budget: %s: %s", e.model.c_str(), over.c_str());
            m_badges[e.id] = {false, text};
            if (first) m_problems.push_back({false, std::move(text), DataPage::Sounds, -1, e.id});
        }
    }

    // Required fields: tuning and scene objects
    std::vector<std::string> required;
    if (const reflect::View tuning = m_tuningPanel.tuning(); tuning.type) collectRequired(*tuning.type, tuning.data, "game.tuning: ", required);
    for (std::string& text : required) m_problems.push_back({true, std::move(text), DataPage::Tuning, 0, 0});
    for (const Entity& e : m_scene.entities) {
        const reflect::TypeInfo* type = entityType(e);
        if (!type || type->fields.empty()) continue;
        reflect::Instance instance(*type);
        reflect::fromText(*type, instance.data(), e.fields);
        required.clear();
        collectRequired(*type, instance.data(), e.name + ": ", required);
        for (std::string& text : required) {
            m_badges[e.id] = {true, text};
            m_problems.push_back({true, std::move(text), DataPage::Sounds, -1, e.id});
        }
    }

    required.clear();
    m_uiPanel.collectBindingProblems(required);
    for (const std::string& text : required) m_problems.push_back({false, trf("UI binding has no field: %s", text.c_str()), DataPage::Sounds, -1, 0});

    const data::TableIndexList& index = m_tablesPanel.index();
    if (index.empty()) return;
    std::vector<std::pair<int, std::string>> tableProblems;
    m_tablesPanel.collectProblems(tableProblems);
    for (auto& [doc, text] : tableProblems) m_problems.push_back({true, std::move(text), DataPage::Table, doc, 0});
    std::vector<std::string> found;
    m_tuningPanel.resolveRows(index, &found);
    for (std::string& text : found) m_problems.push_back({true, std::move(text), DataPage::Tuning, 0, 0});
    for (const Entity& e : m_scene.entities) {
        const reflect::TypeInfo* type = entityType(e);
        if (!type || type->fields.empty() || !m_gameTypes) continue;
        reflect::Instance instance(*type);
        reflect::fromText(*type, instance.data(), e.fields);
        found.clear();
        data::resolveRows(*type, instance.data(), index, &found, fileName(m_scenePath) + " " + e.name + ": ");
        for (std::string& text : found) {
            m_badges[e.id] = {true, text};
            m_problems.push_back({true, std::move(text), DataPage::Sounds, -1, e.id});
        }
    }
}

// gizmo=radius fields of the selected object: three circles and a handle at the camera's right. Dragging is one undo step
bool Editor::updateFieldHandles() {
    Entity* e = selected();
    const reflect::TypeInfo* type = e ? entityType(*e) : nullptr;
    if (!type) return false;
    const reflect::FieldInfo* field = nullptr;
    for (const reflect::FieldInfo& f : type->fields)
        if (f.value->kind == reflect::Kind::Float && splitFieldLabel(f.label).gizmo == "radius") field = &f;
    if (!field) return false;

    reflect::Instance instance(*type);
    reflect::fromText(*type, instance.data(), e->fields);
    f32& radius = *reinterpret_cast<f32*>(static_cast<u8*>(instance.data()) + field->offset);
    const Vec3 center = scene::worldTransform(m_scene, *e).position;
    const Mat4 viewProj = m_view * m_proj;
    const auto project = [&](Vec3 p, ImVec2* out) {
        const f32 cx = p.x * viewProj.m[0][0] + p.y * viewProj.m[1][0] + p.z * viewProj.m[2][0] + viewProj.m[3][0];
        const f32 cy = p.x * viewProj.m[0][1] + p.y * viewProj.m[1][1] + p.z * viewProj.m[2][1] + viewProj.m[3][1];
        const f32 cw = p.x * viewProj.m[0][3] + p.y * viewProj.m[1][3] + p.z * viewProj.m[2][3] + viewProj.m[3][3];
        if (cw <= 0.0f) return false;
        *out = ImVec2(m_viewport.x + (cx / cw * 0.5f + 0.5f) * m_viewport.width, m_viewport.y + (0.5f - cy / cw * 0.5f) * m_viewport.height);
        return true;
    };

    ImDrawList* draw = ImGui::GetBackgroundDrawList(); // over the scene view, under the editor windows
    const ImU32 line = ImGui::GetColorU32(ImVec4(1.0f, 0.85f, 0.45f, 0.8f));
    const Vec3 axes[3][2] = {{{1, 0, 0}, {0, 1, 0}}, {{1, 0, 0}, {0, 0, 1}}, {{0, 1, 0}, {0, 0, 1}}};
    for (const auto& axis : axes) {
        ImVec2 previous;
        bool hasPrevious = false;
        for (int i = 0; i <= 64; ++i) {
            const f32 a = static_cast<f32>(i) / 64.0f * 6.2831853f;
            ImVec2 now;
            const bool visible = project(center + (axis[0] * std::cos(a) + axis[1] * std::sin(a)) * radius, &now);
            if (visible && hasPrevious) draw->AddLine(previous, now, line, 1.5f);
            previous = now;
            hasPrevious = visible;
        }
    }
    const Vec3 forward = cameraForward();
    const Vec3 right = normalize(cross(Vec3{0, 1, 0}, forward));
    ImVec2 handle;
    if (!project(center + right * radius, &handle)) return false;
    const ImGuiIO& io = ImGui::GetIO();
    const f32 dx = io.MousePos.x - handle.x, dy = io.MousePos.y - handle.y;
    const bool dragging = m_handleEntity == e->id && m_handleField == field->name;
    const bool hovered = !sceneMouseBlocked() && !ImGuizmo::IsUsing() && dx * dx + dy * dy < 100.0f;
    draw->AddCircleFilled(handle, hovered || dragging ? 7.0f : 5.0f, ImGui::GetColorU32(ImVec4(1.0f, 0.85f, 0.45f, 1.0f)));
    if (hovered || dragging) ImGui::SetTooltip("%s: %.2f", trData(splitFieldLabel(field->label).name.c_str()), static_cast<double>(radius));

    if (!dragging && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        pushUndo(capture());
        m_handleEntity = e->id;
        m_handleField = field->name;
        return true;
    }
    if (!dragging) return hovered;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        m_handleEntity = 0;
        return true;
    }
    // The mouse on the plane through the center facing the camera
    Vec3 origin, dir;
    mouseRay(io.MousePos.x, io.MousePos.y, &origin, &dir);
    const f32 facing = dot(dir, forward);
    if (std::fabs(facing) > 0.001f) {
        const Vec3 hit = origin + dir * (dot(center - origin, forward) / facing);
        const f32 next = length(hit - center);
        if (next != radius) {
            radius = next;
            reflect::TextFields fields;
            reflect::toText(*type, instance.data(), fields);
            e->fields = std::move(fields);
        }
    }
    return true;
}

// Log page: what is wrong right now (kept current, gone once fixed) above what has happened (history). The flag in the filter row shows or hides the list
void Editor::drawLogPage() {
    bool anyError = false;
    for (const Problem& p : m_problems) anyError |= p.error;
    ImVec4 color = m_problems.empty() ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : anyError ? color::kDanger : color::kWarning;
    if (!m_problemsShown) color.w *= 0.5f;
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushStyleColor(ImGuiCol_Button, m_problemsShown ? ImGui::GetStyleColorVec4(ImGuiCol_FrameBg) : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    char label[64];
    std::snprintf(label, sizeof(label), "%s  %zu###problems", icon::kFlag, m_problems.size());
    if (ImGui::Button(label)) m_problemsShown = !m_problemsShown;
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tr("Problems"));
    ImGui::SameLine();
    m_logPanel.drawFilters();
    if (m_problemsShown && !m_problems.empty()) drawProblems();
    m_logPanel.drawLines();
}

void Editor::drawProblems() {
    const f32 row = ImGui::GetTextLineHeightWithSpacing();
    const f32 padding = ImGui::GetFontSize() * 0.3f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.0f, 1.0f, 1.0f, 0.03f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
    const bool open = ImGui::BeginChild("##problems", ImVec2(0.0f, row * std::min<f32>(static_cast<f32>(m_problems.size()), 5.0f) + padding * 2.0f), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_ResizeY);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (open) {
        for (size_t i = 0; i < m_problems.size(); ++i) {
            const Problem& p = m_problems[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::PushStyleColor(ImGuiCol_Text, p.error ? color::kDanger : color::kWarning);
            const bool clicked = ImGui::Selectable((std::string(p.error ? icon::kError : icon::kWarning) + "  " + p.text).c_str());
            ImGui::PopStyleColor();
            if (clicked) { // copied first: opening a page must not depend on the list staying the same
                const u32 entity = p.entity;
                const DataPage page = p.page;
                const int doc = p.doc;
                if (entity != 0) {
                    m_selected = entity;
                    focusInspector();
                } else if (doc >= 0) {
                    openDataPage(page, doc);
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

} // namespace mealkit::editor
