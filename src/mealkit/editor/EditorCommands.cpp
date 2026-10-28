// 에디터 명령 표와 명령 팔레트 (Ctrl+P). 메뉴·단축키·팔레트·F1 도움말이 이 표 하나를 쓴다.
#include "mealkit/editor/Editor.h"

#include "mealkit/core/Log.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"

#include <imgui.h>
#include <imgui_internal.h> // GetKeyChordName

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>

namespace mealkit::editor {
namespace {

// 대소문자를 무시한 부분 문자열 (한국어는 바이트 그대로 비교)
bool matches(const char* text, const char* needle) {
    if (!needle[0]) return true;
    const size_t n = strlen(needle);
    for (const char* p = text; *p; ++p)
        if (_strnicmp(p, needle, n) == 0) return true;
    return false;
}

} // namespace

void Editor::buildCommands() {
    const auto editing = [this] { return m_mode == Mode::Edit; };
    const auto hasSelection = [this] { return m_mode == Mode::Edit && m_selected != 0; };
    const auto canBuild = [this] { return !m_project.directory.empty() && !m_buildPanel.running(); };
    const auto always = [] { return true; };
    m_commands = {
        {"New Scene", ImGuiMod_Ctrl | ImGuiKey_N, 0, editing, [this] { newScene(); }},
        {"Open...", ImGuiMod_Ctrl | ImGuiKey_O, 0, editing, [this] { openSceneDialog(); }},
        {"Save", ImGuiMod_Ctrl | ImGuiKey_S, 0, editing, [this] { saveAll(); }},
        {"Save As...", 0, 0, editing, [this] { saveSceneAs(); }},
        {"Project Settings...", 0, 0, [this] { return m_mode == Mode::Edit && !m_project.directory.empty(); },
         [this] { m_projectPanel.open(); }},
        {"Undo", ImGuiMod_Ctrl | ImGuiKey_Z, 0, [this] { return m_mode == Mode::Edit && !m_undo.empty(); },
         [this] { undo(); }},
        {"Redo", ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z,
         [this] { return m_mode == Mode::Edit && !m_redo.empty(); }, [this] { redo(); }},
        {"Select All", ImGuiMod_Ctrl | ImGuiKey_A, 0, editing, [this] {
             std::vector<u32> ids;
             for (const scene::Entity& e : m_scene.entities)
                 if (!isHidden(e) && !isLocked(e)) ids.push_back(e.id);
             selectMany(ids, false);
         }},
        {"Duplicate", ImGuiMod_Ctrl | ImGuiKey_D, 0, hasSelection, [this] { duplicateSelected(); }},
        {"Delete", ImGuiKey_Delete, 0, hasSelection, [this] { deleteSelected(); }},
        {"Find in Scene Tree", ImGuiMod_Ctrl | ImGuiKey_F, 0, editing, [this] { m_treeSearchFocus = true; }},
        {"Drop to Floor", ImGuiKey_End, 0, hasSelection, [this] { dropToFloor(); }},
        {"Align X", 0, 0, [this] { return m_mode == Mode::Edit && !m_multi.empty(); }, [this] { alignSelection(0); }},
        {"Align Y", 0, 0, [this] { return m_mode == Mode::Edit && !m_multi.empty(); }, [this] { alignSelection(1); }},
        {"Align Z", 0, 0, [this] { return m_mode == Mode::Edit && !m_multi.empty(); }, [this] { alignSelection(2); }},
        {"Distribute Evenly", 0, 0, [this] { return m_mode == Mode::Edit && m_multi.size() >= 2; },
         [this] { distributeSelection(); }},
        {"Focus Selected", ImGuiKey_F, 0, hasSelection, [this] { focusSelected(); }},
        {"Move W", ImGuiKey_W, 0, editing, [this] { m_gizmoOp = 0; }},
        {"Rotate E", ImGuiKey_E, 0, editing, [this] { m_gizmoOp = 1; }},
        {"Scale R", ImGuiKey_R, 0, editing, [this] { m_gizmoOp = 2; }},
        {"Play", ImGuiKey_F5, 0, [this] { return m_mode == Mode::Edit && m_gameLoaded; },
         [this] { m_playRequest = true; }},
        {"Preferences...", 0, 0, always, [this] { *m_prefsPanel.openFlag() = true; }},
        {"Keyboard Shortcuts", ImGuiKey_F1, 0, always, [this] { m_shortcutsOpen = !m_shortcutsOpen; }},
        {"Command Palette", ImGuiMod_Ctrl | ImGuiKey_P, 0, always, [this] { m_paletteOpen = true; }},
        {"Tables", ImGuiMod_Ctrl | ImGuiKey_T, 0, always, [this] { m_tablesOpen = !m_tablesOpen; }},
        {"Sounds", 0, 0, [this] { return m_audio != nullptr; }, [this] { m_soundsOpen = !m_soundsOpen; }},
        {"Feedback", 0, 0, always, [this] { m_feedbackOpen = !m_feedbackOpen; }},
        {"UI", 0, 0, always, [this] { m_uiPanelOpen = !m_uiPanelOpen; }},
        {"Look Dev", 0, 0, always, [this] { m_lookdevOpen = !m_lookdevOpen; }},
        {"Reset Layout", 0, 0, always, [this] { m_layoutReset = true; }},
        {"Build Game Code (Hot Reload)", ImGuiMod_Ctrl | ImGuiKey_R, 0,
         [this, canBuild] { return canBuild() && m_mode == Mode::Edit; }, [this] { startBuild(BuildKind::GameCode); }},
        {"Release Build", ImGuiMod_Ctrl | ImGuiKey_B, 0, canBuild, [this] { startBuild(BuildKind::Export); }},
        {"Open Release Folder", 0, 0, [this] { return directoryExists(m_project.exportDirectory()); },
         [this] { openInExplorer(m_project.exportDirectory()); }},
        {"Open Project Folder", 0, 0, [this] { return !m_project.directory.empty(); },
         [this] { openInExplorer(m_project.directory); }},
        {"Build Log", 0, 0, always, [this] { *m_buildPanel.openFlag() = !*m_buildPanel.openFlag(); }},
        {"Log", 0, 0, always, [this] { *m_logPanel.openFlag() = !*m_logPanel.openFlag(); }},
    };
}

const Editor::Command* Editor::findCommand(const char* name) const {
    for (const Command& c : m_commands)
        if (strcmp(c.name, name) == 0) return &c;
    return nullptr;
}

std::string Editor::shortcutText(const Command& c) const {
    return c.chord ? ImGui::GetKeyChordName(c.chord) : std::string();
}

// 메뉴 줄: 이름·단축키·켜짐 여부를 명령 표에서 가져온다. selected가 있으면 체크 표시
void Editor::commandMenuItem(const char* name, const bool* selected) {
    const Command* c = findCommand(name);
    MEALKIT_ASSERT(c); // 메뉴에 쓴 이름이 명령 표에 있어야 한다
    if (!c) return;
    const std::string keys = shortcutText(*c);
    if (ImGui::MenuItem(tr(name), keys.empty() ? nullptr : keys.c_str(), selected ? *selected : false, c->enabled()))
        c->run();
}

void Editor::runShortcuts() {
    if (!m_queuedCommands.empty() && ImGui::GetFrameCount() > 2) { // --command: 씬·모델을 다 읽은 뒤
        for (const std::string& name : m_queuedCommands) {
            const Command* c = findCommand(name.c_str());
            if (c && c->enabled()) c->run();
            else MEALKIT_WARN("--command %s: unknown or not available now", name.c_str());
        }
        m_queuedCommands.clear();
    }
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || m_looking) return;
    for (const Command& c : m_commands) {
        const bool hit = (c.chord && ImGui::IsKeyChordPressed(c.chord)) || (c.chord2 && ImGui::IsKeyChordPressed(c.chord2));
        if (hit && c.enabled()) {
            c.run();
            return;
        }
    }
    if (m_mode == Mode::Edit && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_selected = 0;
        m_multi.clear();
    }
}

// F1: 명령 표의 단축키 목록
void Editor::drawShortcutHelp() {
    if (!m_shortcutsOpen) return;
    ImGui::SetNextWindowSize(ImVec2(420, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(trl("Keyboard Shortcuts###Shortcuts"), &m_shortcutsOpen, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        for (const Command& c : m_commands) {
            if (!c.chord) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(tr(c.name));
            ImGui::TableNextColumn();
            std::string keys = shortcutText(c);
            if (c.chord2) keys += std::string(", ") + ImGui::GetKeyChordName(c.chord2);
            ImGui::TextUnformatted(keys.c_str());
        }
        const char* fixed[][2] = {{"Fly the camera", "Right drag + WASD/QE"}, {"Pan the camera", "Middle drag"},
                                  {"Camera speed", "Wheel while flying"},     {"Snap while dragging a gizmo", "Ctrl"},
                                  {"Deselect", "Esc"},                         {"Stop playing", "Esc / F5"}};
        for (auto& row : fixed) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(tr(row[0]));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(tr(row[1]));
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void Editor::openPalette(const std::string& text) {
    m_paletteOpen = true;
    m_paletteInitial = text;
}

// Ctrl+P: 명령·물체·씬·도구를 이름(한국어·영어)으로 찾아 바로 실행한다
void Editor::drawCommandPalette() {
    if (m_paletteOpen && ImGui::GetFrameCount() > 2) { // 첫 프레임에 나타나는 창들이 포커스를 가져가 팝업을 닫는다
        ImGui::OpenPopup("##palette");
        m_paletteOpen = false;
        snprintf(m_paletteSearch, sizeof(m_paletteSearch), "%s", m_paletteInitial.c_str());
        m_paletteInitial.clear();
        m_paletteCursor = 0;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 60.0f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34.0f, 0.0f));
    if (!ImGui::BeginPopup("##palette")) return;

    struct Item {
        std::string label;
        std::string hint;
        std::function<void()> run;
    };
    std::vector<Item> items;
    const char* q = m_paletteSearch;
    for (const Command& c : m_commands) {
        if (c.name == std::string("Command Palette") || !c.enabled()) continue;
        if (matches(tr(c.name), q) || matches(c.name, q)) items.push_back({tr(c.name), shortcutText(c), c.run});
    }
    if (m_mode == Mode::Edit) {
        for (const scene::Entity& e : m_scene.entities) {
            if (!matches(e.name.c_str(), q) && !matches(e.type.c_str(), q)) continue;
            const u32 id = e.id;
            items.push_back({e.name, e.type, [this, id] {
                                 m_selected = id;
                                 focusSelected();
                             }});
        }
        const std::string sceneDir = m_project.directory.empty() ? std::string() : m_project.directory + "\\assets\\scenes";
        if (!sceneDir.empty())
            for (const std::string& f : listFiles(sceneDir, ".scene")) {
                if (!matches(f.c_str(), q)) continue;
                const std::string path = sceneDir + "\\" + f;
                items.push_back({f, tr("Scene"), [this, path] {
                                     saveBackupIfDirty();
                                     loadScene(path);
                                 }});
            }
    }
    for (const ToolInfo& t : m_tools)
        if (t.problem == ToolInfo::Problem::None && matches(t.name.c_str(), q))
            items.push_back({t.name, tr("Tools"), [this, &t] { launchTool(t, m_project.directory); }});

    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##q", tr("Type a command, object or scene"), m_paletteSearch,
                                 sizeof(m_paletteSearch)))
        m_paletteCursor = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) ++m_paletteCursor;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) --m_paletteCursor;
    m_paletteCursor = std::clamp(m_paletteCursor, 0, std::max(0, static_cast<int>(items.size()) - 1));
    const Item* chosen = nullptr;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && !items.empty()) chosen = &items[m_paletteCursor];

    const f32 rowH = ImGui::GetTextLineHeightWithSpacing();
    if (ImGui::BeginChild("##items", ImVec2(0, rowH * std::min<f32>(12.0f, std::max<f32>(1.0f, items.size())) + 4.0f))) {
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            const Item& it = items[i];
            ImGui::PushID(i);
            if (ImGui::Selectable(it.label.c_str(), i == m_paletteCursor)) chosen = &it;
            if (i == m_paletteCursor && (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)))
                ImGui::SetScrollHereY();
            if (!it.hint.empty()) {
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(it.hint.c_str()).x);
                ImGui::TextDisabled("%s", it.hint.c_str());
            }
            ImGui::PopID();
        }
        if (items.empty()) ImGui::TextDisabled("%s", tr("Nothing found"));
    }
    ImGui::EndChild();
    if (chosen) {
        const std::function<void()> run = chosen->run;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        run();
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace mealkit::editor
