// Editor command table and command palette (Ctrl+P). Menus, shortcuts, palette, and F1 help all use this one table.
#include "mealkit/editor/Editor.h"
#include "mealkit/editor/SharedBuild.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/FuzzyMatch.h"

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
        {"Settings...", 0, 0, always, [this] { m_settingsOpen = !m_settingsOpen; }},
        {"Project Settings...", 0, 0, [this] { return m_mode == Mode::Edit && !m_project.directory.empty(); },
         [this] { openSettingsPage(SettingsPage::Project); }},
        {"Modules", 0, 0, [this] { return !m_project.directory.empty(); }, [this] { m_modulesOpen = !m_modulesOpen; }},
        {"Undo", ImGuiMod_Ctrl | ImGuiKey_Z, 0,
         [this] { return m_dataFocused ? dataCanStep(true) : m_mode == Mode::Edit && (uiEditing() ? m_uiPanel.canUndo() : !m_undo.empty()); },
         [this] { m_dataFocused ? dataStep(true) : uiEditing() ? m_uiPanel.undo() : undo(); }},
        {"Redo", ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z,
         [this] { return m_dataFocused ? dataCanStep(false) : m_mode == Mode::Edit && (uiEditing() ? m_uiPanel.canRedo() : !m_redo.empty()); },
         [this] { m_dataFocused ? dataStep(false) : uiEditing() ? m_uiPanel.redo() : redo(); }},
        {"History", ImGuiMod_Ctrl | ImGuiKey_H, 0, always, [this] { m_historyOpen = !m_historyOpen; }},
        {"Select All", ImGuiMod_Ctrl | ImGuiKey_A, 0, editing, [this] {
             std::vector<u32> ids;
             for (const scene::Entity& e : m_scene.entities)
                 if (!isHidden(e) && !isLocked(e)) ids.push_back(e.id);
             selectMany(ids, false);
         }},
        {"Duplicate", ImGuiMod_Ctrl | ImGuiKey_D, 0, [this, hasSelection] { return uiEditing() || hasSelection(); },
         [this] { uiEditing() ? (void)m_uiPanel.duplicateSelected() : duplicateSelected(); }},
        {"Copy", ImGuiMod_Ctrl | ImGuiKey_C, 0, [this] { return uiEditing(); }, [this] { m_uiPanel.copySelected(); }},
        {"Paste", ImGuiMod_Ctrl | ImGuiKey_V, 0, [this] { return uiEditing(); }, [this] { m_uiPanel.paste(); }},
        {"Save as Prefab...", 0, 0, hasSelection, [this] { saveSelectionAsPrefab(); }},
        {"Delete", ImGuiKey_Delete, 0, [this, hasSelection] { return uiEditing() || hasSelection(); },
         [this] { uiEditing() ? (void)m_uiPanel.deleteSelected() : deleteSelected(); }},
        {"Find in Scene Tree", ImGuiMod_Ctrl | ImGuiKey_F, 0, editing, [this] {
             m_hierarchyOpen = true;
             m_treeSearchFocus = true;
         }},
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
        {"Preferences...", 0, 0, always, [this] { openSettingsPage(SettingsPage::Preferences); }},
        {"Keyboard Shortcuts", ImGuiKey_F1, 0, always, [this] { openSettingsPage(SettingsPage::Shortcuts); }},
        {"Command Palette", ImGuiMod_Ctrl | ImGuiKey_P, 0, always, [this] { m_paletteOpen = true; }},
        {"Data", 0, 0, always, [this] { m_dataOpen = !m_dataOpen; }},
        {"Debug", 0, 0, always, [this] { m_debugOpen = !m_debugOpen; }},
        {"Problems", 0, 0, always, [this] {
             openDebugPage(DebugPage::Log);
             m_problemsShown = true;
         }},
        {"State", 0, 0, always, [this] { toggleDebugPage(DebugPage::State); }},
        {"Tests", 0, 0, always, [this] { toggleDebugPage(DebugPage::Tests); }},
        {"Balance", 0, 0, always, [this] { toggleDebugPage(DebugPage::Balance); }},
        {"Run Bot Matches", 0, 0, always, [this] {
             openDebugPage(DebugPage::Balance);
             m_balancePanel.run(m_project);
         }},
        {"Run All Tests", 0, 0, always, [this] {
             openDebugPage(DebugPage::Tests);
             m_testsPanel.runAll(m_project);
         }},
        {"Tuning", 0, 0, always, [this] { toggleDataPage(DataPage::Tuning, 0); }},
        {"Tables", ImGuiMod_Ctrl | ImGuiKey_T, 0, always, [this] { toggleDataPage(DataPage::Table, 0); }},
        {"Sounds", 0, 0, [this] { return m_audio != nullptr; }, [this] { toggleDataPage(DataPage::Sounds, 0); }},
        {"Feedback", 0, 0, always, [this] { toggleDataPage(DataPage::Feedback, 0); }},
        {"Previews", 0, 0, always, [this] { m_previewOpen = !m_previewOpen; }},
        {"UI", 0, 0, always, [this] { openPreviewPage(PreviewPage::Ui); }},
        {"Look Dev", 0, 0, always, [this] { openPreviewPage(PreviewPage::LookDev); }},
        {"Preview Cutscene", 0, 0, always, [this] { openPreviewPage(PreviewPage::Cutscene); }},
        {"Effects", 0, 0, always, [this] { toggleDataPage(DataPage::Effects, 0); }},
        {"Reset Layout", 0, 0, always, [this] { m_layoutReset = true; }},
        {"Build Game Code (Hot Reload)", ImGuiMod_Ctrl | ImGuiKey_R, 0,
         [this, canBuild] { return canBuild() && m_mode == Mode::Edit; }, [this] { startBuild(BuildKind::GameCode); }},
        {"Publish Game Code", 0, 0, canBuild, [this] {
             std::string error;
             if (publishSharedBuild(m_project, error)) setStatus(tr("Game code published to the shared folder"));
             else setStatus(error, true);
         }},
        {"Get Latest Game Code", 0, 0, [this] { return m_mode == Mode::Edit && !m_project.directory.empty(); }, [this] {
             std::string error;
             if (fetchSharedBuild(m_project, &error)) setStatus(tr("Got new game code from the shared folder"));
             else if (!error.empty()) setStatus(error, true);
             else setStatus(tr("Game code is up to date"));
         }},
        {"Release Build", ImGuiMod_Ctrl | ImGuiKey_B, 0, canBuild, [this] { startBuild(BuildKind::Export); }},
        {"Open Release Folder", 0, 0, [this] { return directoryExists(m_project.exportDirectory()); },
         [this] { openInExplorer(m_project.exportDirectory()); }},
        {"Open Project Folder", 0, 0, [this] { return !m_project.directory.empty(); },
         [this] { openInExplorer(m_project.directory); }},
        {"Build Log", 0, 0, always, [this] { toggleDebugPage(DebugPage::Build); }},
        {"Assets", 0, 0, always, [this] { m_assetsOpen = !m_assetsOpen; }},
        {"Scene Tree", 0, 0, always, [this] { m_hierarchyOpen = !m_hierarchyOpen; }},
        {"Scene", 0, 0, editing, [this] { m_sceneOpen = !m_sceneOpen; }},
        {"Inspector", 0, 0, always, [this] { m_inspectorOpen = !m_inspectorOpen; }},
        {"Performance", 0, 0, always, [this] { toggleDebugPage(DebugPage::Performance); }},
        {"Memory", 0, 0, always, [this] { toggleDebugPage(DebugPage::Memory); }},
        {"Log", 0, 0, always, [this] { toggleDebugPage(DebugPage::Log); }},
    };
    tagCommandModules();
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
    if (c->module && !projectHas(c->module)) return; // the project doesn't have that module: no menu item
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
}

void Editor::openPalette(const std::string& text) {
    m_paletteOpen = true;
    m_paletteInitial = text;
}

// Ctrl+P: find commands, objects, scenes, tools, assets, table rows and tuning values by name (Korean/English, gaps and Korean initial consonants allowed)
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
        int score = 0;
    };
    std::vector<Item> items;
    const char* q = m_paletteSearch;
    const auto add = [&](std::string label, std::string hint, std::function<void()> run, int score) {
        if (score >= 0) items.push_back({std::move(label), std::move(hint), std::move(run), score});
    };
    if (ImGui::IsWindowAppearing()) m_paletteAssets = allAssetFiles();
    for (const Command& c : m_commands) {
        if (c.name == std::string("Command Palette") || !c.enabled()) continue;
        add(tr(c.name), shortcutText(c), c.run, std::max(fuzzyScore(tr(c.name), q), fuzzyScore(c.name, q)));
    }
    if (m_mode == Mode::Edit) {
        for (const scene::Entity& e : m_scene.entities) {
            const u32 id = e.id;
            add(e.name, e.type, [this, id] {
                m_selected = id;
                focusSelected();
            }, std::max(fuzzyScore(e.name, q), fuzzyScore(e.type, q)));
        }
        const std::string sceneDir = m_project.directory.empty() ? std::string() : m_project.directory + "\\assets\\scenes";
        if (!sceneDir.empty())
            for (const std::string& f : listFiles(sceneDir, ".scene")) {
                const std::string path = sceneDir + "\\" + f;
                add(f, tr("Scene"), [this, path] { openSceneFile(path); }, fuzzyScore(f, q));
            }
    }
    for (const ToolInfo& t : m_tools)
        if (t.problem == ToolInfo::Problem::None) add(t.name, tr("Tools"), [this, &t] { launchTool(t, m_project.directory); }, fuzzyScore(t.name, q));
    if (q[0]) { // long lists only once something is typed
        if (m_mode == Mode::Edit)
            for (const reflect::TypeInfo* t : allTypes()) {
                const std::string name = trData(t->label), kind = t->name;
                add(name, tr("Add"), [this, kind] { addEntity(kind); }, std::max(fuzzyScore(name, q), fuzzyScore(kind, q)));
            }
        for (const std::string& file : m_paletteAssets)
            add(file, tr("Asset"), [this, file] { revealAsset(file); }, fuzzyScore(file, q));
        for (int d = 0; d < m_tablesPanel.docCount(); ++d) {
            const std::vector<std::string>& ids = m_tablesPanel.docIds(d);
            for (int r = 0; r < static_cast<int>(ids.size()); ++r)
                add(ids[static_cast<size_t>(r)], m_tablesPanel.docName(d), [this, d, r] {
                    openDataPage(DataPage::Table, d);
                    m_tablesPanel.selectRow(d, r);
                }, fuzzyScore(ids[static_cast<size_t>(r)], q));
        }
        if (const reflect::TypeInfo* tuning = m_tuningPanel.tuning().type)
            for (const reflect::FieldInfo& f : tuning->fields) {
                const std::string name = trData(splitFieldLabel(f.label).name.c_str());
                add(name, tr("Tuning"), [this] { openDataPage(DataPage::Tuning, 0); }, std::max(fuzzyScore(name, q), fuzzyScore(f.name, q)));
            }
    }
    // Best match first; with nothing typed, recently run items first
    if (!q[0])
        for (Item& it : items) {
            const auto recent = std::find(m_paletteRecent.begin(), m_paletteRecent.end(), it.label);
            if (recent != m_paletteRecent.end()) it.score = 1000 - static_cast<int>(recent - m_paletteRecent.begin());
        }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.score > b.score; });
    if (q[0] && !items.empty()) { // scattered letters only count when nothing matches better
        const int keep = items.front().score / 2;
        std::erase_if(items, [keep](const Item& it) { return it.score < keep; });
    }
    if (items.size() > 200) items.erase(items.begin() + 200, items.end());

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
        std::erase(m_paletteRecent, chosen->label);
        m_paletteRecent.insert(m_paletteRecent.begin(), chosen->label);
        if (m_paletteRecent.size() > 8) m_paletteRecent.pop_back();
        if (!m_project.directory.empty()) {
            std::string text;
            for (const std::string& line : m_paletteRecent) text += line + "\n";
            createDirectories(m_project.directory + "\\build\\editor");
            writeTextFile(m_project.directory + "\\build\\editor\\palette_recent.txt", text);
        }
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        run();
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace mealkit::editor
