// Editor command table and command palette (Ctrl+P). Menus, shortcuts, palette, and F1 help all use this one table.
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

// Case-insensitive substring (Korean compared byte for byte)
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
        {"Scenes", 0, 0, always, [this] { m_sceneListOpen = !m_sceneListOpen; }},
        {"Open Replay...", 0, 0, [this] { return m_mode == Mode::Edit && m_gameLoaded; },
         [this] { openReplayDialog(); }},
        {"Save As...", 0, 0, editing, [this] { saveSceneAs(); }},
        {"Project Settings...", 0, 0, [this] { return m_mode == Mode::Edit && !m_project.directory.empty(); },
         [this] { m_projectPanel.open(); }},
        {"Modules...", 0, 0, [this] { return !m_project.directory.empty(); }, [this] { m_modulesPanel.open(); }},
        {"Undo", ImGuiMod_Ctrl | ImGuiKey_Z, 0, [this] { return m_mode == Mode::Edit && !m_undo.empty(); },
         [this] { undo(); }},
        {"Redo", ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z,
         [this] { return m_mode == Mode::Edit && !m_redo.empty(); }, [this] { redo(); }},
        {"History", ImGuiMod_Ctrl | ImGuiKey_H, 0, always, [this] { m_historyOpen = !m_historyOpen; }},
        {"Select All", ImGuiMod_Ctrl | ImGuiKey_A, 0, editing, [this] {
             std::vector<u32> ids;
             for (const scene::Entity& e : m_scene.entities)
                 if (!isHidden(e) && !isLocked(e)) ids.push_back(e.id);
             selectMany(ids, false);
         }},
        {"Duplicate", ImGuiMod_Ctrl | ImGuiKey_D, 0, hasSelection, [this] { duplicateSelected(); }},
        {"Save as Prefab...", 0, 0, hasSelection, [this] { saveSelectionAsPrefab(); }},
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
        {"Play From Here", ImGuiMod_Shift | ImGuiKey_F5, 0,
         [this] { return m_mode == Mode::Edit && m_gameLoaded && m_playPlayers == 1; }, [this] {
             m_playFromHere = true;
             m_playRequest = true;
         }},
        {"Toggle Orthographic", ImGuiKey_Keypad5, 0, editing, [this] { m_ortho = !m_ortho; }},
        {"Play", ImGuiKey_F5, 0, [this] { return m_mode == Mode::Edit && m_gameLoaded; },
         [this] {
             m_playFromHere = false;
             m_playRequest = true;
         }},
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
        {"Assets", 0, 0, always, [this] { m_assetsOpen = !m_assetsOpen; }},
        {"Performance", 0, 0, always, [this] { *m_perfPanel.openFlag() = !*m_perfPanel.openFlag(); }},
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

// Menu item: name, shortcut, and enabled state come from the command table. Checkmark if selected is given
void Editor::commandMenuItem(const char* name, const bool* selected) {
    const Command* c = findCommand(name);
    MEALKIT_ASSERT(c); // names used in menus must be in the command table
    if (!c) return;
    const std::string keys = shortcutText(*c);
    if (ImGui::MenuItem(tr(name), keys.empty() ? nullptr : keys.c_str(), selected ? *selected : false, c->enabled()))
        c->run();
}

void Editor::runShortcuts() {
    if (!m_queuedCommands.empty() && ImGui::GetFrameCount() > 2) { // --command: after scenes/models are fully loaded
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
    handleBookmarks();
    if (m_mode == Mode::Edit && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_selected = 0;
        m_multi.clear();
    }
}

// F1: shortcut list from the command table
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
                                  {"Orbit the camera", "Alt + left drag"},     {"Go to camera bookmark", "1 - 9"},
                                  {"Save camera bookmark", "Ctrl + 1 - 9"},
                                  {"Camera speed", "Wheel while flying"},     {"Snap while dragging a gizmo", "Ctrl"},
                                  {"Deselect", "Esc"},                         {"Stop playing", "F5"}};
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

// Ctrl+P: find commands, objects, scenes, and tools by name (Korean/English) and run them right away
void Editor::drawCommandPalette() {
    if (m_paletteOpen && ImGui::GetFrameCount() > 2) { // windows appearing on the first frame take focus and close the popup
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
                items.push_back({f, tr("Scene"), [this, path] { openSceneFile(path); }});
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
