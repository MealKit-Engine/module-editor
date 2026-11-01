#include "mealkit/editor/Editor.h"

#include "mealkit/core/Log.h"
#include "mealkit/core/Profile.h"
#include "mealkit/core/Version.h"
#include "mealkit/editor/CodeLink.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/game/InputRecord.h"
#include "mealkit/platform/FileDialog.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Process.h"
#include "mealkit/platform/Window.h"
#include "mealkit/render/SceneLights.h"
#include "mealkit/rhi/GpuCapture.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>


namespace mealkit::editor {
namespace {

using scene::Entity;

constexpr f32 kFovY = 60.0f * 3.14159265f / 180.0f;
constexpr const char* kHierarchyWindow = "Scene Tree###Hierarchy";
constexpr const char* kInspectorWindow = "Inspector###Inspector";
constexpr const char* kStateWindow = "State###State";
// Windows docked as tabs in the bottom row (default layout)
const ImVec4 kPlayColor(0.42f, 0.24f, 0.08f, 1.0f); // menu bar during play
const char* const kBottomWindows[] = {LogPanel::kWindow, "Assets###Assets", BuildPanel::kWindow, TablesPanel::kWindow};

// Case-insensitive substring (scene tree search)
bool containsNoCase(const std::string& text, const char* needle) {
    const size_t n = strlen(needle);
    for (size_t i = 0; i + n <= text.size(); ++i)
        if (_strnicmp(text.data() + i, needle, n) == 0) return true;
    return false;
}

constexpr size_t kUndoLimit = 100;
const Vec3 kSpawnSize{0.8f, 1.8f, 0.8f};

constexpr const char* kEntityPayload = "MEALKIT_ENTITY";
constexpr f32 kAreaEdgeDraw = 0.0025f; // area edge thickness (fraction of camera distance, so it's visible from afar)
constexpr f32 kAreaEdgePick = 0.6f;   // clickable thickness of area edges (m)

// The 12 edges of an area box: center and half size in object local (scaled) space
void areaEdges(Vec3 half, f32 thickness, Vec3 centers[12], Vec3 halves[12]) {
    const f32 t = thickness * 0.5f;
    int n = 0;
    for (int a = -1; a <= 1; a += 2)
        for (int b = -1; b <= 1; b += 2) {
            const f32 fa = static_cast<f32>(a), fb = static_cast<f32>(b);
            centers[n] = {0.0f, fa * half.y, fb * half.z}, halves[n++] = {half.x + t, t, t}; // x direction
            centers[n] = {fa * half.x, 0.0f, fb * half.z}, halves[n++] = {t, half.y + t, t}; // y direction
            centers[n] = {fa * half.x, fb * half.y, 0.0f}, halves[n++] = {t, t, half.z + t}; // z direction
        }
}

// Row vector convention TRS matrix (same memory layout as ImGuizmo: translation at [12..14])
void compose(const scene::Transform& e, f32 out[16]) {
    const Vec3 x = rotate(e.rotation, {1, 0, 0}) * e.scale.x;
    const Vec3 y = rotate(e.rotation, {0, 1, 0}) * e.scale.y;
    const Vec3 z = rotate(e.rotation, {0, 0, 1}) * e.scale.z;
    const f32 m[16] = {x.x, x.y, x.z, 0, y.x,          y.y,          y.z,          0,
                       z.x, z.y, z.z, 0, e.position.x, e.position.y, e.position.z, 1};
    std::memcpy(out, m, sizeof(m));
}

void decompose(const f32 m[16], scene::Transform& e) {
    Vec3 x{m[0], m[1], m[2]}, y{m[4], m[5], m[6]}, z{m[8], m[9], m[10]};
    const f32 sx = std::max(length(x), 0.01f), sy = std::max(length(y), 0.01f), sz = std::max(length(z), 0.01f);
    x = x / sx;
    y = y / sy;
    z = z / sz;
    // Element Rij of rotation matrix R (column convention) = i-th component of (row j)
    const f32 r00 = x.x, r11 = y.y, r22 = z.z;
    const f32 r21 = y.z, r12 = z.y, r02 = z.x, r20 = x.z, r10 = x.y, r01 = y.x;
    Quat q;
    const f32 trace = r00 + r11 + r22;
    if (trace > 0.0f) {
        const f32 s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25f * s};
    } else if (r00 > r11 && r00 > r22) {
        const f32 s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
        q = {0.25f * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s};
    } else if (r11 > r22) {
        const f32 s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
        q = {(r01 + r10) / s, 0.25f * s, (r12 + r21) / s, (r02 - r20) / s};
    } else {
        const f32 s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
        q = {(r02 + r20) / s, (r12 + r21) / s, 0.25f * s, (r10 - r01) / s};
    }
    e.position = {m[12], m[13], m[14]};
    e.scale = {sx, sy, sz};
    e.rotation = normalize(q);
}

// Intersection distance of a ray and a rotated box (center ± half in entity local space). Negative if it misses.
f32 rayBox(Vec3 origin, Vec3 dir, const scene::Transform& e, Vec3 center, Vec3 half) {
    const Quat inv{-e.rotation.x, -e.rotation.y, -e.rotation.z, e.rotation.w};
    const Vec3 o = rotate(inv, origin - e.position) - center;
    const Vec3 d = rotate(inv, dir);
    const f32 oa[3] = {o.x, o.y, o.z}, da[3] = {d.x, d.y, d.z}, ha[3] = {half.x, half.y, half.z};
    f32 tMin = -1e30f, tMax = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(da[i]) < 1e-8f) {
            if (oa[i] < -ha[i] || oa[i] > ha[i]) return -1.0f;
            continue;
        }
        f32 t1 = (-ha[i] - oa[i]) / da[i], t2 = (ha[i] - oa[i]) / da[i];
        if (t1 > t2) std::swap(t1, t2);
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        if (tMin > tMax) return -1.0f;
    }
    if (tMax < 0.0f) return -1.0f;
    return tMin >= 0.0f ? tMin : 0.0f;
}

void unpackColor(u32 c, f32 out[3]) {
    out[0] = static_cast<f32>(c & 0xFF) / 255.0f;
    out[1] = static_cast<f32>((c >> 8) & 0xFF) / 255.0f;
    out[2] = static_cast<f32>((c >> 16) & 0xFF) / 255.0f;
}

u32 packColor(const f32 c[3], u32 previous) {
    auto channel = [](f32 v) { return static_cast<u32>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return channel(c[0]) | (channel(c[1]) << 8) | (channel(c[2]) << 16) | (previous & 0xFF000000u);
}

const FileFilter kPrefabFilters[] = {{"MealKit prefab (*.prefab)", "*.prefab"}, {"All files (*.*)", "*.*"}};
const FileFilter kSceneFilters[] = {{"MealKit scene (*.scene)", "*.scene"}, {"All files (*.*)", "*.*"}};
const FileFilter kReplayFilters[] = {{"MealKit replay, bug report (*.rec, *.zip)", "*.rec;*.zip"}, {"All files (*.*)", "*.*"}};
const FileFilter kModelFilters[] = {{"glTF model (*.glb, *.gltf)", "*.glb;*.gltf"}, {"All files (*.*)", "*.*"}};

bool toolButton(const char* label, bool active) {
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor();
    return pressed;
}

// Path relative to root ('/' separated) if the file is inside the root folder, empty string if outside
std::string relativeTo(const std::string& root, const std::string& path) {
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        return s;
    };
    const std::string prefix = root + "\\";
    if (lower(path).rfind(lower(prefix), 0) != 0) return {};
    std::string relative = path.substr(prefix.size());
    std::replace(relative.begin(), relative.end(), '\\', '/');
    return relative;
}

} // namespace

bool Editor::init(Window& window, render::Renderer& renderer, const std::string& scenePath, asset::ModelCache& models) {
    buildCommands();
    m_prefs = loadPreferences();
    m_cameraSpeed = m_prefs.cameraSpeed;
    m_window = &window;
    m_renderer = &renderer;
    m_models = &models;
    focusPanel(kInspectorWindow); // starts in edit mode: bring the inspector to front
    if (!m_imgui.init(window, renderer, executableDirectory() + "\\mealkit_editor.ini")) {
        MEALKIT_ERROR("Editor UI initialization failed");
        return false;
    }
    if (fileExists(scenePath)) {
        if (!loadScene(scenePath)) {
            m_scene = scene::makeDefault();
            m_scenePath.clear(); // don't overwrite a broken file
        }
    } else {
        m_scene = scene::makeDefault();
        m_scenePath = scenePath;
        if (scene::save(m_scene, m_scenePath)) setStatus(trf("Created a default scene: %s", fileName(m_scenePath).c_str()));
    }
    return true;
}

void Editor::shutdown() { m_imgui.shutdown(); }

void Editor::update(f64 dt) {
    m_animTime += dt;
    MEALKIT_ZONE("Editor UI");
    m_time += dt;
    if (m_perf.update(dt, {m_renderer})) {
        if (m_live.listening("stats")) m_live.publish("stats", m_perf.json());
        game::FrameSample sample = m_perf.sample();
        if (m_mode == Mode::Play) { // simulation/network belong to the play session
            const game::FrameSample& p = m_playSample;
            sample.hasGame = p.hasGame;
            sample.simMsPerTick = p.simMsPerTick;
            sample.tick = p.tick;
            sample.hasSession = p.hasSession;
            sample.sentBytesPerSec = p.sentBytesPerSec;
            sample.recvBytesPerSec = p.recvBytesPerSec;
            sample.rttMs = p.rttMs;
        }
        m_perfPanel.push(sample);
    }

    m_live.update();
    for (const std::string& e : m_import.takeEvents()) {
        setStatus(e);
        m_uiPanel.refresh(); // new .ui files (ui::System rereads modified files)
    }
    updateHotReload(dt);

    m_imgui.newFrame();
    ImGuizmo::BeginFrame();
    if (!m_editActive) m_pending = capture();
    m_changed = false;

    ImGui::GetStyle().FontScaleMain = m_prefs.uiScale;
    m_buildPanel.setCodeEditor(m_prefs.externalEditor);
    m_logPanel.setCodeEditor(m_prefs.externalEditor);
    if (m_selected != m_multiOwner) {
        m_multi.clear();
        m_multiOwner = m_selected;
    }
    drawMenuBar();
    drawStatusBar();

    const ImGuiID dockId = ImGui::GetID("MealKitDockSpace");
    if (m_layoutReset || !ImGui::DockBuilderGetNode(dockId)) {
        setupDefaultLayout(dockId, m_layoutPreset);
        m_layoutReset = false;
    }
    ImGui::DockSpaceOverViewport(dockId, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
    if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockId)) {
        m_viewport = {central->Pos.x, central->Pos.y, central->Size.x, central->Size.y};
    } else {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        m_viewport = {0.0f, 0.0f, size.x, size.y};
    }

    updateCamera(dt);
    const f32 aspect = m_viewport.width / std::max(m_viewport.height, 1.0f);
    m_proj = projection(aspect);
    m_view = lookAtLH(m_cameraPos, m_cameraPos + cameraForward(), {0, 1, 0});
    updateCameraPreview(dt, aspect); // looking through a virtual camera / cutscene: replaces m_view and m_proj

    const bool editing = m_mode == Mode::Edit;
    drawHierarchy(editing);
    drawInspector(editing);
    m_tuningPanel.draw(kInspectorWindow, m_mode == Mode::Play, m_playPlayers > 1, m_tablesPanel.index());
    if (m_tablesOpen) m_tablesPanel.draw(m_window->nativeHandle(), editing, &m_tablesOpen);
    if (m_soundsOpen && m_audio) m_soundPanel.draw(*m_audio, &m_soundsOpen);
    if (m_feedbackOpen) m_feedbackPanel.draw(m_feedback, m_cameraPos + cameraForward() * 10.0f, dt, &m_feedbackOpen);
    if (m_uiPanelOpen) m_uiPanel.draw(&m_uiPanelOpen);
    if (m_lookdevOpen && m_renderer) m_lookdev.draw(*m_renderer, &m_lookdevOpen);
    // Editing the sound event table (view-only) saves it so changes are heard immediately (audio rereads the file)
    m_viewTableDirty |= m_tablesPanel.takeViewTableChanged();
    m_viewTableTimer += dt;
    if (m_viewTableDirty && m_viewTableTimer >= 0.3) {
        std::string status;
        m_tablesPanel.saveAll(&status);
        m_viewTableDirty = false;
        m_viewTableTimer = 0.0;
    }
    updateRowProblems(dt);
    m_projectPanel.draw(m_project, relativeTo(m_project.directory + "\\assets", m_scenePath));
    m_modulesPanel.draw(m_project, m_engineDirectory, m_mode == Mode::Edit);
    // Module sim/ and view/ are game code: adding or changing them rebuilds the game DLL
    if (m_modulesPanel.takeChanged()) startBuild(BuildKind::GameCode);
    drawState();
    pollBuild();
    pollLayoutCheck();
    drawBuildLog();
    m_logPanel.draw();
    m_perfPanel.draw();
    takePanelStatus();
    drawNotices();
    m_prefsPanel.draw(m_prefs);
    drawHistory();
    drawSceneList();
    drawAssetBrowser();
    drawShortcutHelp();
    drawPlayFrame();
    drawEmptyViewport();
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false) && rhi::gpuCaptureTool()) captureGpuFrame(); // both edit and play

    runShortcuts();
    drawCommandPalette();
    if (editing) {
        drawGizmo();
        drawViewportToolbar();
        handleAssetDrop();
        handlePicking();
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        m_stopRequest = true;
    } else if (m_simDebug && m_canPause && m_simDebug->timeline().recording) {
        if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) m_simDebug->setPaused(!m_simDebug->timeline().paused);
        if (ImGui::IsKeyPressed(ImGuiKey_F7)) m_simDebug->stepOnce();
    }
    reportSimProblems();

    // Undo: pushes the state at the moment editing started (captured at frame start) only once. Not pushed while dragging.
    if (m_changed && !m_editActive) {
        pushUndo(std::move(m_pending));
        m_editActive = true;
    }
    if (m_editActive && !ImGui::IsAnyItemActive() && !ImGuizmo::IsUsing()) m_editActive = false;

    drawPopups();
    updateAutosave(dt);
    updateTitle();
}

void Editor::setSimDebug(game::SimDebug* debug, bool canPause) {
    if (!debug) m_shadowReported = m_replayReported = false;
    m_simDebug = debug;
    m_canPause = canPause;
}

// Menu bar during play: pause, single tick, timeline (recorded range), shadow simulation status
void Editor::drawPlayControls() {
    if (!m_simDebug) return;
    const game::Timeline t = m_simDebug->timeline();
    ImGui::TextDisabled("|");
    if (t.recording && m_canPause) {
        if (ImGui::Button(t.paused ? tr("Resume (F6)") : tr("Pause (F6)"))) m_simDebug->setPaused(!t.paused);
        ImGui::BeginDisabled(!t.paused);
        if (ImGui::Button(tr("Step (F7)"))) m_simDebug->stepOnce();
        int view = static_cast<int>(t.view);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        if (ImGui::SliderInt("##timeline", &view, static_cast<int>(t.first), static_cast<int>(t.live),
                             t.viewingPast ? tr("Past tick %d") : tr("Tick %d")))
            m_simDebug->seek(static_cast<u32>(view));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(
                tr("Drag while paused to rewind to that tick (ticks %u-%u, record %.1f MB).\nResume returns to the present."),
                t.first, t.live, static_cast<f64>(t.memoryBytes) / (1024.0 * 1024.0));
    }
    // Solo play writes the input recording as it goes (user folder replays/current.rec): it becomes a scenario test
    const bool canSaveScenario = !t.viewingPast && m_stateView.type && m_stateView.data && t.live > 0 &&
                                 fileExists(game::replayRecordingPath(game::userFolder(m_project.settings)));
    ImGui::BeginDisabled(!canSaveScenario);
    if (ImGui::Button(tr("Save Scenario"))) saveScenario(t.live - 1);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tr("Saves the inputs so far and the current state values as a scenario test in tests/ "
                                   "(solo play with replays kept, not while viewing the past).\n"
                                   "Keep the checks that state the rule and delete the rest."));
    const game::ShadowStatus& shadow = m_simDebug->shadow();
    if (shadow.enabled) {
        if (shadow.desynced)
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), tr("Shadow: diverged at tick %u"), shadow.tick);
        else ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s", tr("Shadow: in sync"));
    }
}

void Editor::reportSimProblems() {
    if (!m_simDebug) return;
    const game::ShadowStatus& shadow = m_simDebug->shadow();
    if (shadow.desynced && !m_shadowReported) {
        m_shadowReported = true;
        std::string text = trf("The shadow simulation diverged at tick %u. Same inputs gave different results "
                               "(uninitialized values, global state, pointer order, reading real time, ...).",
                               shadow.tick) +
                           "\n\n";
        if (shadow.diff.empty()) text += tr("State fields match; only the physics state differs.");
        for (const std::string& line : shadow.diff) text += line + "\n";
        text += std::string("\n") + tr("The simulation stopped at that tick. See the values in the [State] panel.");
        showMessage(text);
    }
    if (!m_simDebug->replayError().empty() && !m_replayReported) {
        m_replayReported = true;
        showMessage(m_simDebug->replayError());
    }
}

void Editor::focusPanel(const char* window) {
    m_focusPanel = window;
    m_focusFrames = 3; // focus doesn't take on the first frame when the docking node doesn't exist yet, so request it for a few frames
}

void Editor::focusPanelNow() {
    ImGui::SetNextWindowFocus();
    if (--m_focusFrames <= 0) m_focusPanel = nullptr;
}

// Asset hot reload: rereads models exported by external tools (Blender etc.) and scenes edited outside. Not done during play
// (a model's collision hull touches the simulation, so changing it mid-match would split peers). Changes are read when play ends
void Editor::updateHotReload(f64 dt) {
    constexpr f64 kHotReloadSeconds = 0.5; // interval for checking file times
    m_hotReloadTimer -= dt;
    if (m_hotReloadTimer > 0.0 || m_mode != Mode::Edit) return;
    m_hotReloadTimer = kHotReloadSeconds;

    // Lighting model/toon ramp changed in the project settings window, and ramp images edited outside
    applyRenderStyle(m_project.settings);

    // Asset budgets changed in the project settings window (saving changes m_project)
    asset::ModelBudget budgets[asset::kModelKindCount];
    game::modelBudgets(m_project.settings, budgets);
    for (u32 k = 0; k < asset::kModelKindCount; ++k) {
        const asset::ModelBudget& now = m_models->budget(static_cast<asset::ModelKind>(k));
        if (std::memcmp(&now, &budgets[k], sizeof(now)) != 0) {
            m_models->setBudgets(budgets);
            break;
        }
    }
    m_models->setProbeLimit(m_project.settings.probeMaxCount);

    for (const std::string& path : m_models->reloadChanged()) {
        MEALKIT_INFO("Hot reload: model %s", path.c_str());
        setStatus(trf("Reloaded model: %s", path.c_str()));
    }

    if (m_scenePath.empty()) return;
    const unsigned long long time = fileWriteTime(m_scenePath);
    if (time == m_sceneTime || time == 0) return;
    if (m_dirty) { // don't overwrite if there are edits (saving overwrites the disk side)
        if (!m_sceneChangedWarned) setStatus(tr("The scene file changed on disk; you have unsaved changes"));
        m_sceneChangedWarned = true;
        return;
    }
    const u32 selected = m_selected;
    if (loadScene(m_scenePath)) {
        if (m_scene.find(selected)) m_selected = selected; // keep the selection if the same id exists
        MEALKIT_INFO("Hot reload: scene %s", m_scenePath.c_str());
        setStatus(trf("Reloaded scene: %s", fileName(m_scenePath).c_str()));
    }
}

void Editor::updateAutosave(f64 dt) {
    if (m_changed) m_autosavePending = true;
    if (m_mode != Mode::Edit) return;
    m_autosaveTimer += dt;
    // With unsaved changes, write to <scene>.autosave at the preference interval (0 = off)
    if (m_prefs.autosaveSeconds == 0 || m_autosaveTimer < m_prefs.autosaveSeconds) return;
    m_autosaveTimer = 0.0;
    if (m_dirty && m_autosavePending) {
        saveBackupIfDirty();
        m_autosavePending = false;
    }
}

void Editor::drawPopups() {
    if (!m_message.empty() && !ImGui::IsPopupOpen(trl("Notice###Message"))) ImGui::OpenPopup(trl("Notice###Message"));
    if (ImGui::BeginPopupModal(trl("Notice###Message"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
        ImGui::TextUnformatted(m_message.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button(tr("OK"), ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            m_message.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (m_upgradeOffer && m_message.empty() && !ImGui::IsPopupOpen(trl("Project Upgrade###Upgrade")))
        ImGui::OpenPopup(trl("Project Upgrade###Upgrade"));
    if (ImGui::BeginPopupModal(trl("Project Upgrade###Upgrade"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(tr("This project's build files were made from the engine %s template (engine is now %s)."),
                    m_project.settings.engineVersion.empty() ? tr("0.3 or older") : m_project.settings.engineVersion.c_str(),
                    kEngineVersion);
        ImGui::TextUnformatted(tr("Replace the template files (build.bat, CMakeLists.txt, CLAUDE.md, .vscode, .clang-format, ...) with the new version?"));
        ImGui::TextDisabled("%s", tr("Files you edited are not overwritten; the new content is left as <file>.new. Old files are backed up in .upgrade_backup."));
        ImGui::TextDisabled("%s", tr("source/ and assets/ are unchanged. If the game code no longer fits the engine API, fix it using the build log."));
        if (ImGui::Button(tr("Upgrade"), ImVec2(140, 0))) {
            m_upgradeRequested = true;
            m_upgradeOffer = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Later"), ImVec2(140, 0))) {
            m_upgradeOffer = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (m_upgradeRequested && !m_buildPanel.running()) {
        m_upgradeRequested = false;
        startBuild(BuildKind::Upgrade);
    }

    if (!m_recoverPath.empty() && !ImGui::IsPopupOpen(trl("Recover Autosave###Recover")))
        ImGui::OpenPopup(trl("Recover Autosave###Recover"));
    if (ImGui::BeginPopupModal(trl("Recover Autosave###Recover"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(tr("There are unsaved changes (the autosave is newer than the scene file)."));
        ImGui::TextDisabled("%s", m_recoverPath.c_str());
        ImGui::TextUnformatted(tr("Load the autosave? The scene file does not change until you save."));
        if (ImGui::Button(tr("Recover"), ImVec2(120, 0))) {
            scene::Scene recovered;
            std::string error;
            if (scene::load(recovered, m_recoverPath, &error)) {
                pushUndo(capture());
                m_scene = std::move(recovered);
                m_selected = 0;
                m_nameBufferId = 0;
                m_dirty = true;
                setStatus(tr("Autosave recovered - save to write it to the scene file"));
            } else {
                setStatus(trf("Could not read the autosave: %s", error.c_str()), true);
            }
            m_recoverPath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Discard"), ImVec2(120, 0))) {
            removeFile(m_recoverPath);
            m_recoverPath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Editor::renderUi(bool frameActive) { m_imgui.render(*m_renderer, frameActive); }

void Editor::fillEnvironment(render::FrameParams& frame) const {
    const scene::Environment& env = m_scene.environment;
    frame.sunDirection = env.sunDirection;
    frame.sunColor = env.sunColor;
    frame.skyColor = env.skyColor;
    frame.groundColor = env.groundColor;
    frame.fogColor = env.fogColor;
    frame.fogDensity = env.fogDensity;
    frame.environment = m_models->environment(env.hdri);
    frame.environmentIntensity = env.hdriIntensity;
    frame.environmentRotation = env.hdriRotation * (3.14159265f / 180.0f);
    frame.post = m_models->postParams(env.post);
    m_lightList.clear();
    render::addSceneLights(m_scene, m_lightList);
    frame.lights = m_lightList.data();
    frame.lightCount = static_cast<u32>(m_lightList.size());
}

u32 Editor::probeMaxCount() const { return std::clamp<u32>(m_project.settings.probeMaxCount, 1, render::kMaxReflectionProbes); }

void Editor::buildSceneView(render::FrameParams& frame, render::DrawList& out) const {
    frame.viewport = m_viewport;
    frame.viewProj = m_view * m_proj;
    frame.cameraPos = previewing() ? m_previewPose.position : m_cameraPos;
    fillEnvironment(frame);
    m_models->probes(m_scene, m_probeList);
    frame.probes = m_probeList.data();
    frame.probeCount = static_cast<u32>(m_probeList.size());
    frame.debugView = m_debugView;
    addSceneObjects(out, m_cameraPos, false);
    if (m_showGrid && m_mode == Mode::Edit && !m_automated) addGrid(out); // automated captures/golden images have no grid
}

void Editor::addSceneObjects(render::DrawList& out, Vec3 eye, bool gameView) const {
    const render::MeshId box = m_models->boxMesh();
    for (const Entity& e : m_scene.entities) {
        if (gameView && !scene::visibleInGame(e, m_gameTypes)) continue;
        if (!gameView && isHidden(e)) continue;
        const scene::Transform world = scene::worldTransform(m_scene, e);
        if (isArea(e)) { // invisible area: only edges, so the inside isn't hidden
            Vec3 centers[12], halves[12];
            const f32 thickness = std::max(0.05f, length(world.position - eye) * kAreaEdgeDraw);
            areaEdges(world.scale * 0.5f, thickness, centers, halves);
            for (int i = 0; i < 12; ++i) {
                render::InstanceData edge;
                edge.position = world.position + rotate(world.rotation, centers[i]);
                edge.rotation = world.rotation;
                edge.scale = halves[i] * 2.0f;
                edge.color = e.color;
                edge.flags = isSelected(e.id) ? render::kInstanceHighlight : 0u;
                out.add(box, edge);
            }
            continue;
        }
        render::InstanceData inst;
        inst.position = world.position;
        inst.rotation = world.rotation;
        inst.scale = isMarker(e) ? kSpawnSize : world.scale;
        inst.color = e.color;
        inst.flags = e.flags;
        if (e.is(scene::kTypePointLight)) { // point light: a small box in the light's color (so it doesn't block the light)
            inst.scale = {0.2f, 0.2f, 0.2f};
            inst.color = scene::readObject<scene::PointLightFields>(e).color.value;
        }
        if (!gameView && isSelected(e.id)) inst.flags |= render::kInstanceHighlight;
        const asset::Model* model = e.model.empty() ? nullptr : m_models->get(e.model);
        const render::MeshId mesh = model ? m_models->pickLod(*model, eye, world.position, world.scale) : box;
        if (model && m_debugView != render::DebugView::None && !gameView) { // view modes: drawn LOD level, over budget
            for (u32 lod = 0; lod < model->lodCount; ++lod)
                if (model->lods[lod] == mesh) inst.flags |= lod << render::kInstanceLodShift;
            if (!model->budgetProblems.empty()) inst.flags |= render::kInstanceOverBudget;
        }
        if (model) asset::addModel(out, *model, mesh, inst, scene::animField(e), m_animTime); // skinned models use the clip from the anim field
        else out.add(mesh, inst);
    }
}

void Editor::bakeProbes(u32 onlyId) {
    m_probeQueue.clear();
    m_probeFace = 0;
    m_probeCapturePending = false;
    for (const Entity& e : m_scene.entities)
        if (e.is(scene::kTypeProbe) && (!onlyId || e.id == onlyId)) m_probeQueue.push_back(e.id);
    if (m_probeQueue.empty()) setStatus(tr("No reflection probes to bake"));
}

void Editor::renderProbeBake(render::Renderer& renderer) {
    if (m_probeQueue.empty() || !m_models) return;
    u32 size = 16; // power of two (mips halve)
    while (size * 2 <= std::clamp<u32>(m_project.settings.probeSize, 16, 512)) size *= 2;
    if (!m_probeTarget || m_probeTargetSize != size) {
        m_probeTarget = renderer.createRenderTarget(size, size); // when the size changes the old target remains (the renderer doesn't delete it)
        m_probeTargetSize = size;
        m_probeFace = 0;
        m_probeCapturePending = false;
    }
    if (m_probeCapturePending) { // face drawn last frame: sRGB 8-bit → linear
        m_probeCapturePending = false;
        m_probeFaces.resize(static_cast<size_t>(size) * size * 18);
        if (m_probeCapture.width == size && m_probeCapture.height == size) {
            f32* dst = &m_probeFaces[static_cast<size_t>(m_probeFace) * size * size * 3];
            for (size_t i = 0; i < static_cast<size_t>(size) * size; ++i)
                for (u32 c = 0; c < 3; ++c) {
                    const f32 v = static_cast<f32>(m_probeCapture.rgba[i * 4 + c]) / 255.0f;
                    dst[i * 3 + c] = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
                }
        }
        if (++m_probeFace == 6) {
            m_probeFace = 0;
            const u32 id = m_probeQueue.front();
            m_probeQueue.erase(m_probeQueue.begin());
            if (Entity* e = m_scene.find(id)) {
                asset::ProbeMap probe;
                asset::bakeProbe(m_probeFaces.data(), size, probe);
                std::string stem = m_scenePath.empty() ? std::string("untitled") : fileName(m_scenePath);
                if (const size_t dot = stem.find_last_of('.'); dot != std::string::npos) stem.resize(dot);
                const std::string path = scene::probeFileFor(stem, *e);
                if (asset::saveProbe(asset::ModelCache::assetsRoot() + "/" + path, probe)) {
                    if (scene::probeFile(*e) != path) {
                        std::erase_if(e->fields, [](const auto& f) { return f.first == scene::kProbeFileField; });
                        e->fields.emplace_back(scene::kProbeFileField, path);
                        markChanged();
                    }
                    m_models->reloadChanged(); // re-upload if the same path was rebaked
                    MEALKIT_INFO("Reflection probe baked: %s (%u, %u mips)", path.c_str(), probe.size, probe.mips);
                    setStatus(trf("Baked reflection probe: %s", path.c_str()));
                } else {
                    setStatus(trf("Could not write: %s", path.c_str()), true);
                }
            }
            if (m_probeQueue.empty()) return;
        }
    }
    const Entity* e = m_scene.find(m_probeQueue.front());
    if (!e) {
        m_probeQueue.erase(m_probeQueue.begin());
        m_probeFace = 0;
        return;
    }
    // Cube face orientations (same layout as asset::cubeDirection: right of a left-handed lookAt = cross(up, forward))
    static const Vec3 kForward[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const Vec3 kUp[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
    const Vec3 eye = scene::worldTransform(m_scene, *e).position;
    render::FrameParams frame;
    frame.viewProj = lookAtLH(eye, eye + kForward[m_probeFace], kUp[m_probeFace]) *
                     perspectiveReversedZ(3.14159265f * 0.5f, 1.0f, 0.05f, 2000.0f);
    frame.cameraPos = eye;
    frame.cameraForward = kForward[m_probeFace];
    fillEnvironment(frame); // probes aren't used (so old images of other probes don't mix in)
    frame.post.toneMapping = false; // reflections stay linear (tone mapping happens once on the screen that receives them)
    render::DrawList list;
    addSceneObjects(list, eye, true);
    renderer.drawToTarget(m_probeTarget, frame, list, static_cast<render::LightingModel>(m_project.settings.lightingModel));
    renderer.captureTarget(m_probeTarget, &m_probeCapture);
    m_probeCapturePending = true;
}

void Editor::renderGradeCapture(render::Renderer& renderer) {
    if (m_gradeCapturePending) { // endFrame read what was drawn last frame
        m_gradeCapturePending = false;
        std::string stem = m_scenePath.empty() ? std::string("untitled") : fileName(m_scenePath);
        if (const size_t dot = stem.find_last_of('.'); dot != std::string::npos) stem.resize(dot);
        const std::string folder = projectRoot() + "/art/grading";
        const std::string png = folder + "/" + stem + ".png", cube = folder + "/neutral.cube";
        createDirectories(folder);
        if (!fileExists(cube)) asset::saveNeutralCubeLut(cube);
        if (m_gradeCapture.width && savePng(png, m_gradeCapture)) {
            MEALKIT_INFO("Grading capture saved: %s", png.c_str());
            setStatus(trf("Grading capture saved: %s", png.c_str()));
        } else {
            setStatus(trf("Could not write: %s", png.c_str()), true);
        }
        return;
    }
    if (!m_gradeRequested || !m_models || bakingProbes()) return;
    m_gradeRequested = false;
    const u32 w = std::max(64u, static_cast<u32>(m_viewport.width)), h = std::max(64u, static_cast<u32>(m_viewport.height));
    if (!m_gradeTarget || m_gradeTargetSize[0] != w || m_gradeTargetSize[1] != h) { // when the size changes the old target remains
        m_gradeTarget = renderer.createRenderTarget(w, h);
        m_gradeTargetSize[0] = w;
        m_gradeTargetSize[1] = h;
    }
    render::FrameParams frame;
    render::DrawList list;
    frame.viewProj = m_view * m_proj;
    frame.cameraPos = m_cameraPos;
    frame.cameraForward = cameraForward();
    fillEnvironment(frame);
    m_models->probes(m_scene, m_probeList);
    frame.probes = m_probeList.data();
    frame.probeCount = static_cast<u32>(m_probeList.size());
    frame.post.lut = 0; // image before grading (up to tone mapping)
    addSceneObjects(list, m_cameraPos, true);
    renderer.drawToTarget(m_gradeTarget, frame, list, static_cast<render::LightingModel>(m_project.settings.lightingModel));
    renderer.captureTarget(m_gradeTarget, &m_gradeCapture);
    m_gradeCapturePending = true;
}

void Editor::buildShot(Vec3 position, f32 yawDegrees, f32 pitchDegrees, f32 width, f32 height, render::FrameParams& frame,
                       render::DrawList& drawList) {
    constexpr f32 kDegrees = 3.14159265f / 180.0f;
    m_cameraPos = position;
    m_cameraYaw = yawDegrees * kDegrees;
    m_cameraPitch = pitchDegrees * kDegrees;
    m_selected = 0; // no selection highlight
    m_animTime = 0.0; // animation only uses the clip offset (anim field)
    m_viewport = {0.0f, 0.0f, width, height};
    m_proj = perspectiveReversedZ(kFovY, width / std::max(height, 1.0f), 0.1f, 2000.0f);
    m_view = lookAtLH(m_cameraPos, m_cameraPos + cameraForward(), {0, 1, 0});
    buildSceneView(frame, drawList);
}

void Editor::renderLookdev(render::Renderer& renderer, f64 dt) {
    if (!m_lookdevOpen || !m_models) return;
    // Follows the selected object's model (selecting an object without a model keeps the previous model)
    if (const Entity* e = selected(); e && !e->model.empty()) m_lookdev.setModel(e->model);
    m_lookdev.render(renderer, *m_models, static_cast<render::LightingModel>(m_project.settings.lightingModel), dt);
}

void Editor::captureGpuFrame() {
    if (rhi::captureNextGpuFrame()) setStatus(trf("GPU capture: next frame -> %s", rhi::gpuCaptureFolder().c_str()));
}

void Editor::setMode(Mode mode) {
    if (mode == Mode::Play && m_playFromHere && m_playPlayers == 1) moveSpawnsHere();
    m_playFromHere = false;
    if (mode == Mode::Edit && !m_spawnBackup.empty()) { // play from here: restore spawn points (scene file unchanged)
        for (const auto& [id, world] : m_spawnBackup)
            if (Entity* e = m_scene.find(id)) scene::setWorldTransform(m_scene, *e, world);
        m_spawnBackup.clear();
    }
    m_mode = mode;
    m_looking = false;
    m_window->setCursorCaptured(false);
    if (mode == Mode::Edit) m_playStatus.clear();
    focusPanel(mode == Mode::Play ? kStateWindow : kInspectorWindow); // during play, bring the [State] tab to front
}

// Temporarily moves all spawn points to the selected object if it's a spawn, otherwise to the floor under the camera (in the camera's view direction)
void Editor::moveSpawnsHere() {
    scene::Transform target;
    const Entity* picked = selected();
    if (picked && picked->is(scene::kTypeSpawn)) {
        target = scene::worldTransform(m_scene, *picked);
    } else {
        f32 t = 0.0f;
        target.position = m_cameraPos;
        if (raycast(m_cameraPos, {0.0f, -1.0f, 0.0f}, &t)) target.position.y -= t;
        else target.position.y = 0.0f;
        target.position.y += 0.1f;
        target.rotation = quatFromAxisAngle({0.0f, 1.0f, 0.0f}, m_cameraYaw);
    }
    m_spawnBackup.clear();
    for (Entity& e : m_scene.entities) {
        if (!e.is(scene::kTypeSpawn)) continue;
        scene::Transform w = scene::worldTransform(m_scene, e);
        m_spawnBackup.emplace_back(e.id, w);
        w.position = target.position;
        w.rotation = target.rotation;
        scene::setWorldTransform(m_scene, e, w);
    }
    if (m_spawnBackup.empty()) setStatus(tr("Play from here: the scene has no spawn points"), true);
}

bool Editor::takePlayRequest() {
    const bool r = m_playRequest;
    m_playRequest = false;
    return r;
}

bool Editor::takeStopRequest() {
    const bool r = m_stopRequest;
    m_stopRequest = false;
    return r;
}

bool Editor::gameKeyboardAllowed() const { return m_mode == Mode::Play && !ImGui::GetIO().WantCaptureKeyboard; }

bool Editor::gameMouseAllowed() const { return m_mode == Mode::Play && !ImGui::GetIO().WantCaptureMouse; }

void Editor::saveBackupIfDirty() {
    if (!m_dirty) return;
    const std::string base = m_scenePath.empty() ? executableDirectory() + "\\untitled.scene" : m_scenePath;
    const std::string path = base + ".autosave";
    if (scene::save(m_scene, path)) MEALKIT_INFO("Unsaved scene changes written to %s", path.c_str());
}

void Editor::selectByIndex(int index, bool add) {
    if (index == -2) { // scene row (inspector = environment)
        m_selected = 0;
        m_sceneRow = true;
        return;
    }
    if (index >= 0 && static_cast<size_t>(index) < m_scene.entities.size())
        selectMany({m_scene.entities[static_cast<size_t>(index)].id}, add);
}

// ---------------------------------------------------------------- UI

// Predefined layouts: scene tree on the left, one column on the right (inspector + [State]/[Tuning] tabs), bottom row (build log, tables).
// Play shrinks the bottom row and brings [State] to front; lookdev puts a large [Lookdev] window at the top right.
void Editor::setupDefaultLayout(u32 dockId, LayoutPreset preset) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, viewport->WorkSize);
    ImGuiID center = dockId;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.17f, nullptr, &center);
    const f32 rightShare = preset == LayoutPreset::LookDev ? 0.42f : 0.27f;
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, rightShare, nullptr, &center);
    const f32 bottomShare = preset == LayoutPreset::Play ? 0.18f : 0.26f;
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, bottomShare, nullptr, &center);
    ImGui::DockBuilderDockWindow(kHierarchyWindow, left);
    if (preset == LayoutPreset::LookDev) {
        const ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, nullptr, &right);
        ImGui::DockBuilderDockWindow(LookdevPanel::kWindow, right);
        right = rightBottom;
        m_lookdevOpen = true;
    }
    ImGui::DockBuilderDockWindow(kInspectorWindow, right);
    ImGui::DockBuilderDockWindow(kStateWindow, right);
    ImGui::DockBuilderDockWindow(TuningPanel::kWindow, right);
    for (const char* w : kBottomWindows) ImGui::DockBuilderDockWindow(w, bottom);
    ImGui::DockBuilderFinish(dockId);
    focusPanel(preset == LayoutPreset::Play ? kStateWindow : kInspectorWindow);
}

void Editor::drawMenuBar() {
    const bool editing = m_mode == Mode::Edit;
    if (!editing) ImGui::PushStyleColor(ImGuiCol_MenuBarBg, kPlayColor); // so it's clear you're in play
    const bool open = ImGui::BeginMainMenuBar();
    if (!editing) ImGui::PopStyleColor();
    if (!open) return;

    if (ImGui::BeginMenu(tr("File"), editing)) {
        commandMenuItem("New Scene");
        commandMenuItem("Open...");
        commandMenuItem("Save");
        commandMenuItem("Save As...");
        ImGui::Separator();
        commandMenuItem("Scenes", &m_sceneListOpen);
        commandMenuItem("Open Replay...");
        if (ImGui::BeginMenu(tr("Recent Scenes"), !m_recentScenes.empty())) {
            std::string chosen;
            for (const std::string& path : m_recentScenes)
                if (ImGui::MenuItem(fileName(path).c_str(), nullptr, path == m_scenePath)) chosen = path;
            ImGui::EndMenu();
            if (!chosen.empty() && chosen != m_scenePath) openSceneFile(chosen);
        }
        ImGui::Separator();
        commandMenuItem("Project Settings...");
        commandMenuItem("Modules...");
        commandMenuItem("Preferences...");
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Exit"))) m_quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Edit"), editing)) {
        commandMenuItem("Undo");
        commandMenuItem("Redo");
        commandMenuItem("History");
        ImGui::Separator();
        commandMenuItem("Select All");
        commandMenuItem("Duplicate");
        commandMenuItem("Save as Prefab...");
        commandMenuItem("Delete");
        ImGui::Separator();
        commandMenuItem("Drop to Floor");
        if (ImGui::BeginMenu(tr("Align"), !m_multi.empty())) {
            commandMenuItem("Align X");
            commandMenuItem("Align Y");
            commandMenuItem("Align Z");
            commandMenuItem("Distribute Evenly");
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Add"), editing)) {
        drawTypeMenuItems();
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Model..."))) addModelEntity();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("View"))) {
        commandMenuItem("Focus Selected");
        commandMenuItem("Command Palette");
        commandMenuItem("Keyboard Shortcuts");
        commandMenuItem("Tables", &m_tablesOpen);
        commandMenuItem("Sounds", &m_soundsOpen);
        commandMenuItem("Feedback", &m_feedbackOpen);
        commandMenuItem("UI", &m_uiPanelOpen);
        commandMenuItem("Look Dev", &m_lookdevOpen);
        if (ImGui::BeginMenu(tr("Preview Cutscene"), m_mode == Mode::Edit)) {
            if (ImGui::IsWindowAppearing()) m_previewCutscenes.load(); // assets/tables/cutscenes.table as it is now
            for (u32 i = 0; i < m_previewCutscenes.count(); ++i) {
                const asset::Cutscenes::Cutscene& c = m_previewCutscenes.at(i);
                char label[160];
                std::snprintf(label, sizeof(label), "%s (%.1f s)", c.name.c_str(), c.clip ? static_cast<double>(c.clip->duration) : 0.0);
                if (ImGui::MenuItem(label, nullptr, c.name == m_cutscenePreview, c.clip != nullptr)) previewCutscene(c.name);
            }
            if (m_previewCutscenes.count() == 0) ImGui::TextDisabled("%s", tr("No rows in tables/cutscenes.table"));
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("Layout"))) {
            const char* names[] = {tr("Edit"), tr("Play"), tr("Look Dev")};
            for (int i = 0; i < 3; ++i)
                if (ImGui::MenuItem(names[i], nullptr, static_cast<int>(m_layoutPreset) == i)) {
                    m_layoutPreset = static_cast<LayoutPreset>(i);
                    m_layoutReset = true;
                }
            ImGui::Separator();
            commandMenuItem("Reset Layout");
            ImGui::EndMenu();
        }
        ImGui::Separator();
        // View modes: what's expensive, by color (edit screen, not in shipping builds)
        if (ImGui::BeginMenu(tr("View Mode"))) {
            const char* names[] = {tr("Lit"), tr("Wireframe"), tr("Normals"), tr("LOD Level"), tr("Overdraw"),
                                   tr("Over Budget"), tr("Material Features")};
            static_assert(std::size(names) == static_cast<size_t>(render::DebugView::Count));
            for (u32 i = 0; i < static_cast<u32>(render::DebugView::Count); ++i) {
                const auto view = static_cast<render::DebugView>(i);
                if (ImGui::MenuItem(names[i], nullptr, m_debugView == view, m_renderer->supportsDebugView(view)))
                    m_debugView = view;
            }
            ImGui::EndMenu();
        }
        // Off shows the real frame cost instead of the monitor refresh interval
        if (ImGui::MenuItem(tr("Vertical Sync"), nullptr, m_renderer->vsync())) m_renderer->setVsync(!m_renderer->vsync());
        // GPU capture (only when RenderDoc/PIX was loaded with --gpu-capture)
        if (const char* tool = rhi::gpuCaptureTool()) {
            if (ImGui::MenuItem(trf("GPU Capture (%s)", tool).c_str(), "F11")) captureGpuFrame();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Build"), !m_project.directory.empty())) {
        commandMenuItem("Build Game Code (Hot Reload)");
        commandMenuItem("Release Build");
        ImGui::Separator();
        commandMenuItem("Play From Here");
        ImGui::Separator();
        commandMenuItem("Open Release Folder");
        commandMenuItem("Open Project Folder");
        ImGui::Separator();
        commandMenuItem("Build Log", m_buildPanel.openFlag());
        commandMenuItem("Log", m_logPanel.openFlag());
        commandMenuItem("Assets", &m_assetsOpen);
        commandMenuItem("Performance", m_perfPanel.openFlag());
        ImGui::EndMenu();
    }
    // Tool suite (editor/Tools.h): opens MealKit tools from separate repos with this project
    if (ImGui::BeginMenu(tr("Tools"), !m_project.directory.empty())) {
        if (m_tools.empty()) ImGui::MenuItem(tr("No tools installed"), nullptr, false, false);
        for (const ToolInfo& t : m_tools) {
            const std::string label = t.version.empty() ? t.name : t.name + "  " + t.version;
            if (ImGui::MenuItem(label.c_str(), nullptr, false, t.problem == ToolInfo::Problem::None))
                launchTool(t, m_project.directory);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (t.problem == ToolInfo::Problem::Protocol)
                    ImGui::SetTooltip("%s", trf("Needs tool protocol %u, this editor has %u", t.protocol, live::kProtocol).c_str());
                else if (t.problem == ToolInfo::Problem::MissingExe)
                    ImGui::SetTooltip("%s", trf("Program not found: %s", t.exe.c_str()).c_str());
                else ImGui::SetTooltip("%s", t.folder.c_str());
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Open Tools Folder"))) {
            createDirectories(defaultToolsFolder());
            openInExplorer(defaultToolsFolder());
        }
        if (ImGui::MenuItem(tr("Rescan Tools"))) m_tools = findTools(m_toolPaths);
        ImGui::EndMenu();
    }


    const std::string playLabel = editing ? std::string("  ▶ ") + tr("Play (F5)") + "  " : std::string("  ■ ") + tr("Stop (Esc)") + "  ";
    const f32 width = ImGui::CalcTextSize(playLabel.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 8.0f, (ImGui::GetWindowWidth() - width) * 0.5f));
    const ImVec4 color = editing ? ImVec4(0.18f, 0.48f, 0.26f, 1.0f) : ImVec4(0.62f, 0.20f, 0.18f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x * 1.25f, color.y * 1.25f, color.z * 1.25f, 1.0f));
    ImGui::BeginDisabled(editing && !m_gameLoaded); // can't play without the game DLL
    if (ImGui::Button(playLabel.c_str())) {
        if (editing) m_playRequest = true;
        else m_stopRequest = true;
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
    if (editing && !m_gameLoaded && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", m_gameStatus.c_str());

    // Player count (with multiple, more join windows open)
    ImGui::BeginDisabled(!editing);
    const char* counts[] = {tr("Solo"), tr("2 players"), tr("3 players"), tr("4 players")};
    int players = static_cast<int>(m_playPlayers) - 1;
    ImGui::SetNextItemWidth(ImGui::CalcTextSize(counts[1]).x + ImGui::GetFrameHeight() * 2.0f);
    if (ImGui::Combo("##players", &players, counts, 4)) m_playPlayers = static_cast<u32>(players + 1);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tr("With several players the editor hosts and opens more player windows (current scene and tuning, same PC)"));
    if (m_playPlayers > 1) ImGui::Checkbox(tr("Bots in player windows"), &m_playClientBots);
    if (editing) {
        ImGui::Checkbox(tr("Shadow check"), &m_shadowCheck);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s", tr("Runs a second simulation with the same inputs during play and compares every tick.\n"
                                    "On a mismatch it stops and shows the first diverging tick and state fields (2x simulation CPU)"));
    }
    ImGui::EndDisabled();
    if (!editing) drawPlayControls();
    ImGui::EndMainMenuBar();
}

void Editor::drawStatusBar() {
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, ImGui::GetFrameHeight(),
                                    flags)) {
        if (ImGui::BeginMenuBar()) {
            const std::string name = m_scenePath.empty() ? tr("Untitled") : fileName(m_scenePath);
            ImGui::Text("%s%s", name.c_str(), m_dirty ? " *" : "");
            ImGui::TextDisabled("|");
            ImGui::Text(tr("%zu objects"), m_scene.entities.size());
            ImGui::TextDisabled("|");
            if (m_buildPanel.drawStatus(m_time)) {
                ImGui::TextDisabled("|");
            } else if (!m_gameStatus.empty()) {
                if (m_gameLoaded) ImGui::TextDisabled("%s", m_gameStatus.c_str());
                else ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "%s", m_gameStatus.c_str());
                ImGui::TextDisabled("|");
            }
            if (m_mode == Mode::Play) ImGui::TextUnformatted(m_playStatus.c_str());
            else if (m_time - m_statusTime < 4.0)
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s", m_status.c_str());
            else
                ImGui::TextDisabled(
                    "%s", tr("Camera: right drag + WASD/QE, wheel for speed | Select: click | Hold Ctrl on gizmo: snap"));
            const char* fps = "0000 fps";
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::CalcTextSize(fps).x - 16.0f);
            // Red when over budget; clicking opens the [Performance] window
            const ImVec4 fpsColor = m_perfPanel.overBudget() ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f)
                                                             : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            ImGui::TextColored(fpsColor, "%.0f fps", m_perf.sample().fps);
            if (ImGui::IsItemClicked()) *m_perfPanel.openFlag() = true;
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

void Editor::drawHierarchy(bool editing) {
    if (!ImGui::Begin(trl(kHierarchyWindow))) {
        ImGui::End();
        return;
    }
    ImGui::BeginDisabled(!editing);
    if (ImGui::Button(tr("+ Box"))) addEntity(scene::kTypeSolid);
    ImGui::SameLine();
    if (ImGui::Button(tr("+ Model"))) addModelEntity();
    ImGui::SameLine();
    if (ImGui::Button(tr("+ Spawn"))) addEntity(scene::kTypeSpawn);
    ImGui::SameLine();
    if (ImGui::Button(tr("+ Type..."))) ImGui::OpenPopup("##addType");
    if (ImGui::BeginPopup("##addType")) {
        drawTypeMenuItems();
        ImGui::EndPopup();
    }
    // Search (name/kind, Ctrl+F): with text, only matching objects, flat
    if (m_treeSearchFocus) {
        ImGui::SetKeyboardFocusHere();
        m_treeSearchFocus = false;
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##treeSearch", (std::string(icon::kSearch) + "  " + tr("Find (Ctrl+F)")).c_str(), m_treeSearch,
                             sizeof(m_treeSearch));
    ImGui::Separator();

    if (ImGui::BeginChild("##entities")) {
        // parent → child list (entities order)
        std::unordered_map<u32, std::vector<size_t>> children;
        std::vector<size_t> roots;
        for (size_t i = 0; i < m_scene.entities.size(); ++i) {
            const Entity& e = m_scene.entities[i];
            if (e.parent && m_scene.find(e.parent)) children[e.parent].push_back(i);
            else roots.push_back(i);
        }
        m_treeAction = {};
        // Fixed top row "Scene": selecting it makes the inspector show the scene environment (docs/DECISIONS.md 2026-10-28)
        const std::string sceneName = m_scenePath.empty() ? tr("Untitled") : fileName(m_scenePath);
        const std::string sceneLabel = trf("Scene: %s", sceneName.c_str());
        if (ImGui::Selectable(sceneLabel.c_str(), m_sceneRow && m_selected == 0, ImGuiSelectableFlags_SpanAvailWidth)) {
            m_selected = 0;
            m_sceneRow = true;
        }
        if (m_treeSearch[0]) {
            const std::unordered_map<u32, std::vector<size_t>> none;
            for (size_t i = 0; i < m_scene.entities.size(); ++i) {
                const Entity& e = m_scene.entities[i];
                const reflect::TypeInfo* type = entityType(e);
                if (containsNoCase(e.name, m_treeSearch) || containsNoCase(e.type, m_treeSearch) ||
                    (type && containsNoCase(trData(type->label), m_treeSearch)))
                    drawEntityNode(i, none);
            }
        } else {
            for (size_t i : roots) drawEntityNode(i, children);
        }
        m_treeRevealed = m_selected;
        // Dropping on empty space moves to top level
        const ImVec2 rest = ImGui::GetContentRegionAvail();
        ImGui::InvisibleButton("##treeRoot", ImVec2(std::max(rest.x, 1.0f), std::max(rest.y, ImGui::GetFrameHeight())));
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload))
                m_treeAction = {TreeAction::Reparent, *static_cast<const u32*>(payload->Data), 0};
            ImGui::EndDragDropTarget();
        }
    }
    ImGui::EndChild();
    // Change the structure after drawing the whole tree (so the list doesn't change mid-draw)
    switch (m_treeAction.kind) {
    case TreeAction::Reparent: reparent(m_treeAction.entity, m_treeAction.parent); break;
    case TreeAction::Duplicate: duplicateSelected(); break;
    case TreeAction::SavePrefab: saveSelectionAsPrefab(); break;
    case TreeAction::Delete: deleteSelected(); break;
    case TreeAction::None: break;
    }
    ImGui::EndDisabled();
    ImGui::End();
}

void Editor::drawEntityNode(size_t index, const std::unordered_map<u32, std::vector<size_t>>& children) {
    const Entity& e = m_scene.entities[index];
    const u32 id = e.id;
    const auto kids = children.find(id);
    const bool leaf = kids == children.end();
    ImGui::PushID(static_cast<int>(id));
    f32 rgb[3];
    unpackColor(e.color, rgb);
    std::string label = e.name.empty() ? tr("(no name)") : e.name;
    if (!e.is(scene::kTypeSolid)) {
        const reflect::TypeInfo* type = entityType(e);
        label += std::string("  · ") + (type ? trData(type->label) : e.type.c_str());
    }
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap;
    if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (isSelected(id)) flags |= ImGuiTreeNodeFlags_Selected;
    // Newly selected objects expand their ancestors and scroll to their row (when selected in the viewport)
    const bool reveal = m_selected != m_treeRevealed && m_selected != 0;
    if (reveal && !leaf && m_scene.find(m_selected) && scene::isDescendant(m_scene, m_selected, id))
        ImGui::SetNextItemOpen(true);
    // Leave room for the kind icon before the label and draw it in the object's color
    const bool hiddenRow = isHidden(e);
    if (hiddenRow) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
    const bool open = ImGui::TreeNodeEx("##node", flags, "      %s", label.c_str());
    if (reveal && m_selected == id) ImGui::SetScrollHereY();
    const ImVec2 rowMin = ImGui::GetItemRectMin(), rowMax = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddText(ImVec2(rowMin.x + ImGui::GetTreeNodeToLabelSpacing(), rowMin.y),
                                        ImGui::ColorConvertFloat4ToU32(ImVec4(rgb[0], rgb[1], rgb[2], 1.0f)),
                                        entityIcon(e));
    if (hiddenRow) ImGui::PopStyleVar();
    // Click to select, double-click to move to, drag to reparent
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        if (ImGui::GetIO().KeyShift) {
            toggleSelect(id);
        } else {
            m_selected = id;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) focusSelected();
        }
    }
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kEntityPayload, &id, sizeof(id));
        ImGui::Text(tr("%s -> (child of the drop target)"), label.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload))
            m_treeAction = {TreeAction::Reparent, *static_cast<const u32*>(payload->Data), id};
        ImGui::EndDragDropTarget();
    }
    // Visibility/lock toggles on the right of the row (shown only on hover or when on)
    {
        const bool hovered = ImGui::IsItemHovered();
        const f32 w = ImGui::GetFrameHeight();
        const ImVec2 back = ImGui::GetCursorScreenPos();
        bool toggled = false;
        int slot = 0;
        for (auto* set : {&m_locked, &m_hidden}) {
            const bool on = set->count(id) != 0;
            ImGui::SetCursorScreenPos(ImVec2(rowMax.x - w * static_cast<f32>(++slot), rowMin.y));
            ImGui::PushID(slot);
            if (ImGui::InvisibleButton("##toggle", ImVec2(w, rowMax.y - rowMin.y))) {
                if (on) set->erase(id);
                else set->insert(id);
                toggled = true;
            }
            const bool lockSet = set == &m_locked;
            if (on || hovered || ImGui::IsItemHovered()) {
                const char* glyph = lockSet ? (on ? icon::kLock : icon::kUnlock) : (on ? icon::kHide : icon::kView);
                ImGui::GetWindowDrawList()->AddText(ImVec2(ImGui::GetItemRectMin().x + 4.0f, rowMin.y),
                                                    ImGui::GetColorU32(on ? ImGuiCol_Text : ImGuiCol_TextDisabled), glyph);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", lockSet ? tr("Lock (cannot be picked in the scene view)") : tr("Hide in the scene view"));
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(back);
        if (toggled) saveViewState();
    }
    if (ImGui::BeginPopupContextItem()) {
        if (!isSelected(id)) m_selected = id;
        if (ImGui::MenuItem(tr("Duplicate"), "Ctrl+D")) m_treeAction = {TreeAction::Duplicate, id, 0};
        if (ImGui::MenuItem(tr("Save as Prefab..."))) m_treeAction = {TreeAction::SavePrefab, id, 0};
        if (ImGui::MenuItem(tr("Delete"), "Delete")) m_treeAction = {TreeAction::Delete, id, 0};
        if (ImGui::MenuItem(tr("Move to Top Level"), nullptr, false, e.parent != 0))
            m_treeAction = {TreeAction::Reparent, id, 0};
        ImGui::EndPopup();
    }
    if (open && !leaf) {
        for (size_t child : kids->second) drawEntityNode(child, children);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void Editor::reparent(u32 entityId, u32 parentId) {
    Entity* e = m_scene.find(entityId);
    if (!e || e->parent == parentId) return;
    if (parentId && scene::isDescendant(m_scene, parentId, entityId)) {
        setStatus(tr("Cannot move an object under itself or its children"));
        return;
    }
    pushUndo(capture());
    const scene::Transform world = scene::worldTransform(m_scene, *e); // keeps its place on screen
    e->parent = parentId;
    scene::setWorldTransform(m_scene, *e, world);
    const Entity* parent = parentId ? m_scene.find(parentId) : nullptr;
    setStatus(parent ? trf("%s -> child of %s", e->name.c_str(), parent->name.c_str()) : trf("%s -> top level", e->name.c_str()));
}

void Editor::drawInspector(bool editing) {
    if (m_focusPanel == kInspectorWindow) focusPanelNow();
    if (!ImGui::Begin(trl(kInspectorWindow))) {
        ImGui::End();
        return;
    }
    if (!editing)
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.35f, 1.0f), "%s", tr("Playing - scene values go back when you stop"));
    ImGui::BeginDisabled(!editing);
    if (m_selected != 0) m_sceneRow = false; // selecting an object deselects the scene row
    if (!m_multi.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.75f, 1.0f, 1.0f));
        ImGui::TextWrapped("%s", trf("%zu objects selected - the gizmo moves all, this panel edits the last one",
                                     m_multi.size() + 1).c_str());
        ImGui::PopStyleColor();
    }
    Entity* e = selected();
    if (!e && m_sceneRow) {
        drawEnvironment();
    } else if (!e) {
        ImGui::TextDisabled("%s", tr("Nothing selected."));
        ImGui::TextDisabled("%s", tr("Click in the viewport or pick from the scene tree."));
    } else {
        ImGui::PushItemWidth(-ImGui::CalcTextSize(tr("Rotation (deg)")).x - 12.0f);
        if (m_nameBufferId != e->id) {
            strncpy_s(m_nameBuffer, e->name.c_str(), _TRUNCATE);
            m_nameBufferId = e->id;
        }
        if (ImGui::InputText(tr("Name"), m_nameBuffer, sizeof(m_nameBuffer))) {
            e->name = m_nameBuffer;
            markChanged();
        }
        const reflect::TypeInfo* type = entityType(*e);
        const std::string typeLabel = type ? trData(type->label) : trf("%s (not in game code)", e->type.c_str());
        if (ImGui::BeginCombo(tr("Type"), typeLabel.c_str())) {
            for (const reflect::TypeInfo* t : allTypes()) {
                if (ImGui::Selectable(trData(t->label), t == type) && t != type) {
                    e->type = t->name;
                    e->fields = scene::defaultFields(*t);
                    markChanged();
                }
            }
            ImGui::EndCombo();
        }
        // Opens the game kind's declaration (MEALKIT_OBJECT) in the code editor (project source folder, or the samples folder in the engine repo)
        if (type && !scene::findType(e->type, nullptr)) {
            if (ImGui::SmallButton(tr("Open Code"))) {
                FileLine at;
                const std::vector<std::string> folders = {m_project.directory + "\\source", m_project.directory + "\\samples"};
                if (findInHeaders(folders, std::string("MEALKIT_OBJECT(") + type->name + ")", &at))
                    openInCodeEditor(m_prefs.externalEditor, at.file, at.line);
                else setStatus(trf("Could not find MEALKIT_OBJECT(%s) in the project code", type->name), true);
            }
        }
        drawObjectFields(*e);

        if (const asset::Model* model = e->model.empty() ? nullptr : m_models->get(e->model);
            model && model->skinned() && ImGui::CollapsingHeader(tr("Animation"), ImGuiTreeNodeFlags_DefaultOpen)) {
            // Skinned model: looping view clip (scene field anim, also played on the game screen)
            ImGui::TextDisabled(tr("%u joints, %u clips"), model->bones, static_cast<u32>(model->clips.size()));
            const std::string current = scene::animField(*e);
            const std::string currentClip = current.substr(0, current.find(' '));
            if (ImGui::BeginCombo(tr("Clip"), currentClip.empty() ? tr("(rest pose)") : currentClip.c_str())) {
                const auto set = [&](const std::string& value) {
                    std::erase_if(e->fields, [](const auto& f) { return f.first == scene::kAnimField; });
                    if (!value.empty()) e->fields.emplace_back(scene::kAnimField, value);
                    markChanged();
                };
                if (ImGui::Selectable(tr("(rest pose)"), currentClip.empty())) set({});
                for (const asset::AnimationClip& c : model->clips) {
                    char label[160];
                    std::snprintf(label, sizeof(label), "%s (%.2f s)", c.name.c_str(), static_cast<double>(c.duration));
                    if (ImGui::Selectable(label, c.name == currentClip)) set(c.name);
                }
                ImGui::EndCombo();
            }
        }

        if (e->is(scene::kTypeVirtualCamera) && ImGui::CollapsingHeader(tr("Virtual camera"), ImGuiTreeNodeFlags_DefaultOpen)) {
            // Shows the scene view through this camera (no follow target in edit mode: it stands at its placement)
            bool through = m_lookThrough == e->id;
            if (ImGui::Checkbox(tr("Look Through"), &through)) m_lookThrough = through ? e->id : 0;
            ImGui::SameLine();
            ImGui::TextDisabled("%s", tr("Esc stops"));
        }

        if (e->is(scene::kTypeProbe) && ImGui::CollapsingHeader(tr("Reflection probe"), ImGuiTreeNodeFlags_DefaultOpen)) {
            // Reflection probe: bake (edit mode, baked files in assets/probes/)
            const std::string file = scene::probeFile(*e);
            ImGui::TextDisabled("%s", file.empty() ? tr("Not baked") : file.c_str());
            ImGui::BeginDisabled(bakingProbes());
            if (ImGui::Button(tr("Bake"))) bakeProbes(e->id);
            ImGui::SameLine();
            if (ImGui::Button(tr("Bake all probes"))) bakeProbes();
            ImGui::EndDisabled();
            u32 count = 0;
            for (const Entity& other : m_scene.entities) count += other.is(scene::kTypeProbe) ? 1u : 0u;
            if (count > probeMaxCount())
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), tr("%u probes, only the first %u are used (project settings)"), count,
                                   probeMaxCount());
        }

        if (!isMarker(*e) && ImGui::CollapsingHeader(tr("Model"), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (e->model.empty()) {
                ImGui::TextDisabled("%s", tr("Default box"));
            } else {
                ImGui::TextUnformatted(e->model.c_str());
                if (const asset::Model* model = m_models->get(e->model)) {
                    ImGui::TextDisabled(tr("%u triangles · %u LODs"), model->triangles[0], model->lodCount);
                    // Asset budget: usage vs. the kind's (folder's) limit, red when over (mealkit.project budget*)
                    const asset::ModelBudget& b = m_models->budget(model->kind);
                    const ImVec4 red(1.0f, 0.45f, 0.4f, 1.0f);
                    const auto usage = [&](u32 used, u32 limit, const char* what) {
                        const ImVec4 color = used > limit ? red : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                        ImGui::TextColored(color, "%s %u / %u", what, used, limit);
                    };
                    const char* kind = model->kind == asset::ModelKind::Character     ? tr("characters")
                                       : model->kind == asset::ModelKind::Environment ? tr("environment")
                                                                                      : tr("props");
                    ImGui::TextDisabled("%s", trf("Budget: %s", kind).c_str());
                    usage(model->triangles[0], b.triangles, tr("Triangles"));
                    usage(model->materials, b.materials, tr("Materials"));
                } else ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", tr("Could not load (see the log)"));
            }
            if (ImGui::Button(tr("Choose Model..."))) {
                const std::string path = chooseModel();
                if (!path.empty()) {
                    e->model = path;
                    markChanged();
                }
            }
            if (!e->model.empty()) {
                ImGui::SameLine();
                if (ImGui::Button(tr("Back to Box"))) {
                    e->model.clear();
                    markChanged();
                }
            }
        }

        f32 matrix[16], t[3], r[3], s[3];
        compose(scene::localTransform(*e), matrix);
        ImGuizmo::DecomposeMatrixToComponents(matrix, t, r, s);
        bool changed = false;
        if (ImGui::CollapsingHeader(e->parent ? tr("Transform (relative to parent)###transform") : tr("Transform###transform"),
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            changed |= transformRow(tr("Position"), "##pos", t, 0.05f, 0.0f, 0.0f, "%.2f", 0.0f);
            changed |= transformRow(tr("Rotation (deg)"), "##rot", r, 0.5f, 0.0f, 0.0f, "%.1f", 0.0f);
            if (!isMarker(*e)) changed |= transformRow(tr("Scale"), "##scale", s, 0.02f, 0.01f, 10000.0f, "%.2f", 1.0f, &m_scaleLock);
        }
        if (changed) {
            for (f32& v : s) v = std::max(v, 0.01f);
            ImGuizmo::RecomposeMatrixFromComponents(t, r, s, matrix);
            scene::Transform local;
            decompose(matrix, local);
            e->position = local.position;
            e->rotation = local.rotation;
            e->scale = local.scale;
            markChanged();
        }

        if (ImGui::CollapsingHeader(tr("Look"), ImGuiTreeNodeFlags_DefaultOpen)) {
            f32 rgb[3];
            unpackColor(e->color, rgb);
            if (ImGui::ColorEdit3(tr("Color"), rgb)) {
                e->color = packColor(rgb, e->color);
                markChanged();
            }
            bool checker = (e->flags & render::kInstanceChecker) != 0;
            if (ImGui::Checkbox(tr("Checker pattern"), &checker)) {
                e->flags = checker ? (e->flags | render::kInstanceChecker) : (e->flags & ~render::kInstanceChecker);
                markChanged();
            }
        }
        ImGui::PopItemWidth();
        ImGui::TextDisabled("ID %u", e->id);

        ImGui::Spacing();
        if (ImGui::Button(tr("Duplicate (Ctrl+D)"))) duplicateSelected();
        ImGui::SameLine();
        if (ImGui::Button(tr("Delete (Del)"))) deleteSelected(); // e isn't used after this
    }
    ImGui::EndDisabled();
    ImGui::End();
}

// One transform row: dot if different from default, right-click "Reset to default / Copy / Paste", scale has a lock-ratio toggle
bool Editor::transformRow(const char* label, const char* id, f32 v[3], f32 speed, f32 lo, f32 hi, const char* format,
                          f32 defaultValue, bool* lockRatio) {
    ImGui::PushID(id);
    const f32 before[3] = {v[0], v[1], v[2]};
    if (lockRatio) {
        if (ImGui::SmallButton(*lockRatio ? icon::kLock : icon::kUnlock)) *lockRatio = !*lockRatio;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Keep the ratio between X, Y and Z"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - ImGui::GetItemRectSize().x - ImGui::GetStyle().ItemSpacing.x);
    }
    bool changed = ImGui::DragFloat3(label, v, speed, lo, hi, format);
    if (changed && lockRatio && *lockRatio) { // the rest follow the ratio of the changed axis
        for (int i = 0; i < 3; ++i)
            if (v[i] != before[i] && before[i] != 0.0f) {
                const f32 k = v[i] / before[i];
                for (int j = 0; j < 3; ++j)
                    if (j != i) v[j] = before[j] * k;
                break;
            }
    }
    const bool differs = v[0] != defaultValue || v[1] != defaultValue || v[2] != defaultValue;
    if (differs) {
        const ImVec2 min = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(min.x - 5.0f, min.y + ImGui::GetFrameHeight() * 0.5f), 2.5f,
                                                    ImGui::GetColorU32(ImVec4(0.45f, 0.70f, 1.0f, 1.0f)));
    }
    if (ImGui::BeginPopupContextItem("##transformMenu")) {
        if (ImGui::MenuItem(tr("Reset to Default"), nullptr, false, differs)) {
            v[0] = v[1] = v[2] = defaultValue;
            changed = true;
        }
        if (ImGui::MenuItem(tr("Copy Value"))) {
            char text[96];
            snprintf(text, sizeof(text), "%g %g %g", v[0], v[1], v[2]);
            ImGui::SetClipboardText(text);
        }
        if (ImGui::MenuItem(tr("Paste Value"))) {
            f32 p[3];
            const char* clip = ImGui::GetClipboardText();
            if (clip && sscanf(clip, "%f %f %f", &p[0], &p[1], &p[2]) == 3) {
                v[0] = p[0], v[1] = p[1], v[2] = p[2];
                changed = true;
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

// [Edit] > [History]: oldest at top. Clicking reverts to just before that change (redo can go forward)
void Editor::drawHistory() {
    if (!m_historyOpen) return;
    ImGui::SetNextWindowSize(ImVec2(320, 380), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(trl("History###History"), &m_historyOpen, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    ImGui::BeginDisabled(m_mode != Mode::Edit);
    int target = -1; // number to undo
    for (size_t i = 0; i < m_undo.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(m_undo[i].label.c_str())) target = static_cast<int>(m_undo.size() - i);
        ImGui::PopID();
    }
    ImGui::Selectable(tr("(now)"), true, ImGuiSelectableFlags_Disabled);
    for (size_t i = m_redo.size(); i-- > 0;)
        ImGui::TextDisabled("%s", m_redo[i].label.c_str());
    ImGui::EndDisabled();
    ImGui::End();
    for (int i = 0; i < target; ++i) undo();
}

void Editor::openSceneFile(const std::string& path) {
    saveBackupIfDirty();
    loadScene(path);
}

void Editor::loadRecentScenes() {
    m_recentScenes.clear();
    std::string text;
    if (m_project.directory.empty() || !readTextFile(m_project.directory + "\\build\\editor\\recent_scenes.txt", text)) return;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && fileExists(line)) m_recentScenes.push_back(line);
    }
}

void Editor::rememberRecentScene(const std::string& path) {
    if (m_project.directory.empty() || path.empty()) return;
    std::erase(m_recentScenes, path);
    m_recentScenes.insert(m_recentScenes.begin(), path);
    if (m_recentScenes.size() > 8) m_recentScenes.resize(8);
    std::string text;
    for (const std::string& p : m_recentScenes) text += p + "\n";
    createDirectories(m_project.directory + "\\build\\editor");
    writeTextFile(m_project.directory + "\\build\\editor\\recent_scenes.txt", text);
}

// [File] > [Scenes]: scenes in assets/scenes. Set the start scene (★, mealkit.project scene); double-click to open
void Editor::drawSceneList() {
    if (!m_sceneListOpen) return;
    ImGui::SetNextWindowSize(ImVec2(360, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(trl("Scenes###Scenes"), &m_sceneListOpen)) {
        ImGui::End();
        return;
    }
    const std::string folder = m_project.directory + "\\assets\\scenes";
    static std::string picked;
    ImGui::BeginDisabled(m_mode != Mode::Edit);
    std::string open;
    for (const std::string& name : listFiles(folder, ".scene")) {
        const std::string path = folder + "\\" + name;
        const std::string relative = "scenes/" + name;
        const bool start = relative == m_project.settings.scene;
        const std::string label = std::string(start ? "★ " : "   ") + name + (path == m_scenePath ? std::string("  ") + tr("(open)") : "");
        if (ImGui::Selectable(label.c_str(), picked == path, ImGuiSelectableFlags_AllowDoubleClick)) {
            picked = path;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) open = path;
        }
    }
    ImGui::Separator();
    ImGui::BeginDisabled(picked.empty() || picked == m_scenePath);
    if (ImGui::Button(tr("Open"))) open = picked;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(picked.empty() || m_project.directory.empty());
    if (ImGui::Button(tr("Set as Start Scene"))) m_projectPanel.setStartScene(m_project, "scenes/" + fileName(picked));
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("New Scene"))) newScene();
    ImGui::TextDisabled("%s", tr("★ marks the scene the game starts with (mealkit.project)"));
    ImGui::EndDisabled();
    ImGui::End();
    if (!open.empty()) openSceneFile(open);
}

// Scene environment (when the scene row is selected in the inspector)
void Editor::drawEnvironment() {
    ImGui::SeparatorText(tr("Environment"));
    scene::Environment& env = m_scene.environment;
    const ImGuiColorEditFlags hdr = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR;
    bool c = false;
    ImGui::PushItemWidth(-ImGui::CalcTextSize(tr("Ground bounce")).x - 12.0f);
    c |= ImGui::DragFloat3(tr("Sun direction"), &env.sunDirection.x, 0.01f, -1.0f, 1.0f, "%.2f");
    c |= ImGui::ColorEdit3(tr("Sun light"), &env.sunColor.x, hdr);
    c |= ImGui::ColorEdit3(tr("Sky ambient"), &env.skyColor.x, hdr);
    c |= ImGui::ColorEdit3(tr("Ground bounce"), &env.groundColor.x, hdr);
    c |= ImGui::ColorEdit3(tr("Fog / background"), &env.fogColor.x, hdr);
    c |= ImGui::DragFloat(tr("Fog density"), &env.fogDensity, 0.0002f, 0.0f, 0.1f, "%.4f");
    // Sky HDRI: one .hdr under assets/textures/ (none = the color gradient above). First selection pauses briefly to bake
    if (ImGui::BeginCombo(tr("Sky HDRI"), env.hdri.empty() ? tr("None (sky colors)") : env.hdri.c_str())) {
        if (ImGui::Selectable(tr("None (sky colors)"), env.hdri.empty())) env.hdri.clear(), c = true;
        for (const std::string& h : asset::listHdris(asset::ModelCache::assetsRoot()))
            if (ImGui::Selectable(h.c_str(), env.hdri == h)) env.hdri = h, c = true;
        ImGui::EndCombo();
    }
    ImGui::BeginDisabled(env.hdri.empty());
    c |= ImGui::DragFloat(tr("HDRI intensity"), &env.hdriIntensity, 0.01f, 0.0f, 16.0f, "%.2f");
    c |= ImGui::DragFloat(tr("HDRI rotation"), &env.hdriRotation, 1.0f, -360.0f, 360.0f, "%.0f");
    ImGui::EndDisabled();

    // Post-processing (scene::PostFields → render::PostParams). View values; player settings can turn effects off
    ImGui::SeparatorText(tr("Post processing"));
    scene::PostFields& post = env.post;
    c |= ImGui::Checkbox(tr("Tone mapping (AgX)"), &post.toneMapping);
    ImGui::BeginDisabled(!post.toneMapping);
    c |= ImGui::DragFloat(tr("Exposure (EV)"), &post.exposure, 0.02f, -6.0f, 6.0f, "%.2f");
    if (ImGui::BeginCombo(tr("Color LUT"), post.lut.empty() ? tr("None") : post.lut.c_str())) {
        if (ImGui::Selectable(tr("None"), post.lut.empty())) post.lut.clear(), c = true;
        for (const std::string& l : asset::listLuts(asset::ModelCache::assetsRoot()))
            if (ImGui::Selectable(l.c_str(), post.lut == l)) post.lut = l, c = true;
        ImGui::EndCombo();
    }
    if (ImGui::Button(tr("Grading capture"))) m_gradeRequested = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Saves the scene view without the LUT to art/grading/ with a neutral .cube. Grade it in DaVinci "
                                   "Resolve or Photoshop, export a 32-point .cube into assets/lut/ and pick it above."));
    c |= ImGui::DragFloat(tr("Bloom intensity"), &post.bloomIntensity, 0.01f, 0.0f, 2.0f, "%.2f");
    c |= ImGui::DragFloat(tr("Bloom threshold"), &post.bloomThreshold, 0.02f, 0.0f, 16.0f, "%.2f");
    c |= ImGui::ColorEdit3(tr("Bloom tint"), &post.bloomTint.x, hdr);
    c |= ImGui::SliderFloat(tr("Vignette"), &post.vignette, 0.0f, 1.0f, "%.2f");
    c |= ImGui::SliderFloat(tr("Vignette start"), &post.vignetteSmoothness, 0.0f, 1.0f, "%.2f");
    c |= ImGui::SliderFloat(tr("Film grain"), &post.grain, 0.0f, 1.0f, "%.2f");
    c |= ImGui::SliderFloat(tr("Chromatic aberration"), &post.chromaticAberration, 0.0f, 0.05f, "%.3f");
    ImGui::EndDisabled();
    ImGui::PopItemWidth();
    if (c) markChanged();
    ImGui::TextDisabled("%s", tr("Colors are linear values."));
}

// ---------------------------------------------------------------- Definition data tables ([Tables] panel)

void Editor::saveTablesForPlay() {
    std::string status;
    if (!m_tablesPanel.dirty()) return;
    if (m_tablesPanel.saveAll(&status)) setStatus(trf("Tables saved for play: %s", status.c_str()));
    else setStatus(status);
}

// Row references in data outside the panel (tuning, scene object fields) also go into the [Tables] panel's "missing row references" list (every 0.5 s)
void Editor::updateRowProblems(f64 dt) {
    m_rowCheckTimer += dt;
    if (m_rowCheckTimer < 0.5) return;
    m_rowCheckTimer = 0.0;
    std::vector<std::string> problems;
    const data::TableIndexList& index = m_tablesPanel.index();
    if (index.empty()) {
        m_tablesPanel.setExtraProblems({});
        return;
    }
    m_tuningPanel.resolveRows(index, &problems);
    for (const Entity& e : m_scene.entities) {
        const reflect::TypeInfo* type = entityType(e);
        if (!type || type->fields.empty() || !m_gameTypes) continue;
        reflect::Instance instance(*type);
        reflect::fromText(*type, instance.data(), e.fields);
        data::resolveRows(*type, instance.data(), index, &problems, fileName(m_scenePath) + " " + e.name + ": ");
    }
    m_tablesPanel.setExtraProblems(std::move(problems));
}

// Simulation state during play (shows the state field declarations as is)
void Editor::drawState() {
    if (ImGuiWindow* inspector = ImGui::FindWindowByName(kInspectorWindow); inspector && inspector->DockId)
        ImGui::SetNextWindowDockID(inspector->DockId, ImGuiCond_FirstUseEver);
    if (m_focusPanel == kStateWindow) focusPanelNow();
    if (!ImGui::Begin(trl(kStateWindow))) {
        ImGui::End();
        return;
    }
    if (m_mode != Mode::Play || !m_stateView.type || !m_stateView.data) {
        ImGui::TextDisabled("%s", tr("The simulation state (values declared with MEALKIT_FIELD) appears during play."));
        ImGui::End();
        return;
    }
    ImGui::TextDisabled(tr("%s (read-only)"), m_stateView.type->name);
    drawFieldsReadOnly(*m_stateView.type, m_stateView.data);
    ImGui::End();
}

void Editor::drawGizmo() {
    Entity* e = selected();
    if (!e || m_viewport.width < 1.0f || m_viewport.height < 1.0f) return;

    ImGuizmo::SetOrthographic(m_ortho);
    ImGuizmo::SetRect(m_viewport.x, m_viewport.y, m_viewport.width, m_viewport.height);
    f32 matrix[16];
    compose(scene::worldTransform(m_scene, *e), matrix); // the gizmo is in world space; the result is converted back to parent-relative

    const ImGuizmo::OPERATION ops[3] = {ImGuizmo::TRANSLATE, ImGuizmo::ROTATE, ImGuizmo::SCALE};
    ImGuizmo::OPERATION op = ops[m_gizmoOp];
    if (isMarker(*e) && op == ImGuizmo::SCALE) op = ImGuizmo::TRANSLATE;
    const ImGuizmo::MODE mode = (m_gizmoLocal || op == ImGuizmo::SCALE) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    const f32 step = op == ImGuizmo::TRANSLATE ? m_prefs.snapMove : op == ImGuizmo::ROTATE ? m_prefs.snapRotate : m_prefs.snapScale;
    const f32 snap[3] = {step, step, step};
    const bool snapping = ImGui::GetIO().KeyCtrl != m_snapOn; // when toolbar snap is on, Ctrl temporarily turns it off

    if (ImGuizmo::Manipulate(&m_view.m[0][0], &m_proj.m[0][0], op, mode, matrix, nullptr, snapping ? snap : nullptr)) {
        scene::Transform world;
        decompose(matrix, world);
        const scene::Transform before = scene::worldTransform(m_scene, *e);
        scene::setWorldTransform(m_scene, *e, world);
        if (!m_multi.empty()) moveOthersWith(before, world);
        markChanged();
    }
}

// Toolbar overlaid on the viewport (top left) and axis gizmo (top right; clicking an axis views from that side)
void Editor::drawViewportToolbar() {
    if (m_viewport.width < 200.0f || m_viewport.height < 120.0f) return;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    ImGui::SetNextWindowPos(ImVec2(m_viewport.x + 8.0f, m_viewport.y + 8.0f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
    if (ImGui::Begin("##ViewportToolbar", nullptr, flags)) {
        auto tip = [](const char* text) {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text);
        };
        if (toolButton(tr("Move W"), m_gizmoOp == 0)) m_gizmoOp = 0;
        ImGui::SameLine(0, 2);
        if (toolButton(tr("Rotate E"), m_gizmoOp == 1)) m_gizmoOp = 1;
        ImGui::SameLine(0, 2);
        if (toolButton(tr("Scale R"), m_gizmoOp == 2)) m_gizmoOp = 2;
        ImGui::SameLine();
        if (toolButton(m_gizmoLocal ? tr("Local Axes") : tr("World Axes"), false)) m_gizmoLocal = !m_gizmoLocal;
        ImGui::SameLine();
        if (toolButton(tr("Snap"), m_snapOn)) m_snapOn = !m_snapOn;
        tip(tr("Snap without holding Ctrl (Ctrl turns it off for a moment)"));
        ImGui::SameLine(0, 0);
        if (ImGui::ArrowButton("##snapSettings", ImGuiDir_Down)) ImGui::OpenPopup("##snapPopup");
        if (ImGui::BeginPopup("##snapPopup")) {
            if (drawSnapSettings(m_prefs)) savePreferences(m_prefs);
            ImGui::EndPopup();
        }
        ImGui::SameLine(0, 2);
        if (toolButton(tr("Grid"), m_showGrid)) m_showGrid = !m_showGrid;
        ImGui::SameLine();
        if (toolButton(m_ortho ? tr("Orthographic") : tr("Perspective"), false)) m_ortho = !m_ortho;
        tip(tr("Switch between perspective and orthographic (wheel zooms in orthographic)"));
        ImGui::SameLine();
        const char* views[] = {tr("Lit"), tr("Wireframe"), tr("Normals"), tr("LOD Level"), tr("Overdraw"),
                               tr("Over Budget"), tr("Material Features")};
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        if (ImGui::BeginCombo("##viewMode", views[static_cast<int>(m_debugView)])) {
            for (u32 i = 0; i < static_cast<u32>(render::DebugView::Count); ++i) {
                const auto view = static_cast<render::DebugView>(i);
                ImGui::BeginDisabled(!m_renderer->supportsDebugView(view));
                if (ImGui::Selectable(views[i], m_debugView == view)) m_debugView = view;
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        tip(tr("View Mode"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        ImGui::SliderFloat("##camSpeed", &m_cameraSpeed, 1.0f, 400.0f, "%.0f m/s", ImGuiSliderFlags_Logarithmic);
        tip(tr("Camera speed"));
    }
    ImGui::End();
    ImGui::PopStyleVar();

    // Axis gizmo: when ImGuizmo rotates the view matrix, convert back to camera angles/position
    const f32 size = 96.0f;
    Mat4 view = m_view;
    const f32 pivot = 10.0f; // orbits around the point this far ahead
    ImGuizmo::ViewManipulate(&view.m[0][0], pivot, ImVec2(m_viewport.x + m_viewport.width - size - 8.0f, m_viewport.y + 8.0f),
                             ImVec2(size, size), IM_COL32(30, 30, 34, 120));
    if (ImGuizmo::IsUsingViewManipulate() || std::memcmp(&view, &m_view, sizeof(view)) != 0) {
        // Row vector view matrix: column 2 is forward, camera position = -t * R^T
        const Vec3 forward = normalize(Vec3{view.m[0][2], view.m[1][2], view.m[2][2]});
        const Vec3 t{view.m[3][0], view.m[3][1], view.m[3][2]};
        m_cameraPos = {-(t.x * view.m[0][0] + t.y * view.m[0][1] + t.z * view.m[0][2]),
                       -(t.x * view.m[1][0] + t.y * view.m[1][1] + t.z * view.m[1][2]),
                       -(t.x * view.m[2][0] + t.y * view.m[2][1] + t.z * view.m[2][2])};
        m_cameraYaw = std::atan2(forward.x, forward.z);
        m_cameraPitch = std::clamp(std::asin(std::clamp(forward.y, -1.0f, 1.0f)), -1.55f, 1.55f);
        m_view = view;
    }
}

// Floor (y = 0) grid: only around under the camera, spaced by the snap move unit. Drawn as thin boxes so it has depth
void Editor::addGrid(render::DrawList& out) const {
    f32 cell = m_prefs.snapMove; // snap move unit; grouped if too dense
    while (cell < 0.25f) cell *= 2.0f;
    const int half = 40;
    const f32 cx = std::round(m_cameraPos.x / cell) * cell, cz = std::round(m_cameraPos.z / cell) * cell;
    const f32 length = cell * half * 2.0f, thick = cell * 0.025f;
    const render::MeshId box = m_models->boxMesh();
    for (int i = -half; i <= half; ++i) {
        const bool major = (static_cast<int>(std::round((cx + i * cell) / cell)) % 10) == 0;
        render::InstanceData line;
        line.color = major ? 0xFF303030 : 0xFF585858;
        line.position = {cx + i * cell, 0.01f, cz};
        line.scale = {major ? thick * 2.0f : thick, 0.004f, length};
        out.add(box, line);
        const bool majorZ = (static_cast<int>(std::round((cz + i * cell) / cell)) % 10) == 0;
        line.color = majorZ ? 0xFF303030 : 0xFF585858;
        line.position = {cx, 0.01f, cz + i * cell};
        line.scale = {length, 0.004f, majorZ ? thick * 2.0f : thick};
        out.add(box, line);
    }
}

// ---------------------------------------------------------------- Input

Vec3 Editor::cameraForward() const {
    return {std::sin(m_cameraYaw) * std::cos(m_cameraPitch), std::sin(m_cameraPitch),
            std::cos(m_cameraYaw) * std::cos(m_cameraPitch)};
}

void Editor::updateCamera(f64 dt) {
    ImGuiIO& io = ImGui::GetIO();
    if (m_mode != Mode::Edit) {
        m_looking = false;
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        return;
    }
    const ImVec2 mouse = io.MousePos;
    const bool inViewport = mouse.x >= m_viewport.x && mouse.y >= m_viewport.y &&
                            mouse.x < m_viewport.x + m_viewport.width && mouse.y < m_viewport.y + m_viewport.height;

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && inViewport && !io.WantCaptureMouse) m_looking = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) m_looking = false;
    m_window->setCursorCaptured(m_looking);

    const Vec3 forward = cameraForward();
    const Vec3 right = normalize(cross({0, 1, 0}, forward));
    const Vec3 up = cross(forward, right);
    Input& input = m_window->input();

    if (m_looking) {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        m_cameraYaw += input.mouseDeltaX() * 0.0035f;
        m_cameraPitch = std::clamp(m_cameraPitch - input.mouseDeltaY() * 0.0035f, -1.55f, 1.55f);
        if (io.MouseWheel != 0.0f)
            m_cameraSpeed = std::clamp(m_cameraSpeed * std::pow(1.25f, io.MouseWheel), 1.0f, 400.0f);
        Vec3 move{};
        if (ImGui::IsKeyDown(ImGuiKey_W)) move += forward;
        if (ImGui::IsKeyDown(ImGuiKey_S)) move -= forward;
        if (ImGui::IsKeyDown(ImGuiKey_D)) move += right;
        if (ImGui::IsKeyDown(ImGuiKey_A)) move -= right;
        if (ImGui::IsKeyDown(ImGuiKey_E)) move += Vec3{0, 1, 0};
        if (ImGui::IsKeyDown(ImGuiKey_Q)) move -= Vec3{0, 1, 0};
        const f32 speed = m_cameraSpeed * (io.KeyShift ? 3.0f : 1.0f);
        m_cameraPos += normalize(move) * (speed * static_cast<f32>(dt));
    } else {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        // Alt + left drag: orbit around the selected object (or 10 m ahead if none)
        if (io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && inViewport && !io.WantCaptureMouse) {
            const Entity* e = selected();
            m_orbitPivot = e ? scene::worldTransform(m_scene, *e).position : m_cameraPos + forward * 10.0f;
            m_orbiting = true;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_orbiting = false;
        if (m_orbiting) {
            const f32 dist = length(m_orbitPivot - m_cameraPos);
            m_cameraYaw += io.MouseDelta.x * 0.006f;
            m_cameraPitch = std::clamp(m_cameraPitch - io.MouseDelta.y * 0.006f, -1.55f, 1.55f);
            m_cameraPos = m_orbitPivot - cameraForward() * dist;
        } else if (inViewport && !io.WantCaptureMouse) {
            if (io.MouseWheel != 0.0f) {
                if (m_ortho) { // orthographic: screen size
                    m_orthoHalfHeight = std::clamp(m_orthoHalfHeight * std::pow(0.85f, io.MouseWheel), 0.5f, 500.0f);
                } else { // toward where the mouse points (15% of the distance at a time if it hits an object)
                    Vec3 origin, dir;
                    mouseRay(mouse.x, mouse.y, &origin, &dir);
                    f32 hit = 0.0f;
                    const f32 step = raycast(origin, dir, &hit) ? std::max(0.5f, hit * 0.15f) : 3.0f;
                    m_cameraPos += dir * (io.MouseWheel * step);
                }
            }
            if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) { // middle button drag: pan
                const f32 scale = m_ortho ? m_orthoHalfHeight * 2.0f / std::max(m_viewport.height, 1.0f) : 0.03f;
                m_cameraPos += (right * -input.mouseDeltaX() + up * input.mouseDeltaY()) * scale;
            }
        }
    }
}

Mat4 Editor::projection(f32 aspect) const {
    if (!m_ortho) return perspectiveReversedZ(kFovY, aspect, 0.1f, 2000.0f);
    // Reversed-Z orthographic (row vectors, left-handed): near plane = 1, far plane = 0
    constexpr f32 zNear = -500.0f, zFar = 2000.0f; // so objects behind the camera are visible too
    Mat4 r;
    r.m[0][0] = 1.0f / (m_orthoHalfHeight * aspect);
    r.m[1][1] = 1.0f / m_orthoHalfHeight;
    r.m[2][2] = -1.0f / (zFar - zNear);
    r.m[3][2] = zFar / (zFar - zNear);
    r.m[3][3] = 1.0f;
    return r;
}

void Editor::mouseRay(f32 mouseX, f32 mouseY, Vec3* origin, Vec3* dir) const {
    const f32 nx = (mouseX - m_viewport.x) / std::max(m_viewport.width, 1.0f) * 2.0f - 1.0f;
    const f32 ny = 1.0f - (mouseY - m_viewport.y) / std::max(m_viewport.height, 1.0f) * 2.0f;
    const f32 aspect = m_viewport.width / std::max(m_viewport.height, 1.0f);
    const Vec3 forward = cameraForward();
    const Vec3 right = normalize(cross({0, 1, 0}, forward));
    const Vec3 up = cross(forward, right);
    if (m_ortho) {
        *origin = m_cameraPos + right * (nx * m_orthoHalfHeight * aspect) + up * (ny * m_orthoHalfHeight) - forward * 500.0f;
        *dir = forward;
        return;
    }
    const f32 tanHalf = std::tan(kFovY * 0.5f);
    *origin = m_cameraPos;
    *dir = normalize(forward + right * (nx * tanHalf * aspect) + up * (ny * tanHalf));
}

// Number keys 1~9: go to a saved camera position; Ctrl+number: save the current position (per scene in build\editor\<scene>.view)
void Editor::handleBookmarks() {
    const ImGuiIO& io = ImGui::GetIO();
    if (m_mode != Mode::Edit || io.WantTextInput || m_looking || io.KeyAlt || io.KeyShift) return;
    for (int i = 0; i < 9; ++i) {
        if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + i), false)) continue;
        CameraBookmark& b = m_bookmarks[i];
        if (io.KeyCtrl) {
            b = {true, m_cameraPos, m_cameraYaw, m_cameraPitch};
            saveViewState();
            setStatus(trf("Camera bookmark %d saved", i + 1));
        } else if (b.set) {
            m_cameraPos = b.position;
            m_cameraYaw = b.yaw;
            m_cameraPitch = b.pitch;
        } else {
            setStatus(trf("No camera bookmark %d (Ctrl+%d saves one)", i + 1, i + 1));
        }
    }
}

// Left click = select (Shift = add/remove), drag on empty space = select objects inside the box
void Editor::handlePicking() {
    ImGuiIO& io = ImGui::GetIO();
    if (m_boxSelecting) {
        const Vec2 a = m_boxStart, b{io.MousePos.x, io.MousePos.y};
        const ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y)), hi(std::max(a.x, b.x), std::max(a.y, b.y));
        const bool dragged = hi.x - lo.x > 4.0f || hi.y - lo.y > 4.0f;
        if (dragged) {
            ImDrawList* draw = ImGui::GetForegroundDrawList();
            draw->AddRectFilled(lo, hi, IM_COL32(90, 150, 255, 40));
            draw->AddRect(lo, hi, IM_COL32(90, 150, 255, 200));
        }
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
        m_boxSelecting = false;
        if (dragged) {
            const Mat4 viewProj = m_view * m_proj;
            std::vector<u32> inside;
            for (const Entity& e : m_scene.entities) {
                if (isLocked(e) || isHidden(e)) continue;
                const Vec3 p = scene::worldTransform(m_scene, e).position;
                const f32 cx = p.x * viewProj.m[0][0] + p.y * viewProj.m[1][0] + p.z * viewProj.m[2][0] + viewProj.m[3][0];
                const f32 cy = p.x * viewProj.m[0][1] + p.y * viewProj.m[1][1] + p.z * viewProj.m[2][1] + viewProj.m[3][1];
                const f32 cw = p.x * viewProj.m[0][3] + p.y * viewProj.m[1][3] + p.z * viewProj.m[2][3] + viewProj.m[3][3];
                if (cw <= 0.0f) continue; // behind the camera
                const f32 sx = m_viewport.x + (cx / cw * 0.5f + 0.5f) * m_viewport.width;
                const f32 sy = m_viewport.y + (0.5f - cy / cw * 0.5f) * m_viewport.height;
                if (sx >= lo.x && sx <= hi.x && sy >= lo.y && sy <= hi.y) inside.push_back(e.id);
            }
            selectMany(inside, io.KeyShift);
            return;
        }
    } else {
        if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left) || io.WantCaptureMouse || io.KeyAlt || ImGuizmo::IsOver() ||
            ImGuizmo::IsUsing() || ImGuizmo::IsViewManipulateHovered() || ImGuizmo::IsUsingViewManipulate())
            return;
        const ImVec2 mouse = io.MousePos;
        if (mouse.x < m_viewport.x || mouse.y < m_viewport.y || mouse.x >= m_viewport.x + m_viewport.width ||
            mouse.y >= m_viewport.y + m_viewport.height)
            return;
        m_boxSelecting = true; // decide on release whether it was a click or a drag
        m_boxStart = {mouse.x, mouse.y};
        return;
    }
    Vec3 origin, dir;
    mouseRay(m_boxStart.x, m_boxStart.y, &origin, &dir);
    f32 distance = 0.0f;
    const u32 best = raycast(origin, dir, &distance);
    if (io.KeyShift) {
        if (best) toggleSelect(best);
    } else {
        m_selected = best;
    }
}

u32 Editor::raycast(Vec3 origin, Vec3 dir, f32* distance, const std::vector<u32>& skip) const {
    u32 best = 0;
    f32 bestT = 1e30f;
    for (const Entity& e : m_scene.entities) {
        if (isLocked(e) || isHidden(e) || std::find(skip.begin(), skip.end(), e.id) != skip.end()) continue;
        const scene::Transform w = scene::worldTransform(m_scene, e);
        if (isArea(e)) { // areas are selected by clicking edges (clicking inside selects objects inside)
            Vec3 centers[12], halves[12];
            areaEdges(w.scale * 0.5f, kAreaEdgePick, centers, halves);
            for (int i = 0; i < 12; ++i) {
                const f32 t = rayBox(origin, dir, w, centers[i], halves[i]);
                if (t >= 0.0f && t < bestT) {
                    bestT = t;
                    best = e.id;
                }
            }
            continue;
        }
        Vec3 center, half;
        pickBounds(e, w, &center, &half);
        const f32 t = rayBox(origin, dir, w, center, half);
        if (t >= 0.0f && t < bestT) {
            bestT = t;
            best = e.id;
        }
    }
    *distance = bestT;
    return best;
}

// Box used for picking/snapping to the floor (object-relative, before rotation): models use their bounding box, markers a small box
void Editor::pickBounds(const Entity& e, const scene::Transform& w, Vec3* center, Vec3* half) const {
    *center = {};
    *half = (isMarker(e) ? kSpawnSize : w.scale) * 0.5f;
    if (const asset::Model* model = e.model.empty() ? nullptr : m_models->get(e.model)) {
        const Vec3 c = (model->boundsMin + model->boundsMax) * 0.5f, h = (model->boundsMax - model->boundsMin) * 0.5f;
        *center = {c.x * w.scale.x, c.y * w.scale.y, c.z * w.scale.z};
        *half = {std::fabs(h.x * w.scale.x), std::fabs(h.y * w.scale.y), std::fabs(h.z * w.scale.z)};
    }
}

void Editor::dropToFloor() {
    const std::vector<u32> roots = selectionRoots();
    if (roots.empty()) return;
    pushUndo(capture());
    for (u32 id : roots) {
        Entity* e = m_scene.find(id);
        scene::Transform w = scene::worldTransform(m_scene, *e);
        Vec3 center, half;
        pickBounds(*e, w, &center, &half);
        // Height down to the lowest point of the rotated box
        f32 below = 0.0f;
        for (int k = 0; k < 8; ++k) {
            const Vec3 corner{k & 1 ? half.x : -half.x, k & 2 ? half.y : -half.y, k & 4 ? half.z : -half.z};
            below = std::max(below, -rotate(w.rotation, center + corner).y);
        }
        std::vector<u32> skip = scene::subtree(m_scene, id);
        f32 t = 0.0f;
        const Vec3 from{w.position.x, w.position.y - below + 0.01f, w.position.z};
        f32 floorY = 0.0f;
        if (raycast(from, {0.0f, -1.0f, 0.0f}, &t, skip)) floorY = from.y - t;
        else if (from.y < 0.0f) floorY = 0.0f;
        w.position.y = floorY + below;
        scene::setWorldTransform(m_scene, *e, w);
    }
    setStatus(tr("Dropped to the floor"));
}

void Editor::alignSelection(int axis) {
    const Entity* primary = selected();
    if (!primary || m_multi.empty()) return;
    pushUndo(capture());
    const Vec3 target = scene::worldTransform(m_scene, *primary).position;
    for (u32 id : selectionRoots()) {
        if (id == m_selected) continue;
        Entity* e = m_scene.find(id);
        scene::Transform w = scene::worldTransform(m_scene, *e);
        (&w.position.x)[axis] = (&target.x)[axis];
        scene::setWorldTransform(m_scene, *e, w);
    }
}

void Editor::distributeSelection() {
    std::vector<u32> roots = selectionRoots();
    if (roots.size() < 3) return;
    pushUndo(capture());
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (u32 id : roots) {
        const Vec3 p = scene::worldTransform(m_scene, *m_scene.find(id)).position;
        for (int a = 0; a < 3; ++a) {
            (&lo.x)[a] = std::min((&lo.x)[a], (&p.x)[a]);
            (&hi.x)[a] = std::max((&hi.x)[a], (&p.x)[a]);
        }
    }
    int axis = 0;
    for (int a = 1; a < 3; ++a)
        if ((&hi.x)[a] - (&lo.x)[a] > (&hi.x)[axis] - (&lo.x)[axis]) axis = a;
    auto key = [&](u32 id) {
        const Vec3 p = scene::worldTransform(m_scene, *m_scene.find(id)).position;
        return (&p.x)[axis];
    };
    std::sort(roots.begin(), roots.end(), [&](u32 a, u32 b) { return key(a) < key(b); });
    const f32 step = ((&hi.x)[axis] - (&lo.x)[axis]) / static_cast<f32>(roots.size() - 1);
    for (size_t i = 0; i < roots.size(); ++i) {
        Entity* e = m_scene.find(roots[i]);
        scene::Transform w = scene::worldTransform(m_scene, *e);
        (&w.position.x)[axis] = (&lo.x)[axis] + step * static_cast<f32>(i);
        scene::setWorldTransform(m_scene, *e, w);
    }
}

bool Editor::isSelected(u32 id) const {
    return id && (id == m_selected || std::find(m_multi.begin(), m_multi.end(), id) != m_multi.end());
}

std::vector<u32> Editor::selectionRoots() const {
    std::vector<u32> all;
    if (m_selected) all.push_back(m_selected);
    all.insert(all.end(), m_multi.begin(), m_multi.end());
    std::vector<u32> roots;
    for (u32 id : all) {
        if (!m_scene.find(id)) continue;
        bool under = false;
        for (u32 other : all)
            under |= other != id && scene::isDescendant(m_scene, id, other);
        if (!under) roots.push_back(id);
    }
    return roots;
}

void Editor::toggleSelect(u32 id) {
    if (id == m_selected) { // removing the primary selection makes the next one primary
        m_selected = m_multi.empty() ? 0 : m_multi.front();
        if (!m_multi.empty()) m_multi.erase(m_multi.begin());
    } else if (auto it = std::find(m_multi.begin(), m_multi.end(), id); it != m_multi.end()) {
        m_multi.erase(it);
    } else {
        if (m_selected) m_multi.push_back(m_selected);
        m_selected = id;
    }
    m_multiOwner = m_selected;
}

void Editor::selectMany(const std::vector<u32>& ids, bool add) {
    if (!add) {
        m_selected = 0;
        m_multi.clear();
    }
    for (u32 id : ids)
        if (!isSelected(id)) {
            if (m_selected) m_multi.push_back(m_selected);
            m_selected = id;
        }
    m_multiOwner = m_selected;
}

// Moves the other selected objects (their top-level ones) by as much as the primary selection's world transform changed from before → after.
// Rotation around the primary selection's position, scale by each one's ratio
void Editor::moveOthersWith(const scene::Transform& before, const scene::Transform& after) {
    const Quat inv{-before.rotation.x, -before.rotation.y, -before.rotation.z, before.rotation.w};
    const Quat delta = normalize(after.rotation * inv);
    const Vec3 ratio{after.scale.x / (before.scale.x != 0.0f ? before.scale.x : 1.0f),
                     after.scale.y / (before.scale.y != 0.0f ? before.scale.y : 1.0f),
                     after.scale.z / (before.scale.z != 0.0f ? before.scale.z : 1.0f)};
    for (u32 id : selectionRoots()) {
        if (id == m_selected || scene::isDescendant(m_scene, id, m_selected)) continue;
        Entity* o = m_scene.find(id);
        scene::Transform w = scene::worldTransform(m_scene, *o);
        w.position = after.position + rotate(delta, w.position - before.position);
        w.rotation = normalize(delta * w.rotation);
        w.scale = {w.scale.x * ratio.x, w.scale.y * ratio.y, w.scale.z * ratio.z};
        scene::setWorldTransform(m_scene, *o, w);
    }
}

// ---------------------------------------------------------------- Edit actions

Entity* Editor::selected() { return m_selected ? m_scene.find(m_selected) : nullptr; }

const reflect::TypeInfo* Editor::entityType(const Entity& e) const { return scene::findType(e.type, m_gameTypes); }

bool Editor::isHidden(const Entity& e) const {
    for (const Entity* p = &e; p; p = p->parent ? m_scene.find(p->parent) : nullptr)
        if (m_hidden.count(p->id)) return true;
    return false;
}

bool Editor::isLocked(const Entity& e) const {
    for (const Entity* p = &e; p; p = p->parent ? m_scene.find(p->parent) : nullptr)
        if (m_locked.count(p->id)) return true;
    return false;
}

const char* Editor::entityIcon(const Entity& e) const {
    if (e.is(scene::kTypePointLight)) return icon::kLight;
    if (e.is(scene::kTypeVirtualCamera)) return icon::kCamera;
    if (e.is(scene::kTypeProbe)) return icon::kProbe;
    if (isArea(e)) return icon::kArea;
    if (isMarker(e)) return icon::kPin;
    if (!e.model.empty()) return icon::kModel;
    if (e.is(scene::kTypeSolid) || e.is(scene::kTypeDecoration)) return icon::kBox;
    return icon::kFlag;
}

std::string Editor::viewStatePath() const {
    if (m_project.directory.empty() || m_scenePath.empty()) return {};
    return m_project.directory + "\\build\\editor\\" + fileName(m_scenePath) + ".view";
}

// Two lines "hidden 3 5" / "locked 7" + camera bookmarks "camera <1~9> x y z yaw pitch"
void Editor::loadViewState() {
    m_hidden.clear();
    m_locked.clear();
    for (CameraBookmark& b : m_bookmarks) b = {};
    std::string text;
    const std::string path = viewStatePath();
    if (path.empty() || !readTextFile(path, text)) return;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        int slot = 0;
        CameraBookmark b;
        if (sscanf(line.c_str(), "camera %d %f %f %f %f %f", &slot, &b.position.x, &b.position.y, &b.position.z, &b.yaw,
                   &b.pitch) == 6 &&
            slot >= 1 && slot <= 9) {
            b.set = true;
            m_bookmarks[slot - 1] = b;
            continue;
        }
        auto* set = line.rfind("hidden", 0) == 0 ? &m_hidden : line.rfind("locked", 0) == 0 ? &m_locked : nullptr;
        if (!set) continue;
        const char* c = line.c_str() + 6;
        char* next = nullptr;
        for (u32 id = strtoul(c, &next, 10); next != c; id = strtoul(c, &next, 10)) {
            if (id) set->insert(id);
            c = next;
        }
    }
}

void Editor::saveViewState() const {
    const std::string path = viewStatePath();
    if (path.empty()) return;
    std::string text;
    for (auto [name, set] : {std::pair{"hidden", &m_hidden}, std::pair{"locked", &m_locked}}) {
        std::vector<u32> ids(set->begin(), set->end());
        std::sort(ids.begin(), ids.end());
        text += name;
        for (u32 id : ids) text += " " + std::to_string(id);
        text += "\n";
    }
    for (int i = 0; i < 9; ++i) {
        const CameraBookmark& b = m_bookmarks[i];
        if (!b.set) continue;
        char line[160];
        snprintf(line, sizeof(line), "camera %d %.3f %.3f %.3f %.4f %.4f\n", i + 1, b.position.x, b.position.y,
                 b.position.z, b.yaw, b.pitch);
        text += line;
    }
    createDirectories(m_project.directory + "\\build\\editor");
    writeTextFile(path, text);
}

bool Editor::isMarker(const Entity& e) const {
    const reflect::TypeInfo* type = entityType(e);
    return type && (type->flags & reflect::kObjectMarker);
}

bool Editor::isArea(const Entity& e) const {
    const reflect::TypeInfo* type = entityType(e);
    return type && (type->flags & reflect::kObjectArea);
}

std::vector<const reflect::TypeInfo*> Editor::allTypes() const {
    std::vector<const reflect::TypeInfo*> list = scene::builtinTypes();
    if (m_gameTypes) {
        std::vector<const reflect::TypeInfo*> game = *m_gameTypes;
        std::sort(game.begin(), game.end(), [](const reflect::TypeInfo* a, const reflect::TypeInfo* b) {
            return std::strcmp(a->label, b->label) < 0;
        });
        list.insert(list.end(), game.begin(), game.end());
    }
    return list;
}

void Editor::drawTypeMenuItems() {
    const size_t builtins = scene::builtinTypes().size();
    const std::vector<const reflect::TypeInfo*> types = allTypes();
    for (size_t i = 0; i < types.size(); ++i) {
        if (i == builtins) ImGui::SeparatorText(tr("Game"));
        if (ImGui::MenuItem(trData(types[i]->label))) addEntity(types[i]->name);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", types[i]->name);
    }
    if (!m_gameTypes) ImGui::TextDisabled("%s", tr("(game types appear when the game code is loaded)"));
}

// Game kind fields: read text values into an instance of the declared type, edit with widgets, and write back as text
void Editor::drawObjectFields(Entity& e) {
    const reflect::TypeInfo* type = entityType(e);
    if (type && type->fields.empty()) return;
    if (!ImGui::CollapsingHeader(tr("Fields"), ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (!type) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f), tr("The game code has no '%s' type. The values are kept as they are."),
                           e.type.c_str());
        for (auto& [path, value] : e.fields) {
            char buffer[256];
            strncpy_s(buffer, value.c_str(), _TRUNCATE);
            if (ImGui::InputText(path.c_str(), buffer, sizeof(buffer))) {
                value = buffer;
                markChanged();
            }
        }
        return;
    }
    reflect::Instance instance(*type);
    std::string warnings;
    reflect::fromText(*type, instance.data(), e.fields, &warnings);
    const reflect::Instance defaults(*type);
    const FieldContext context{&m_tablesPanel.index(), defaults.data()};
    if (drawFields(*type, instance.data(), &context)) {
        reflect::TextFields fields;
        reflect::toText(*type, instance.data(), fields);
        e.fields = std::move(fields);
        markChanged();
    }
    if (!warnings.empty()) ImGui::TextDisabled(tr("Undeclared fields: %s (removed on save)"), warnings.c_str());
}

void Editor::addEntity(const std::string& typeName) {
    const reflect::TypeInfo* type = scene::findType(typeName, m_gameTypes);
    if (!type) return;
    pushUndo(capture());
    const Vec3 forward = cameraForward();
    Vec3 pos = m_cameraPos + forward * 12.0f;
    if (forward.y < -0.05f) {
        const f32 t = -m_cameraPos.y / forward.y;
        if (t > 0.0f && t < 80.0f) pos = m_cameraPos + forward * t;
    }
    const bool solid = typeName == scene::kTypeSolid;
    Entity& e = m_scene.add(type->name, solid ? tr("Box") : trData(type->label));
    e.name += " " + std::to_string(e.id);
    e.fields = scene::defaultFields(*type);
    if (type->flags & reflect::kObjectMarker) {
        e.position = {pos.x, std::max(pos.y, 0.0f) + 0.9f, pos.z};
        e.color = 0xFF5ADCFF;
    } else if (type->flags & reflect::kObjectArea) {
        e.scale = {10, 4, 10};
        e.position = {pos.x, std::max(pos.y, 0.0f) + 2.0f, pos.z};
        e.color = 0xFF50C8FF;
    } else {
        e.scale = {2, 2, 2};
        e.position = {pos.x, std::max(pos.y, 0.0f) + 1.0f, pos.z};
        e.color = solid ? 0xFFD2C8C8 : typeName == scene::kTypeDecoration ? 0xFFDCAA78 : 0xFF8CD2F0;
    }
    m_selected = e.id;
    setStatus(trf("Added: %s", e.name.c_str()));
}

std::string Editor::chooseModel() {
    const std::string root = asset::ModelCache::assetsRoot();
    const std::string path = openFileDialog(m_window->nativeHandle(), "", root + "\\models", kModelFilters, 2);
    if (path.empty()) return {};
    // Inside the assets folder, saved as a relative path (so the same scene file works on other PCs)
    const std::string relative = relativeTo(root, path);
    if (!relative.empty()) return relative;
    setStatus(tr("The file is outside the assets folder. In multiplayer everyone needs it at the same path"));
    return path;
}

void Editor::addModelEntity() {
    const std::string path = chooseModel();
    if (!path.empty()) addModelEntity(path, nullptr);
}

void Editor::addModelEntity(const std::string& path, const Vec3* at) {
    const asset::Model* model = m_models->get(path);
    if (!model) {
        setStatus(trf("Could not load the model: %s", path.c_str()), true);
        return;
    }
    addEntity(scene::kTypeSolid);
    Entity* e = selected();
    e->model = path;
    e->scale = {1, 1, 1};
    e->color = 0xFFFFFFFF;                       // show the material color as is
    if (at) e->position = *at + Vec3{0.0f, 1.0f, 0.0f}; // where it was dropped
    e->position.y += -1.0f - model->boundsMin.y; // touch the floor (addEntity raises it by 1)
    const std::string file = fileName(path);
    e->name = file.substr(0, file.find_last_of('.'));
    setStatus(trf("Model added: %s", path.c_str()));
}

// Copies the selected objects and their children with the same structure
void Editor::duplicateSelected() {
    if (!selected()) return;
    pushUndo(capture());
    const std::vector<u32> roots = selectionRoots();
    std::vector<u32> newRoots;
    size_t total = 0;
    std::string name;
    for (u32 root : roots) {
        const std::vector<u32> ids = scene::subtree(m_scene, root);
        std::unordered_map<u32, u32> remap;
        for (u32 id : ids) remap[id] = m_scene.nextId++;
        std::vector<Entity> copies;
        for (u32 id : ids) {
            Entity copy = *m_scene.find(id);
            const bool top = id == root;
            copy.id = remap[id];
            copy.parent = top ? copy.parent : remap[copy.parent];
            if (top) {
                copy.name += std::string(" ") + tr("copy");
                copy.position.x += 1.0f;
            }
            copies.push_back(std::move(copy));
        }
        if (name.empty()) name = copies.front().name;
        newRoots.push_back(copies.front().id);
        total += ids.size();
        m_scene.entities.insert(m_scene.entities.end(), copies.begin(), copies.end());
    }
    selectMany(newRoots, false);
    if (roots.size() > 1) setStatus(trf("Duplicated %zu objects", roots.size()));
    else setStatus(total > 1 ? trf("Duplicated: %s (with %zu children)", name.c_str(), total - 1) : trf("Duplicated: %s", name.c_str()));
}

// Saves the selected objects and their children as a prefab file (assets/prefabs/)
void Editor::saveSelectionAsPrefab() {
    const Entity* e = selected();
    if (!e) return;
    const std::string root = asset::ModelCache::assetsRoot();
    createDirectories(root + "\\prefabs");
    const std::string path = saveFileDialog(m_window->nativeHandle(), root + "\\prefabs\\" + e->name + ".prefab",
                                            root + "\\prefabs", kPrefabFilters, 2, "prefab");
    if (path.empty()) return;
    const std::vector<u32> roots = selectionRoots();
    if (!scene::savePrefab(m_scene, roots, path)) {
        setStatus(trf("Could not save %s", path.c_str()), true);
        return;
    }
    const std::string relative = relativeTo(root, path);
    if (relative.empty()) setStatus(tr("The prefab is outside the assets folder. Only prefabs under assets/prefabs/ show in [Assets]"));
    else setStatus(trf("Prefab saved: %s", relative.c_str()));
}

// Adds copies of a prefab's objects (like duplicate: later edits to the file don't change them)
void Editor::placePrefab(const std::string& relative, const Vec3* at) {
    scene::Scene prefab;
    std::string error;
    const std::string path = asset::ModelCache::assetsRoot() + "\\" + relative;
    if (!scene::loadPrefab(prefab, path, &error)) {
        setStatus(trf("Could not load the prefab: %s (%s)", relative.c_str(), error.c_str()), true);
        return;
    }
    if (prefab.entities.empty()) return;
    pushUndo(capture());
    scene::Transform place;
    if (at) {
        place.position = *at;
    } else { // on the floor in front of the camera, like addEntity
        const Vec3 forward = cameraForward();
        place.position = m_cameraPos + forward * 12.0f;
        if (forward.y < -0.05f) {
            const f32 t = -m_cameraPos.y / forward.y;
            if (t > 0.0f && t < 80.0f) place.position = m_cameraPos + forward * t;
        }
        place.position.y = std::max(place.position.y, 0.0f);
    }
    const std::vector<u32> roots = scene::placePrefab(m_scene, prefab, place);
    selectMany(roots, false);
    setStatus(trf("Prefab placed: %s (%zu objects)", relative.c_str(), prefab.entities.size()));
}

// Deletes the selected objects and their children
void Editor::deleteSelected() {
    const Entity* e = selected();
    if (!e) return;
    pushUndo(capture());
    const std::vector<u32> roots = selectionRoots();
    std::vector<u32> ids;
    for (u32 root : roots) {
        const std::vector<u32> sub = scene::subtree(m_scene, root);
        ids.insert(ids.end(), sub.begin(), sub.end());
    }
    if (roots.size() > 1) setStatus(trf("Deleted %zu objects", roots.size()));
    else setStatus(ids.size() > 1 ? trf("Deleted: %s (with %zu children)", e->name.c_str(), ids.size() - 1) : trf("Deleted: %s", e->name.c_str()));
    std::erase_if(m_scene.entities,
                  [&](const Entity& x) { return std::find(ids.begin(), ids.end(), x.id) != ids.end(); });
    m_selected = 0;
    m_multi.clear();
}

void Editor::focusSelected() {
    const Entity* e = selected();
    if (!e) return;
    const scene::Transform w = scene::worldTransform(m_scene, *e);
    const f32 extent = std::max({w.scale.x, w.scale.y, w.scale.z});
    m_cameraPos = w.position - cameraForward() * (std::min(extent, 60.0f) * 1.2f + 6.0f);
}

void Editor::newScene() {
    pushUndo(capture());
    scene::Scene fresh;
    Entity& ground = fresh.add(scene::kTypeSolid, "Ground");
    ground.position = {0, -0.5f, 0};
    ground.scale = {120, 1, 120};
    ground.color = 0xFF609676;
    ground.flags = render::kInstanceChecker;
    Entity& spawn = fresh.add(scene::kTypeSpawn, "Spawn 1");
    spawn.position = {0, 0.9f, 0};
    spawn.color = 0xFF5ADCFF;
    m_scene = std::move(fresh);
    m_scenePath.clear();
    m_selected = 0;
    setStatus(tr("New scene (Ctrl+Z to undo)"));
}

void Editor::openSceneDialog() {
    const std::string path = openFileDialog(m_window->nativeHandle(), m_scenePath, "", kSceneFilters, 2);
    if (path.empty()) return;
    saveBackupIfDirty();
    loadScene(path);
}

void Editor::openReplayDialog() {
    // Default folder: this project's user folder replays/ (left after every play, game/InputRecord.h)
    const std::string folder = game::userFolder(m_project.settings) + "/" + game::kReplayFolder;
    const std::string path = openFileDialog(m_window->nativeHandle(), "", folder, kReplayFilters, 2);
    if (!path.empty()) requestReplay(path);
}

// Scenario test from editor play (game/Scenario.h): tests/scenarioN.rec (inputs up to tick) + tests/scenarioN.scenario
// with one == check per top-level state value at that tick.
void Editor::saveScenario(u32 tick) {
    const std::string dir = m_project.directory + "/tests";
    createDirectories(dir);
    std::string name;
    for (u32 i = 1; name.empty(); ++i)
        if (!fileExists(dir + "/scenario" + std::to_string(i) + ".scenario")) name = "scenario" + std::to_string(i);
    // Copy the recording (binary; it's flushed every tick)
    const std::string from = game::replayRecordingPath(game::userFolder(m_project.settings));
    std::FILE* in = openFile(from, "rb");
    std::FILE* out = in ? openFile(dir + "/" + name + ".rec", "wb") : nullptr;
    bool copied = in && out;
    char buffer[65536];
    for (size_t n; copied && (n = std::fread(buffer, 1, sizeof(buffer), in)) > 0;)
        copied = std::fwrite(buffer, 1, n, out) == n;
    if (in) std::fclose(in);
    if (out) std::fclose(out);
    if (!copied) {
        setStatus(trf("Could not save the scenario: %s", from.c_str()), true);
        return;
    }
    std::string text = "# Saved in editor play at tick " + std::to_string(tick) +
                       ". Keep the checks that state the rule and delete the rest.\n"
                       "# Run: <game>_headless --test tests/" + name + ".scenario (--update-expected rewrites the == values)\n";
    text += "inputs " + name + ".rec\n";
    text += "ticks " + std::to_string(tick + 1) + "\n";
    const std::string assets = m_project.directory + "/assets/";
    std::string scenePath = m_scenePath;
    std::replace(scenePath.begin(), scenePath.end(), '\\', '/');
    std::string assetsSlash = assets;
    std::replace(assetsSlash.begin(), assetsSlash.end(), '\\', '/');
    if (scenePath.rfind(assetsSlash, 0) == 0) text += "scene " + scenePath.substr(assetsSlash.size()) + "\n";
    for (const reflect::FieldInfo& f : m_stateView.type->fields) {
        const reflect::Kind kind = f.value->kind;
        if ((f.flags & reflect::kFieldView) || kind == reflect::Kind::Struct || kind == reflect::Kind::Array ||
            kind == reflect::Kind::Container)
            continue;
        const void* data = static_cast<const u8*>(m_stateView.data) + f.offset;
        text += "tick " + std::to_string(tick) + " " + f.name + " == " + reflect::formatScalar(*f.value, data) + "\n";
    }
    const std::string path = dir + "/" + name + ".scenario";
    if (!writeTextFile(path, text)) {
        setStatus(trf("Could not save the scenario: %s", path.c_str()), true);
        return;
    }
    MEALKIT_INFO("Scenario saved: %s", path.c_str());
    setStatus(trf("Scenario saved: tests/%s.scenario", name.c_str()));
}

bool Editor::loadScene(const std::string& path) {
    scene::Scene loaded;
    std::string error;
    if (!scene::load(loaded, path, &error)) {
        MEALKIT_ERROR("Cannot open scene %s: %s", path.c_str(), error.c_str());
        setStatus(trf("Could not open: %s", error.c_str()), true);
        return false;
    }
    m_scene = std::move(loaded);
    m_scenePath = path;
    loadViewState();
    m_sceneTime = fileWriteTime(path);
    rememberRecentScene(path);
    m_sceneChangedWarned = false;
    m_dirty = false;
    m_autosavePending = false;
    // If last time ended without saving or crashed, the autosave is newer
    const std::string autosave = path + ".autosave";
    m_recoverPath = fileExists(autosave) && fileWriteTime(autosave) > fileWriteTime(path) ? autosave : std::string();
    m_undo.clear();
    m_redo.clear();
    m_selected = 0;
    m_nameBufferId = 0;
    setStatus(trf("Opened: %s", fileName(path).c_str()));
    return true;
}

bool Editor::saveScene() {
    if (m_scenePath.empty()) return saveSceneAs();
    if (!scene::save(m_scene, m_scenePath)) {
        setStatus(trf("Could not save: %s", m_scenePath.c_str()), true);
        return false;
    }
    m_dirty = false;
    m_sceneTime = fileWriteTime(m_scenePath);
    m_sceneChangedWarned = false;
    m_autosavePending = false;
    removeFile(m_scenePath + ".autosave"); // saved, so the autosave isn't needed
    setStatus(trf("Saved: %s", fileName(m_scenePath).c_str()));
    return true;
}

bool Editor::saveSceneAs() {
    const std::string path = saveFileDialog(m_window->nativeHandle(), m_scenePath, "", kSceneFilters, 2, "scene");
    if (path.empty()) return false;
    m_scenePath = path;
    return saveScene();
}

Editor::Snapshot Editor::capture() const { return {m_scene.entities, m_scene.environment, m_scene.nextId, m_selected}; }

void Editor::restore(const Snapshot& s) {
    m_scene.entities = s.entities;
    m_scene.environment = s.environment;
    m_scene.nextId = s.nextId;
    m_selected = m_scene.find(s.selected) ? s.selected : 0;
    m_nameBufferId = 0;
    m_dirty = true;
}

void Editor::pushUndo(Snapshot snapshot) {
    if (snapshot.label.empty()) {
        const Entity* e = selected();
        snapshot.label = e ? trf("%s changed", e->name.c_str()) : std::string(tr("Scene changed"));
    }
    m_undoLabelFrame = ImGui::GetFrameCount();
    m_undo.push_back(std::move(snapshot));
    if (m_undo.size() > kUndoLimit) m_undo.erase(m_undo.begin());
    m_redo.clear();
    m_dirty = true;
}

void Editor::undo() {
    if (m_undo.empty()) return;
    m_redo.push_back(capture());
    m_redo.back().label = m_undo.back().label;
    restore(m_undo.back());
    m_undo.pop_back();
    setStatus(tr("Undo"));
}

void Editor::redo() {
    if (m_redo.empty()) return;
    m_undo.push_back(capture());
    m_undo.back().label = m_redo.back().label;
    restore(m_redo.back());
    m_redo.pop_back();
    setStatus(tr("Redo"));
}

void Editor::setStatus(const std::string& text, bool failed, NoticeOpen open) {
    m_status = text;
    m_statusTime = m_time;
    if (m_undoLabelFrame == ImGui::GetFrameCount() && !m_undo.empty()) m_undo.back().label = text; // names like duplicate/delete
    if (m_notices.size() >= 5) m_notices.erase(m_notices.begin());
    m_notices.push_back({text, m_time, failed, failed ? open : NoticeOpen::None});
}

void Editor::updateTitle() {
    std::string title = std::string("MealKit Editor ") + kEngineVersion + " - ";
    if (!m_project.name().empty()) title += m_project.name() + " - ";
    title += m_scenePath.empty() ? std::string(tr("Untitled")) : fileName(m_scenePath);
    if (m_dirty) title += " *";
    if (m_tuningPanel.dirty()) title += std::string("  (") + tr("Tuning") + " *)";
    if (m_projectPanel.dirty()) title += std::string("  (") + tr("Project Settings") + " *)";
    if (m_mode == Mode::Play) title += std::string("  [") + tr("Playing") + "]";
    if (title != m_title) {
        m_window->setTitle(title.c_str());
        m_title = title;
    }
}

// ---------------------------------------------------------------- Project builds

void Editor::setProject(const ProjectInfo& project) {
    m_project = project;
    loadViewState(); // the scene opens before the project
    loadRecentScenes();
    if (!m_scenePath.empty()) rememberRecentScene(m_scenePath);
    // Window layout per project (build\editor\layout.ini). A project opened for the first time gets the default layout
    if (m_prefs.projectLayout && !project.directory.empty()) {
        const std::string folder = project.directory + "\\build\\editor";
        createDirectories(folder);
        if (!m_imgui.setIniPath(folder + "\\layout.ini")) m_layoutReset = true;
    }
    m_tablesPanel.setProjectDirectory(project.directory);
    m_tuningPanel.setProjectDirectory(project.directory);
    m_feedbackPanel.setProjectDirectory(project.directory);
    m_uiPanel.setProject(project.settings.language);
    applyRenderStyle(project.settings);
    live::ServerConfig lc;
    lc.kind = "editor";
    lc.projectDir = project.directory;
    lc.projectName = project.name();
    lc.port = live::kEditorPort;
    lc.token = m_penpotToken;
    m_live.start(lc);
    m_live.addTopic("stats"); // performance numbers in the editor status bar (game::FrameStats::json)
    audio::addLiveService(m_live, [this] { return m_audio; }); // MealKit Audio: playback in the editor, reload, status
    m_tools = findTools(m_toolPaths);
    if (!m_engineDirectory.empty()) {
        ImportProject ip;
        ip.engineDir = m_engineDirectory;
        ip.projectDir = project.directory;
        ip.name = project.name();
        ip.language = project.settings.language;
        m_import.attach(m_live, ip);
        m_uiPanel.setPenpotUrl(m_import.pluginUrl());
    }
    m_projectPanel.revert(project);
    // Don't ask for the engine repo itself (no engine)
    const game::ProjectSettings& s = project.settings;
    m_upgradeOffer = !s.engine.empty() && s.engineVersion != kEngineVersion;
    if (m_upgradeOffer)
        MEALKIT_WARN("Project template is from engine %s, this engine is %s - tools\\upgrade_project.ps1 can update it",
                     s.engineVersion.empty() ? "(unknown)" : s.engineVersion.c_str(), kEngineVersion);
    startLayoutCheck();
}

// Folder layout check (docs/CODING_MODEL.md "Project folder layout"): the rules live in one place, the engine's tools\check_layout.ps1, and give the same result as check.ps1/Export
void Editor::startLayoutCheck() {
    if (m_engineDirectory.empty() || m_project.directory.empty() || m_layoutCheck.running()) return;
    const std::string script = m_engineDirectory + "\\tools\\check_layout.ps1";
    if (!fileExists(script)) return;
    m_layoutOutput.clear();
    m_layoutCheck.start("powershell -NoProfile -ExecutionPolicy Bypass -File \"" + script + "\" -Project \"" +
                            m_project.directory + "\"",
                        m_project.directory);
}

void Editor::pollLayoutCheck() {
    if (!m_layoutCheck.running()) return;
    if (m_layoutCheck.poll(m_layoutOutput)) return;
    std::string problems;
    u32 errors = 0, warnings = 0;
    size_t start = 0;
    while (start < m_layoutOutput.size()) {
        size_t end = m_layoutOutput.find('\n', start);
        if (end == std::string::npos) end = m_layoutOutput.size();
        const std::string line = m_layoutOutput.substr(start, end - start);
        start = end + 1;
        const bool error = line.rfind("error:", 0) == 0, warning = line.rfind("warning:", 0) == 0;
        if (!error && !warning) continue;
        errors += error ? 1u : 0u;
        warnings += warning ? 1u : 0u;
        if (error) MEALKIT_ERROR("Layout %s", line.c_str());
        else MEALKIT_WARN("Layout %s", line.c_str());
        problems += line + "\n";
    }
    if (errors)
        showMessage(trf("The project folder layout has %u problem(s). Release builds fail until they are fixed:\n\n%s",
                        static_cast<unsigned>(errors), problems.c_str()));
    else if (warnings)
        setStatus(trf("Project layout: %u warning(s) - see the log (tools\\upgrade_project.ps1 creates missing folders)",
                      static_cast<unsigned>(warnings)));
}

void Editor::setGameCodeStatus(bool loaded, const std::string& text) {
    if (loaded && m_gameLoaded && text != m_gameStatus) setStatus(text); // hot reload
    m_gameLoaded = loaded;
    m_gameStatus = text;
}

bool Editor::takeGameCodeBuilt() {
    const bool built = m_gameCodeBuilt;
    m_gameCodeBuilt = false;
    return built;
}

bool Editor::takeGameCodeBuildFailed() {
    const bool failed = m_gameCodeFailed;
    m_gameCodeFailed = false;
    return failed;
}

// Shipping copies assets and mealkit.project from disk, so unsaved scenes/project settings are saved first
void Editor::startBuild(BuildKind kind) {
    if (m_buildPanel.running() || m_project.directory.empty()) return;
    if (kind == BuildKind::Export && m_dirty && !m_scenePath.empty() && !saveScene()) return;
    if (kind == BuildKind::Export && m_projectPanel.dirty() && !m_projectPanel.save(m_project)) return;
    m_buildPanel.start(kind, m_project, m_engineDirectory, m_time);
}

void Editor::pollBuild() {
    if (!m_buildPanel.poll(m_project, m_time)) return;
    const BuildKind kind = m_buildPanel.kind();
    if (!m_buildPanel.succeeded()) {
        if (kind == BuildKind::GameCode) {
            m_gameCodeFailed = true;
            if (!m_gameLoaded) m_gameStatus = tr("Game code build failed - fix it and press Ctrl+R");
        }
    } else if (kind == BuildKind::Upgrade) {
        // The upgrade tool changed mealkit.project's engine/engineVersion: reread only the values the tool manages
        ProjectInfo upgraded;
        if (loadProject(m_project.directory, upgraded)) {
            m_project.settings.engine = upgraded.settings.engine;
            m_project.settings.engineVersion = upgraded.settings.engineVersion;
            m_projectPanel.setToolValues(upgraded.settings);
        }
        MEALKIT_INFO("Project upgraded to engine %s", kEngineVersion);
        startBuild(BuildKind::GameCode); // templates (CMakeLists etc.) changed, so rebuild
    } else if (kind == BuildKind::GameCode) {
        m_gameCodeBuilt = true;
    }
}

void Editor::drawBuildLog() {
    BuildKind request;
    if (m_buildPanel.drawLog(m_mode == Mode::Edit, m_project, m_time, &request)) startBuild(request);
}

void Editor::saveAll() {
    saveScene();
    m_tuningPanel.saveDirty();
    if (m_projectPanel.dirty()) m_projectPanel.save(m_project);
    std::string status;
    if (m_tablesPanel.dirty()) m_tablesPanel.saveAll(&status);
}

void Editor::takePanelStatus() {
    for (std::string status : {m_tuningPanel.takeStatus(), m_projectPanel.takeStatus(), m_modulesPanel.takeStatus()})
        if (!status.empty()) setStatus(status);
    if (std::string status = m_buildPanel.takeStatus(); !status.empty())
        setStatus(status, !m_buildPanel.running() && !m_buildPanel.succeeded(), NoticeOpen::BuildLog);
}

// Notification at the bottom right of the viewport: success disappears after 4 seconds, failure stays until closed. Shows progress while building
void Editor::drawNotices() {
    std::erase_if(m_notices, [&](const Notice& n) { return !n.failed && m_time - n.time > 4.0; });
    const bool building = m_buildPanel.running();
    if (m_notices.empty() && !building) return;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove;
    ImGui::SetNextWindowPos(ImVec2(m_viewport.x + m_viewport.width - 12.0f, m_viewport.y + m_viewport.height - 12.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("##Notices", nullptr, flags)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        for (size_t i = 0; i < m_notices.size(); ++i) {
            Notice& n = m_notices[i];
            ImGui::PushID(static_cast<int>(i));
            if (n.failed) {
                if (ImGui::SmallButton("x")) n.time = -1.0;
                ImGui::SameLine();
            }
            ImGui::TextColored(n.failed ? ImVec4(1.0f, 0.50f, 0.45f, 1.0f) : ImVec4(0.60f, 0.88f, 0.60f, 1.0f), "%s",
                               n.text.c_str());
            if (n.open != NoticeOpen::None && ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (n.open != NoticeOpen::None && ImGui::IsItemClicked()) {
                const bool build = n.open == NoticeOpen::BuildLog;
                *(build ? m_buildPanel.openFlag() : m_logPanel.openFlag()) = true;
                focusPanel(build ? BuildPanel::kWindow : LogPanel::kWindow);
                n.time = -1.0;
            }
            ImGui::PopID();
        }
        if (building) {
            if (!m_notices.empty()) ImGui::Separator();
            m_buildPanel.drawStatus(m_time);
            // Indeterminate progress bar (negative = back-and-forth animation)
            ImGui::ProgressBar(-static_cast<f32>(m_time), ImVec2(ImGui::GetFontSize() * 14.0f, 0.0f), "");
        }
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
    std::erase_if(m_notices, [](const Notice& n) { return n.time < 0.0; });
}

void Editor::drawPlayFrame() {
    if (m_mode != Mode::Play) return;
    const ImVec2 a(m_viewport.x, m_viewport.y), b(m_viewport.x + m_viewport.width, m_viewport.y + m_viewport.height);
    ImGui::GetBackgroundDrawList()->AddRect(a, b, ImGui::GetColorU32(ImVec4(0.95f, 0.55f, 0.15f, 1.0f)), 0.0f, 0, 4.0f);
}

void Editor::drawEmptyViewport() {
    if (m_mode != Mode::Edit || m_gameLoaded || m_project.directory.empty()) return;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove;
    ImGui::SetNextWindowPos(ImVec2(m_viewport.x + m_viewport.width * 0.5f, m_viewport.y + m_viewport.height * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin("##NoGameCode", nullptr, flags)) {
        ImGui::TextUnformatted(tr("The game code is not built yet. Build it to play."));
        ImGui::BeginDisabled(m_buildPanel.running());
        if (ImGui::Button(tr("Build Game Code"))) startBuild(BuildKind::GameCode);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Build Log"))) {
            *m_buildPanel.openFlag() = true;
            focusPanel(BuildPanel::kWindow);
        }
    }
    ImGui::End();
}

void Editor::applyRenderStyle(const game::ProjectSettings& settings) {
    if (!m_renderer) return;
    game::applyRenderStyle(settings, *m_renderer, m_renderStyle);
    if (m_automated) {
        const render::ResolutionSettings& r = m_renderer->resolution();
        m_renderer->setDynamicResolution(false, r.minScale, r.budgetMs, r.sharpness);
    }
}

} // namespace mealkit::editor

namespace mealkit::editor {

bool Editor::previewCutscene(const std::string& name) {
    if (m_previewCutscenes.count() == 0) m_previewCutscenes.load();
    m_previewBrain.load(scene::Scene{});
    if (!m_previewBrain.playCutscene(m_previewCutscenes, name)) {
        setStatus(trf("Cutscene '%s' has no playable camera clip (see the log)", name.c_str()), true);
        return false;
    }
    m_lookThrough = 0;
    m_cutscenePreview = name;
    m_previewSeconds = 0.0f;
    return true;
}

void Editor::updateCameraPreview(f64 dt, f32 aspect) {
    if (m_mode != Mode::Edit || m_looking || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_lookThrough = 0;
        m_cutscenePreview.clear();
    }
    if (m_lookThrough && !m_scene.find(m_lookThrough)) m_lookThrough = 0;
    if (!previewing()) return;
    camera::BrainInput in;
    in.defaultFov = kFovY * (180.0f / 3.14159265f);
    if (!m_cutscenePreview.empty()) {
        in.dt = static_cast<f32>(std::min(dt, 0.1));
        std::vector<std::string> events;
        m_previewBrain.update(in, &events);
        m_previewSeconds += in.dt;
        if (m_feedback)
            for (const std::string& row : events) m_feedback->play(row, m_previewBrain.pose().position);
        if (!m_previewBrain.cutscenePlaying()) m_cutscenePreview.clear(); // the end: back to the editor camera
    } else {
        // Only this camera active, in a flattened copy (edits to its fields show at once)
        scene::Scene flat = scene::flattened(m_scene);
        for (scene::Entity& e : flat.entities) {
            if (!e.is(scene::kTypeVirtualCamera)) continue;
            std::erase_if(e.fields, [](const auto& f) { return f.first == "active"; });
            e.fields.emplace_back("active", e.id == m_lookThrough ? "true" : "false");
        }
        m_previewBrain.load(flat);
        m_previewBrain.update(in);
    }
    if (!previewing()) return;
    m_previewPose = m_previewBrain.pose();
    const Vec3 forward = m_previewPose.forward();
    m_view = lookAtLH(m_previewPose.position, m_previewPose.position + forward, rotate(m_previewPose.rotation, Vec3{0, 1, 0}));
    m_proj = perspectiveReversedZ(m_previewPose.fovDegrees * (3.14159265f / 180.0f), aspect, 0.1f, 2000.0f);
    drawCameraPreviewLabel();
}

void Editor::drawCameraPreviewLabel() {
    if (m_automated) return; // captures show only the camera's picture
    char text[200];
    if (!m_cutscenePreview.empty())
        std::snprintf(text, sizeof(text), tr("Cutscene %s  %.1f s  (Esc stops)"), m_cutscenePreview.c_str(),
                      static_cast<double>(m_previewSeconds));
    else {
        const scene::Entity* e = m_scene.find(m_lookThrough);
        std::snprintf(text, sizeof(text), tr("Looking through %s  (Esc stops)"), e ? e->name.c_str() : "?");
    }
    ImGui::GetForegroundDrawList()->AddText(ImVec2(m_viewport.x + 12.0f, m_viewport.y + 40.0f), IM_COL32(255, 220, 90, 255), text);
}

} // namespace mealkit::editor
