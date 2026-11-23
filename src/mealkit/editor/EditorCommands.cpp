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

std::string shortcutsPath() { return editorUserDirectory() + MEALKIT_SEP "editor_shortcuts.txt"; }

// "Ctrl+Shift+S" (the names ImGui::GetKeyChordName writes) -> chord. 0 if a part is not known
int parseChord(const std::string& text) {
    int chord = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('+', pos);
        if (end == std::string::npos) end = text.size();
        std::string part = text.substr(pos, end - pos);
        pos = end + 1;
        while (!part.empty() && part.front() == ' ') part.erase(0, 1);
        while (!part.empty() && part.back() == ' ') part.pop_back();
        if (part.empty()) continue;
        if (part == "Ctrl") chord |= ImGuiMod_Ctrl;
        else if (part == "Shift") chord |= ImGuiMod_Shift;
        else if (part == "Alt") chord |= ImGuiMod_Alt;
        else if (part == "Super") chord |= ImGuiMod_Super;
        else {
            ImGuiKey found = ImGuiKey_None;
            for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
                const ImGuiKey key = static_cast<ImGuiKey>(k);
                if (ImGui::IsGamepadKey(key) || ImGui::IsMouseKey(key) || ImGui::IsLRModKey(key)) continue;
                if (part == ImGui::GetKeyName(key)) found = key;
            }
            if (found == ImGuiKey_None) return 0;
            chord |= found;
        }
    }
    return (chord & ~ImGuiMod_Mask_) ? chord : 0; // a shortcut needs a key besides the modifiers
}

// The key pressed this frame as a chord (modifiers held + one key), 0 if none. Gamepad, mouse and bare modifier keys don't count
int pressedChord() {
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        const ImGuiKey key = static_cast<ImGuiKey>(k);
        if (ImGui::IsGamepadKey(key) || ImGui::IsMouseKey(key) || ImGui::IsLRModKey(key)) continue;
        if (ImGui::IsKeyPressed(key, false)) return static_cast<int>(ImGui::GetIO().KeyMods) | key;
    }
    return 0;
}

} // namespace

void Editor::buildCommands() {
    const auto editing = [this] { return m_mode == Mode::Edit; };
    const auto canBuild = [this] { return !m_project.directory.empty() && !m_buildPanel.running(); };
    const auto always = [] { return true; };
    m_commands = {
        {"Open...", ImGuiMod_Ctrl | ImGuiKey_O, 0, editing, [this] { openSceneDialog(); }},
        {"Save", ImGuiMod_Ctrl | ImGuiKey_S, 0, editing, [this] { saveAll(); }},
        {"Scenes", 0, 0, always, [this] { m_sceneListOpen = !m_sceneListOpen; }},
        {"Open Replay...", 0, 0, [this] { return m_mode == Mode::Edit && m_gameLoaded; },
         [this] { openReplayDialog(); }},
        {"Settings...", 0, 0, always, [this] { m_settingsOpen = !m_settingsOpen; }},
        {"Project Settings...", 0, 0, [this] { return m_mode == Mode::Edit && !m_project.directory.empty(); },
         [this] { openSettingsPage(SettingsPage::Project); }},
        {"Modules", 0, 0, [this] { return !m_project.directory.empty(); }, [this] { m_modulesOpen = !m_modulesOpen; }},
        {"Undo", ImGuiMod_Ctrl | ImGuiKey_Z, 0, [this] { return m_dataFocused && dataCanStep(true); }, [this] { dataStep(true); }},
        {"Redo", ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, [this] { return m_dataFocused && dataCanStep(false); },
         [this] { dataStep(false); }},
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
        {"UI", 0, 0, always, [this] { if (m_uiWorkspaceOpen && m_uiPanelOpen) m_uiWorkspaceOpen = false; else openUiPanel(); }},
        {"Look Dev", 0, 0, always, [this] { openPreviewPage(PreviewPage::LookDev); }},
        {"Preview Cutscene", 0, 0, always, [this] { openPreviewPage(PreviewPage::Cutscene); }},
        {"Effects", 0, 0, always, [this] { toggleDataPage(DataPage::Effects, 0); }},
        {"Reset Layout", 0, 0, always, [this] { m_layoutReset = true; }},
        {"Maximize Viewport", 0, 0, [this] { return m_sceneOpen; }, [this] { m_sceneMaximized = !m_sceneMaximized; }},
        {"Open MkUI", 0, 0, [this] { return !m_project.directory.empty(); }, [this] {
             if (!openInDesigner({})) setStatus(tr("MkUI is not installed (see [Settings] > Tools)"), true);
         }},
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
        {"Viewport", 0, 0, editing, [this] { m_sceneOpen = !m_sceneOpen; }},
        {"Performance", 0, 0, always, [this] { toggleDebugPage(DebugPage::Performance); }},
        {"Memory", 0, 0, always, [this] { toggleDebugPage(DebugPage::Memory); }},
        {"Log", 0, 0, always, [this] { toggleDebugPage(DebugPage::Log); }},
    };
    for (Command& c : m_commands) {
        c.defaultChord = c.chord;
        c.defaultChord2 = c.chord2;
    }
    tagCommandModules();
    loadShortcutOverrides();
}

void Editor::loadShortcutOverrides() {
    std::string text;
    if (!readTextFile(shortcutsPath(), text)) return;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find(" =");
        if (eq == std::string::npos) continue;
        const std::string name = line.substr(0, eq);
        std::string keys = line.substr(eq + 2);
        while (!keys.empty() && keys.front() == ' ') keys.erase(0, 1);
        const int chord = keys.empty() ? 0 : parseChord(keys);
        bool known = false;
        for (Command& c : m_commands)
            if (name == c.name) {
                known = true;
                if (keys.empty() || chord) {
                    c.chord = chord;
                    c.chord2 = 0;
                } else {
                    MEALKIT_WARN("editor_shortcuts.txt: unknown keys \"%s\" for %s", keys.c_str(), name.c_str());
                }
            }
        if (!known) MEALKIT_WARN("editor_shortcuts.txt: unknown command \"%s\"", name.c_str());
    }
}

void Editor::saveShortcutOverrides() const {
    std::string out = "# MealKit editor shortcuts: <command> = <keys>, empty = no shortcut. Edited in [Settings] > Keyboard Shortcuts\n";
    for (const Command& c : m_commands)
        if (c.chord != c.defaultChord || c.chord2 != c.defaultChord2) out += std::string(c.name) + " = " + shortcutText(c) + "\n";
    createDirectories(editorUserDirectory());
    if (!writeTextFile(shortcutsPath(), out)) MEALKIT_WARN("Could not write %s", shortcutsPath().c_str());
}

// Gives the command at index this key as its only shortcut (0 clears it). The caller has dealt with a command that used the key
void Editor::setCommandChord(size_t index, int chord) {
    m_commands[index].chord = chord;
    m_commands[index].chord2 = 0;
    saveShortcutOverrides();
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
    // --command: after scenes/models are fully loaded, one per frame (like a person pressing keys: panels draw in between)
    if (!m_queuedCommands.empty() && ImGui::GetFrameCount() > 2 + static_cast<int>(m_commandsRun)) {
        const std::string name = m_queuedCommands.front();
        m_queuedCommands.erase(m_queuedCommands.begin());
        ++m_commandsRun;
        const Command* c = findCommand(name.c_str());
        if (c && c->enabled()) {
            MEALKIT_INFO("--command %s", name.c_str());
            c->run();
        } else {
            MEALKIT_WARN("--command %s: unknown or not available now", name.c_str());
        }
    }
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || m_looking || m_shortcutRecording >= 0) return; // while a key is being picked for a command, pressing it must not run it
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

// F1 / [Settings] > Keyboard Shortcuts: every command with a key, or that can have one. Click a key, press the new one (Backspace clears, Esc cancels)
void Editor::drawShortcutHelp() {
    ImGui::TextDisabled("%s", tr("Click a shortcut, then press the new keys. Backspace removes it, Esc cancels."));
    // A key press while one is being picked
    if (m_shortcutRecording >= 0) {
        const int pressed = pressedChord();
        const size_t index = static_cast<size_t>(m_shortcutRecording);
        if (pressed == ImGuiKey_Escape) {
            m_shortcutRecording = -1;
        } else if (pressed != 0) {
            m_shortcutRecording = -1;
            const int chord = pressed == ImGuiKey_Backspace ? 0 : pressed;
            int other = -1;
            for (size_t k = 0; k < m_commands.size() && chord; ++k)
                if (k != index && (m_commands[k].chord == chord || m_commands[k].chord2 == chord)) other = static_cast<int>(k);
            if (other >= 0) {
                m_shortcutPendingIndex = static_cast<int>(index);
                m_shortcutPendingChord = chord;
                m_shortcutPendingOther = other;
                ImGui::OpenPopup("##shortcutTaken");
            } else {
                setCommandChord(index, chord);
            }
        }
    }
    if (ImGui::BeginPopupModal("##shortcutTaken", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const Command& taker = m_commands[static_cast<size_t>(m_shortcutPendingIndex)];
        Command& owner = m_commands[static_cast<size_t>(m_shortcutPendingOther)];
        ImGui::TextUnformatted(trf("%s is used by %s. Use it for %s instead?", ImGui::GetKeyChordName(m_shortcutPendingChord), tr(owner.name), tr(taker.name)).c_str());
        if (ImGui::Button(tr("OK"))) {
            if (owner.chord == m_shortcutPendingChord) {
                owner.chord = owner.chord2 == m_shortcutPendingChord ? 0 : owner.chord2;
                owner.chord2 = 0;
            } else {
                owner.chord2 = 0;
            }
            setCommandChord(static_cast<size_t>(m_shortcutPendingIndex), m_shortcutPendingChord);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginTable("##keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##keys", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 14.0f);
        ImGui::TableSetupColumn("##reset", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.5f);
        for (size_t i = 0; i < m_commands.size(); ++i) {
            Command& c = m_commands[i];
            if (c.module && !projectHas(c.module)) continue;
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(tr(c.name));
            ImGui::TableNextColumn();
            const bool recording = m_shortcutRecording == static_cast<int>(i);
            std::string keys = shortcutText(c);
            if (c.chord2) keys += std::string(", ") + ImGui::GetKeyChordName(c.chord2);
            if (keys.empty()) keys = "-";
            if (ImGui::Selectable((recording ? std::string(tr("Press a key...")) : keys).c_str(), recording)) m_shortcutRecording = recording ? -1 : static_cast<int>(i);
            ImGui::TableNextColumn();
            if (c.chord != c.defaultChord || c.chord2 != c.defaultChord2) {
                if (iconButton(icon::kUndo, tr("Reset to the default shortcut"))) {
                    c.chord = c.defaultChord;
                    c.chord2 = c.defaultChord2;
                    saveShortcutOverrides();
                }
            }
            ImGui::PopID();
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
        const std::string sceneDir = m_project.directory.empty() ? std::string() : m_project.directory + MEALKIT_SEP "assets" MEALKIT_SEP "scenes";
        if (!sceneDir.empty())
            for (const std::string& f : listFiles(sceneDir, ".scene")) {
                const std::string path = sceneDir + MEALKIT_SEP + f;
                add(f, tr("Scene"), [this, path] { openSceneFile(path); }, fuzzyScore(f, q));
            }
    }
    for (const ToolInfo& t : m_tools)
        if (t.problem == ToolInfo::Problem::None) add(t.name, tr("Tools"), [this, &t] { launchTool(t, m_project.directory); }, fuzzyScore(t.name, q));
    if (q[0]) { // long lists only once something is typed
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
            createDirectories(m_project.directory + MEALKIT_SEP "build" MEALKIT_SEP "editor");
            writeTextFile(m_project.directory + MEALKIT_SEP "build" MEALKIT_SEP "editor" MEALKIT_SEP "palette_recent.txt", text);
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
