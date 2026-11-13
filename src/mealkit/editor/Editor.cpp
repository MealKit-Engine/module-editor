#include "mealkit/editor/Editor.h"
#include "mealkit/editor/SharedBuild.h"
#include "mealkit/editor/EditorColors.h"

#include "mealkit/core/Log.h"
#include "mealkit/core/Memory.h"
#include "mealkit/core/Profile.h"
#include "mealkit/core/Version.h"
#include "mealkit/editor/CodeLink.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/game/InputRecord.h"
#include "mealkit/game/Scenario.h"
#include "mealkit/platform/FileDialog.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Process.h"
#include "mealkit/platform/System.h"
#include "mealkit/platform/Window.h"
#include "mealkit/render/Environment.h"
#include "mealkit/render/ModelDraw.h"
#include "mealkit/render/SceneLights.h"
#include "mealkit/rhi/GpuCapture.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
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
constexpr const char* kSceneWindow = "Viewport###SceneView";
constexpr const char* kUiWindow = "UI###UiWorkspace";
// Windows docked as tabs in the bottom row (default layout)
const ImVec4 kPlayColor(0.42f, 0.24f, 0.08f, 1.0f); // menu bar during play
const char* const kBottomWindows[] = {"Assets###Assets", "Debug###Debug"};

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

} // namespace

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

bool Editor::init(Window& window, render::Renderer& renderer, const std::string& scenePath, asset::ModelCache& models) {
    buildCommands();
    m_prefs = loadPreferences();
    m_cameraSpeed = m_prefs.cameraSpeed;
    colorPickerSize() = m_prefs.colorPickerSize;
    m_window = &window;
    m_renderer = &renderer;
    m_sprites = std::make_unique<render::SpriteTextures>(renderer);
    m_models = &models;
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

void Editor::shutdown() {
    if (!m_automated) {
        if (m_window) {
            m_prefs.window = m_window->placement();
            savePreferences(m_prefs);
        }
        savePanelState();
    }
    m_imgui.shutdown();
}

// Which panels are open, next to the project's layout.ini. ImGui saves where windows are docked but not these flags: a closed flag removes the tab
void Editor::loadPanelState() {
    std::string text;
    if (m_automated || m_panelStatePath.empty() || !readTextFile(m_panelStatePath, text)) return;
    const std::pair<const char*, bool*> panels[] = {{"scene", &m_sceneOpen}, {"assets", &m_assetsOpen},
                                                    {"debug", &m_debugOpen},         {"data", &m_dataOpen},   {"previews", &m_previewOpen},
                                                    {"modules", &m_modulesOpen}, {"ui", &m_uiWorkspaceOpen}};
    size_t start = 0;
    for (size_t end = text.find('\n'); end != std::string::npos; start = end + 1, end = text.find('\n', start)) {
        const std::string line = text.substr(start, end - start);
        const size_t space = line.find(' ');
        if (space == std::string::npos) continue;
        const std::string key = line.substr(0, space);
        const int value = std::atoi(line.c_str() + space + 1);
        for (const auto& [name, flag] : panels)
            if (key == name) *flag = value != 0;
        if (key == "previewPage" && value >= 0 && value <= static_cast<int>(PreviewPage::Cutscene)) m_previewPage = static_cast<PreviewPage>(value);
    }
}

void Editor::savePanelState() const {
    if (m_panelStatePath.empty()) return;
    std::string text;
    const auto add = [&text](const char* name, int value) { text += std::string(name) + " " + std::to_string(value) + "\n"; };
    add("scene", m_sceneOpen);
    add("assets", m_assetsOpen);
    add("debug", m_debugOpen);
    add("data", m_dataOpen);
    add("previews", m_previewOpen);
    add("modules", m_modulesOpen);
    add("ui", m_uiWorkspaceOpen);
    add("previewPage", static_cast<int>(m_previewPage));
    writeTextFile(m_panelStatePath, text);
}

void Editor::update(f64 dt) {
    m_animTime += dt;
    MEALKIT_ZONE("Editor UI");
    mem::endFrame(); // the previous frame is drawn, so its frame arena allocations are done
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
    updateHotReload(dt);

    if (!m_layoutLoad.empty()) { // only between frames
        ImGui::LoadIniSettingsFromDisk(m_layoutLoad.c_str());
        m_layoutLoad.clear();
    }
    m_imgui.newFrame([this] { runTestInput(); });
    ImGuizmo::BeginFrame();

    ImGui::GetStyle().FontScaleMain = m_prefs.uiScale;
    if (m_prefs.compact != m_compactApplied) { // spacing from the normal style, so turning it off restores it exactly
        ImGuiStyle& style = ImGui::GetStyle();
        const f32 scale = m_prefs.compact ? 0.5f : 2.0f;
        style.FramePadding.y *= scale;
        style.ItemSpacing.y *= scale;
        style.CellPadding.y *= scale;
        m_compactApplied = m_prefs.compact;
    }
    if (colorPickerSize() != m_prefs.colorPickerSize && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) { // after the drag ends
        m_prefs.colorPickerSize = colorPickerSize();
        savePreferences(m_prefs);
    }
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
    {
        // Own host window, not DockSpaceOverViewport: with the passthrough flag ImGui fills an occupied center node, which would cover the 3D picture
        const ImGuiViewport* main = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(main->WorkPos);
        ImGui::SetNextWindowSize(main->WorkSize);
        ImGui::SetNextWindowViewport(main->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##DockHost", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                                ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar(2);
        // The gaps between docked panels are painted in WindowBg: make them the dark gutter so panels read as separate without lines
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
        ImGui::DockSpace(dockId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
        ImGui::PopStyleColor();
        ImGui::End();
    }
    drawSceneWindow(dockId);
    drawUiWorkspace();

    if (m_sceneOpen) updateCamera(dt);
    const f32 aspect = m_viewport.width / std::max(m_viewport.height, 1.0f);
    m_proj = projection(aspect);
    m_view = lookAtLH(m_cameraPos, m_cameraPos + cameraForward(), {0, 1, 0});
    updateCameraPreview(dt, aspect); // looking through a virtual camera / cutscene: replaces m_view and m_proj

    const bool editing = m_mode == Mode::Edit;
    drawDataHub(dt, editing);
    drawDebugHub();
    drawPreviewHub();
    // Editing the sound event table (view-only) saves it so changes are heard immediately (audio rereads the file)
    m_viewTableDirty |= m_tablesPanel.takeViewTableChanged();
    m_viewTableTimer += dt;
    if (m_viewTableDirty && m_viewTableTimer >= 0.3) {
        std::string status;
        m_tablesPanel.saveAll(&status);
        m_viewTableDirty = false;
        m_viewTableTimer = 0.0;
    }
    updateProblems(dt);
    m_testsPanel.poll(m_project, dt);
    m_balancePanel.poll(m_project);
    updateStateGraphs();
    m_modulesPanel.update();
    drawSettingsHub();
    m_modulesPanel.draw(m_project, m_engineDirectory, m_mode == Mode::Edit, &m_modulesOpen);
    // Module sim/ and view/ are game code: adding or changing them rebuilds the game DLL
    if (m_modulesPanel.takeChanged()) {
        readProjectModules(); // menus and pages follow the new selection
        startBuild(BuildKind::GameCode);
    }
    closePanelsOfMissingModules();
    pollBuild();
    pollLayoutCheck();
    drawBuildLog();
    takePanelStatus();
    drawNotices();
    drawSceneList();
    drawAssetBrowser();
    drawPlayFrame();
    drawEmptyViewport();
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false) && rhi::gpuCaptureTool()) captureGpuFrame(); // both edit and play

    runShortcuts();
    drawCommandPalette();
    if (editing && !m_sceneVisible) {
        // Another tab covers the scene view: no gizmo, toolbar or picking
    } else if (editing) {
        drawViewportToolbar(); // read-only preview: camera, view modes; the scene is edited in MkRender
    } else if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) { // F5 (Shift+F5 too) stops; Esc belongs to the game (pause menus)
        m_stopRequest = true;
    } else if (m_simDebug && m_canPause && m_simDebug->timeline().recording) {
        if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) m_simDebug->setPaused(!m_simDebug->timeline().paused);
        if (ImGui::IsKeyPressed(ImGuiKey_F7)) m_simDebug->stepOnce();
    }
    reportSimProblems();


    drawPopups();
    drawEngineRestartPrompt();
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
        if (iconButton(t.paused ? icon::kPlay : icon::kPause, t.paused ? tr("Resume (F6)") : tr("Pause (F6)"))) m_simDebug->setPaused(!t.paused);
        ImGui::BeginDisabled(!t.paused);
        if (iconButton(icon::kStep, tr("Step (F7)"))) m_simDebug->stepOnce();
        int view = static_cast<int>(t.view);
        // The timeline gives way so [Save Scenario] and the shadow status stay on the bar in a narrow window
        const ImGuiStyle& style = ImGui::GetStyle();
        const f32 font = ImGui::GetFontSize();
        const f32 after = ImGui::GetFrameHeight() + style.ItemSpacing.x * 2.0f +
                          (m_simDebug->shadow().enabled ? font * 10.0f : 0.0f);
        ImGui::SetNextItemWidth(std::clamp(ImGui::GetWindowWidth() - ImGui::GetCursorPosX() - after, font * 4.0f, font * 14.0f));
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
    if (iconButton(icon::kSave, tr("Save Scenario"))) saveScenario(t.live - 1);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tr("Saves the inputs so far and the current state values as a scenario test in tests/ "
                                   "(solo play with replays kept, not while viewing the past).\n"
                                   "Keep the checks that state the rule and delete the rest."));
    const game::ShadowStatus& shadow = m_simDebug->shadow();
    if (shadow.enabled) {
        if (shadow.desynced)
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), tr("Shadow: diverged at tick %u"), shadow.tick);
        else ImGui::TextColored(color::kGood, "%s", tr("Shadow: in sync"));
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

// The scene view is a dock tab with a clear background: the 3D picture stays on the back buffer (offscreen targets have no shadows) and fits this window's area.
// Docked windows are opaque, so it only works docked: let go while floating, it returns to the center.
void Editor::drawSceneWindow(unsigned int dockId) {
    ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockId);
    if (!m_sceneOpen) {
        // Closed: its dock space stays, and the 3D picture still on the back buffer is covered
        m_sceneVisible = m_sceneHovered = false;
        const ImVec2 min = central ? central->Pos : ImVec2(m_viewport.x, m_viewport.y);
        const ImVec2 max = central ? ImVec2(central->Pos.x + central->Size.x, central->Pos.y + central->Size.y) : ImVec2(m_viewport.x + m_viewport.width, m_viewport.y + m_viewport.height);
        ImGui::GetBackgroundDrawList()->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_MenuBarBg));
        return;
    }
    if (central) {
        const ImGuiWindow* existing = ImGui::FindWindowByName(kSceneWindow);
        if (!existing || (!existing->DockIsActive && !ImGui::IsMouseDown(ImGuiMouseButton_Left))) ImGui::SetNextWindowDockID(central->ID, ImGuiCond_Always);
    }
    if (m_sceneFrontFrames > 0) {
        --m_sceneFrontFrames;
        ImGui::SetNextWindowFocus();
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    m_sceneVisible = ImGui::Begin(iconTitle(icon::kCube, kSceneWindow).c_str(), m_mode == Mode::Edit ? &m_sceneOpen : nullptr, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNavInputs); // with nav inputs, focus here would make ImGui claim the keyboard from the game
    ImGui::PopStyleVar();
    m_sceneHovered = m_sceneVisible && ImGui::IsWindowHovered();
    if (m_sceneVisible) { // behind another tab it keeps its last size, so a playing game doesn't resize
        ImVec2 pos = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
        // The tool bar is one row along the top of the tab and the 3D picture starts below it. The row stays during play
        // (holding the play status) so starting and stopping don't resize the picture
        m_toolbarHeight = 0.0f;
        if (size.y > ImGui::GetFrameHeight() * 6.0f) {
            m_toolbarHeight = std::floor(ImGui::GetFrameHeight() + 8.0f);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + m_toolbarHeight), ImGui::GetColorU32(ImGuiCol_WindowBg));
            if (m_mode == Mode::Play)
                draw->AddText(ImVec2(pos.x + 10.0f, pos.y + (m_toolbarHeight - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                              m_playStatus.empty() ? tr("Playing") : m_playStatus.c_str());
            pos.y += m_toolbarHeight;
            size.y -= m_toolbarHeight;
        }
        m_viewport = {pos.x, pos.y, std::max(size.x, 1.0f), std::max(size.y, 1.0f)};
        ImGuizmo::SetAlternativeWindow(ImGui::FindWindowByName(kSceneWindow)); // GetCurrentWindow reads DLL data
    }
    ImGui::End();
}

// [UI] tab beside the viewport: the project's screens with "Open in MkUI" (the Designer edits them), and while playing the game's open
// screens with their values and binding errors (UiPanel)
void Editor::drawUiWorkspace() {
    m_uiPanelOpen = false;
    if (!m_uiWorkspaceOpen) return;
    if (const ImGuiWindow* scene = ImGui::FindWindowByName(kSceneWindow); scene && scene->DockId)
        ImGui::SetNextWindowDockID(scene->DockId, ImGuiCond_FirstUseEver); // layouts saved before the tab existed
    if (m_uiFrontFrames > 0) { // asked to come to the front: keep asking until the tab shows (the dock node may not exist yet)
        --m_uiFrontFrames;
        m_sceneFrontFrames = 0;
        ImGui::SetNextWindowFocus();
    }
    const bool visible = ImGui::Begin(iconTitle(icon::kPreview, kUiWindow).c_str(), &m_uiWorkspaceOpen, ImGuiWindowFlags_NoCollapse);
    if (const ImGuiWindow* w = ImGui::FindWindowByName(kUiWindow); visible && m_uiFrontFrames > 0 && w && w->DockTabIsVisible) m_uiFrontFrames = 0; // GetCurrentWindow reads DLL data
    if (visible) {
        m_uiPanelOpen = true;
        m_uiPanel.draw(m_mode == Mode::Edit ? nullptr : m_gameUi);
    }
    ImGui::End();
}

// The MkUI Designer from the [Tools] list (the tool whose manifest opens .ui), with the project and optionally one screen
bool Editor::openInDesigner(const std::string& uiFile) {
    for (const ToolInfo& t : m_tools)
        if (t.problem == ToolInfo::Problem::None && std::find(t.opens.begin(), t.opens.end(), ".ui") != t.opens.end())
            return launchTool(t, m_project.directory, uiFile);
    return false;
}

static const ToolInfo* findSceneTool(const std::vector<ToolInfo>& tools) {
    for (const ToolInfo& t : tools)
        if (t.problem == ToolInfo::Problem::None && std::find(t.opens.begin(), t.opens.end(), ".scene") != t.opens.end()) return &t;
    return nullptr;
}

bool Editor::hasSceneTool() const { return findSceneTool(m_tools) != nullptr; }

bool Editor::openInSceneTool(const std::string& sceneFile) {
    const ToolInfo* t = findSceneTool(m_tools);
    return t && launchTool(*t, m_project.directory, sceneFile);
}

// The mouse is on a window other than the scene view
bool Editor::sceneMouseBlocked() const { return ImGui::GetIO().WantCaptureMouse && !m_sceneHovered; }

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
    const u32 selected = m_selected;
    if (loadScene(m_scenePath)) {
        if (m_scene.find(selected)) m_selected = selected; // keep the selection if the same id exists
        MEALKIT_INFO("Hot reload: scene %s", m_scenePath.c_str());
        setStatus(trf("Reloaded scene: %s", fileName(m_scenePath).c_str()));
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
    frame.post = render::postParams(*m_models, env.post);
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
    render::probes(*m_models, m_scene, m_probeList);
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
        if (e.is(scene::kTypeScatter)) { // instances only (the layer object itself has no shape)
            render::addScatter(out, *m_models, e, eye, !gameView && isSelected(e.id) ? render::kInstanceHighlight : 0u);
            continue;
        }
        if (e.is(scene::kTypeSprite)) { // image quad (render/Sprites.h)
            if (m_sprites) render::addSprite(out, *m_sprites, e, world);
            continue;
        }
        if (e.is(scene::kTypeTerrain)) { // axis-aligned, rotation ignored (like the collision)
            if (asset::Terrain* terrain = terrainOf(e))
                render::addTerrain(out, *m_models, *terrain, world.position, world.scale, e.color,
                                     !gameView && isSelected(e.id) ? render::kInstanceHighlight : 0u, eye);
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
        if (model) render::addModel(out, *model, mesh, inst, scene::animField(e), m_animTime); // skinned models use the clip from the anim field
        else out.add(mesh, inst);
    }
}

void Editor::renderLookdev(render::Renderer& renderer, f64 dt) {
    if (m_models) m_thumbnails.render(renderer, *m_models);
    if (!m_lookdevOpen || !m_models) return;
    // Follows the model picked in [Assets] (picking another kind of file keeps the previous model)
    const std::string ext = m_assetSelected.size() > 4 ? m_assetSelected.substr(m_assetSelected.find_last_of('.')) : std::string();
    if (ext == ".glb" || ext == ".gltf") m_lookdev.setModel(m_assetSelected);
    m_lookdev.render(renderer, *m_models, static_cast<render::LightingModel>(m_project.settings.lightingModel), dt);
}

void Editor::captureGpuFrame() {
    if (rhi::captureNextGpuFrame()) setStatus(trf("GPU capture: next frame -> %s", rhi::gpuCaptureFolder().c_str()));
}

void Editor::setMode(Mode mode) {
    m_gameUi = nullptr; // set again by the play session's first update
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
    if (mode == Mode::Play) {
        m_sceneOpen = true; // the game must be seen
        openDebugPage(DebugPage::State); // the [State] page during play
        m_sceneFrontFrames = m_debugFrontFrames + 2; // outlasts the [Debug] request so the game view ends up with the keyboard
    }
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

bool Editor::gameMouseAllowed() const { return m_mode == Mode::Play && !sceneMouseBlocked(); }

// ---------------------------------------------------------------- UI

// Predefined layouts: the scene view, one column on the right ([Previews]), bottom row (build log, tables).
// Play shrinks the bottom row and brings [State] to front; lookdev puts a large [Lookdev] window at the top right.
void Editor::setupDefaultLayout(u32 dockId, LayoutPreset preset) {
    m_sceneOpen = true;
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, viewport->WorkSize);
    ImGuiID center = dockId;
    m_previewOpen = true;
    const f32 rightShare = preset == LayoutPreset::LookDev ? 0.42f : 0.27f;
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, rightShare, nullptr, &center);
    const f32 bottomShare = preset == LayoutPreset::Play ? 0.18f : 0.26f;
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, bottomShare, nullptr, &center);
    ImGui::DockBuilderDockWindow(kUiWindow, center); // a tab behind the viewport
    ImGui::DockBuilderDockWindow(kSceneWindow, center);
    m_sceneFrontFrames = 2;
    if (preset == LayoutPreset::LookDev) {
        const ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, nullptr, &right);
        ImGui::DockBuilderDockWindow("Previews###Previews", right);
        right = rightBottom;
        openPreviewPage(PreviewPage::LookDev);
    } else {
        ImGui::DockBuilderDockWindow("Previews###Previews", right);
    }
    for (const char* w : kBottomWindows) ImGui::DockBuilderDockWindow(w, bottom);
    ImGui::DockBuilderDockWindow("Data###Data", bottom);
    ImGui::DockBuilderFinish(dockId);
    if (preset == LayoutPreset::Play) openDebugPage(DebugPage::State);
}

void Editor::drawMenuBar() {
    const bool editing = m_mode == Mode::Edit;
    if (!editing) ImGui::PushStyleColor(ImGuiCol_MenuBarBg, kPlayColor); // so it's clear you're in play
    const bool open = ImGui::BeginMainMenuBar();
    if (!editing) ImGui::PopStyleColor();
    if (!open) return;

    // Enabled during play too: each item decides for itself
    if (ImGui::BeginMenu(tr("Project"))) {
        commandMenuItem("Settings..."); // project settings, preferences, tools and shortcuts are its pages
        ImGui::Separator();
        // Back to the project list: MealKit Manager. The editor stays open
        if (ImGui::MenuItem(tr("Project Manager"))) {
            const std::string manager = editorUserDirectory() + "\\manager\\MealKitManager.exe";
            if (!fileExists(manager)) setStatus(tr("MealKit Manager is not installed"), true);
            else if (!launchDetached(manager, "--engine \"" + m_engineDirectory + "\"", m_engineDirectory))
                setStatus(tr("Could not start MealKit Manager"), true);
        }
        commandMenuItem("Open Project Folder");
        commandMenuItem("Open Release Folder");
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Exit"))) m_quit = true;
        ImGui::EndMenu();
    }
    if (projectHas("scene") && ImGui::BeginMenu(tr("Scene"), editing)) {
        commandMenuItem("Open...");
        if (ImGui::BeginMenu(tr("Recent Scenes"), !m_recentScenes.empty())) {
            std::string chosen;
            for (const std::string& path : m_recentScenes)
                if (ImGui::MenuItem(fileName(path).c_str(), nullptr, path == m_scenePath)) chosen = path;
            ImGui::EndMenu();
            if (!chosen.empty() && chosen != m_scenePath) openSceneFile(chosen);
        }
        commandMenuItem("Scenes", &m_sceneListOpen);
        ImGui::Separator();
        if (hasSceneTool() && ImGui::MenuItem(tr("Open in MkRender"), nullptr, false, !m_scenePath.empty())) {
            const std::string assets = m_project.directory + "\\assets\\";
            openInSceneTool(m_scenePath.rfind(assets, 0) == 0 ? m_scenePath.substr(assets.size()) : m_scenePath);
        }
        ImGui::Separator();
        commandMenuItem("Open Replay...");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Edit"), editing)) {
        commandMenuItem("Undo");
        commandMenuItem("Redo");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Window"))) {
        commandMenuItem("Viewport", &m_sceneOpen);
        if (projectHas("ui")) commandMenuItem("UI", &m_uiWorkspaceOpen);
        ImGui::Separator();
        commandMenuItem("Assets", &m_assetsOpen);
        commandMenuItem("Debug", &m_debugOpen); // log with problems, build log, performance and state are its pages
        commandMenuItem("Data", &m_dataOpen); // tuning, tables, feedback and sounds are its pages (their commands stay in the palette)
        commandMenuItem("Previews", &m_previewOpen); // UI, look dev and cutscenes are its pages
        commandMenuItem("Modules", &m_modulesOpen); // engine and source modules of the project, and the registry
        ImGui::Separator();
        if (ImGui::BeginMenu(tr("Layout"))) {
            const char* names[] = {tr("Edit"), tr("Play"), tr("Look Dev")};
            for (int i = 0; i < 3; ++i)
                if (ImGui::MenuItem(names[i], nullptr, static_cast<int>(m_layoutPreset) == i)) {
                    m_layoutPreset = static_cast<LayoutPreset>(i);
                    m_layoutReset = true;
                }
            ImGui::Separator();
            commandMenuItem("Reset Layout");
            // Layouts the user saved (per user, in the editor's folder)
            const std::string saved = editorUserDirectory() + "\\layouts";
            const std::vector<std::string> layouts = listFiles(saved, ".ini");
            if (!layouts.empty()) ImGui::Separator();
            for (const std::string& f : layouts)
                if (ImGui::MenuItem(f.substr(0, f.size() - 4).c_str())) m_layoutLoad = saved + "\\" + f;
            ImGui::Separator();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
            ImGui::InputTextWithHint("##layoutName", tr("Layout name"), m_layoutName, sizeof(m_layoutName));
            ImGui::SameLine();
            ImGui::BeginDisabled(!data::validRowId(m_layoutName));
            if (ImGui::SmallButton(tr("Save"))) {
                createDirectories(saved);
                ImGui::SaveIniSettingsToDisk((saved + "\\" + m_layoutName + ".ini").c_str());
                m_layoutName[0] = 0;
            }
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }
        ImGui::Separator();
        commandMenuItem("Command Palette");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Build"), !m_project.directory.empty())) {
        commandMenuItem("Build Game Code (Hot Reload)");
        commandMenuItem("Release Build");
        ImGui::EndMenu();
    }
    // Tool suite (editor/Tools.h): opens MealKit tools from separate repos with this project
    if ((!m_tools.empty() || !m_installableTools.empty()) && !m_project.directory.empty() && ImGui::BeginMenu(tr("Tools"))) { // installing and rescanning are in [Settings]
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
        // Not installed yet: MealKit Manager installs tools (its Tools page)
        if (!m_installableTools.empty()) {
            if (!m_tools.empty()) ImGui::Separator();
            for (const mkcontract::AvailableTool& a : m_installableTools) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                const bool clicked = ImGui::MenuItem(trf("%s (install in MealKit Manager...)", a.name.c_str()).c_str());
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered() && !a.summary.empty()) ImGui::SetTooltip("%s", a.summary.c_str());
                if (clicked && !openManagerTools()) setStatus(tr("Could not start MealKit Manager"), true);
            }
        }
        ImGui::EndMenu();
    }


    // No game module: nothing to play, so no play button, scene switcher or player options
    if (!projectHas("game")) {
        ImGui::EndMainMenuBar();
        return;
    }
    const f32 width = ImGui::GetFrameHeight() * 2.2f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 8.0f, (ImGui::GetWindowWidth() - width) * 0.5f));
    // docs/TOOL_DESIGN.md 3.2: no colored fills. Editing: a neutral pill with a positive-green play glyph;
    // playing is the active state, so the accent fills the button behind the stop glyph
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 fill = editing ? style.Colors[ImGuiCol_Button] : ImVec4(0x3B / 255.0f, 0x63 / 255.0f, 0xFB / 255.0f, 1.0f);
    const ImVec4 hover = editing ? style.Colors[ImGuiCol_ButtonHovered] : ImVec4(0x4B / 255.0f, 0x75 / 255.0f, 1.0f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, fill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight() * 0.5f); // pill
    ImGui::BeginDisabled(editing && !m_gameLoaded); // can't play without the game DLL
    if (ImGui::Button("##playStop", ImVec2(width, 0.0f))) {
        if (editing) m_playRequest = true;
        else m_stopRequest = true;
    }
    drawGlyph(ImGui::GetWindowDrawList(), editing ? icon::kPlay : icon::kStop, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetFontSize() * 0.82f,
              editing ? ImGui::GetColorU32(ImVec4(0x2D / 255.0f, 0xB5 / 255.0f, 0x74 / 255.0f, 1.0f)) : ImGui::GetColorU32(ImGuiCol_Text));
    ImGui::EndDisabled();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", editing && !m_gameLoaded ? m_gameStatus.c_str() : editing ? tr("Play (F5)") : tr("Stop (F5)"));

    // The bar's controls sit directly on it until hovered. Switching scenes is in the Scene menu and the status bar's scene name
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

    // Play options (only before play, so the play controls have the room): player count, with several more join windows open
    if (editing) {
        const char* counts[] = {tr("Solo"), tr("2 players"), tr("3 players"), tr("4 players")};
        const int players = static_cast<int>(m_playPlayers) - 1;
        const std::string countLabel = std::string(icon::kPeople) + "  " + counts[players];
        ImGui::SetNextItemWidth(ImGui::CalcTextSize(counts[1]).x + ImGui::GetFrameHeight() * 2.6f);
        if (beginCombo("##playOptions", countLabel.c_str(), ImGuiComboFlags_HeightLargest)) {
            for (int i = 0; i < 4; ++i)
                if (ImGui::Selectable(counts[i], i == players)) m_playPlayers = static_cast<u32>(i + 1);
            ImGui::Separator();
            ImGui::BeginDisabled(m_playPlayers == 1);
            ImGui::Checkbox(tr("Bots in player windows"), &m_playClientBots);
            ImGui::EndDisabled();
            ImGui::BeginDisabled(m_playPlayers != 1);
            ImGui::Checkbox(tr("Start at the first screen"), &m_playMenus);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", tr("Solo play of a game with menus starts at its title and menus, like the game's exe"));
            commandMenuItem("Play From Here");
            ImGui::Separator();
            ImGui::Checkbox(tr("Shadow check"), &m_shadowCheck);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("%s", tr("Runs a second simulation with the same inputs during play and compares every tick.\n"
                                        "On a mismatch it stops and shows the first diverging tick and state fields (2x simulation CPU)"));
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s", tr("With several players the editor hosts and opens more player windows (current scene and tuning, same PC)"));
        ImGui::PopStyleColor();
    } else {
        ImGui::PopStyleColor();
        drawPlayControls();
    }
    ImGui::EndMainMenuBar();
}

void Editor::drawStatusBar() {
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, ImGui::GetFrameHeight(),
                                    flags)) {
        if (ImGui::BeginMenuBar()) {
            // Space between groups instead of divider characters
            const auto gap = [] { ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 0.75f, 0.0f)); };
            // The open scene; clicking it while editing lists the project's scenes to switch to
            {
                const std::string name = m_scenePath.empty() ? tr("Untitled") : fileName(m_scenePath);
                ImGui::TextDisabled("%s", name.c_str());
                if (m_mode == Mode::Edit && !m_project.directory.empty()) {
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        ImGui::SetTooltip("%s", tr("Open another scene"));
                    }
                    if (ImGui::IsItemClicked()) ImGui::OpenPopup("##sceneSwitch");
                    if (ImGui::BeginPopup("##sceneSwitch")) {
                        const std::string folder = m_project.directory + "\\assets\\scenes";
                        for (const std::string& f : listFiles(folder, ".scene"))
                            if (ImGui::Selectable(f.c_str(), f == name) && f != name) openSceneFile(folder + "\\" + f);
                        ImGui::EndPopup();
                    }
                }
                gap();
            }
            ImGui::TextDisabled(tr("%zu objects"), m_scene.entities.size());
            gap();
            if (m_buildPanel.drawStatus(m_time)) {
                gap();
            } else if (!m_gameStatus.empty()) {
                if (m_gameLoaded) ImGui::TextDisabled("%s", m_gameStatus.c_str());
                else ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "%s", m_gameStatus.c_str());
                gap();
            }
            drawUnsavedStatus();
            if (m_mode == Mode::Play) ImGui::TextUnformatted(m_playStatus.c_str());
            else if (m_sceneHovered) // only when it applies; messages show as notices over the viewport (drawNotices)
                ImGui::TextDisabled(
                    "%s", tr("Camera: right drag + WASD/QE, wheel for speed | Select: click | Hold Ctrl on gizmo: snap"));
            const char* fps = "0000 fps";
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::CalcTextSize(fps).x - 16.0f);
            // Red when over budget; clicking opens the [Performance] window
            const ImVec4 fpsColor = m_perfPanel.overBudget() ? color::kDanger
                                                             : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            ImGui::TextColored(fpsColor, "%.0f fps", m_perf.sample().fps);
            if (ImGui::IsItemClicked()) openDebugPage(DebugPage::Performance);
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

void Editor::openSceneFile(const std::string& path) {
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
    if (iconButton(icon::kLaunch, tr("Open"))) open = picked;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(picked.empty() || m_project.directory.empty());
    if (iconButton(icon::kStarFilled, tr("Set as Start Scene"))) m_projectPanel.setStartScene(m_project, "scenes/" + fileName(picked));
    ImGui::EndDisabled();
    if (hasSceneTool()) {
        ImGui::SameLine();
        ImGui::BeginDisabled(picked.empty());
        if (iconButton(icon::kLaunch, tr("Open in MkRender"))) openInSceneTool("scenes/" + fileName(picked));
        ImGui::EndDisabled();
    }
    ImGui::TextDisabled("%s", tr("★ marks the scene the game starts with (mealkit.project)"));
    ImGui::EndDisabled();
    ImGui::End();
    if (!open.empty()) openSceneFile(open);
}

// ---------------------------------------------------------------- Definition data tables ([Tables] panel)

void Editor::saveTablesForPlay() {
    std::string status;
    if (!m_tablesPanel.dirty()) return;
    if (m_tablesPanel.saveAll(&status)) setStatus(trf("Tables saved for play: %s", status.c_str()));
    else setStatus(status);
}

// Simulation state during play (shows the state field declarations as is)
void Editor::drawState() {
    if (m_mode != Mode::Play || !m_stateView.type || !m_stateView.data) {
        ImGui::TextDisabled("%s", tr("The simulation state (values declared with MEALKIT_FIELD) appears during play."));
        return;
    }
    ImGui::TextDisabled(tr("%s (read-only)"), m_stateView.type->name);
    drawStateGraphs();
    drawFieldsReadOnly(*m_stateView.type, m_stateView.data);
}

// Toolbar overlaid on the viewport (top left) and axis gizmo (top right; clicking an axis views from that side)
void Editor::drawViewportToolbar() {
    if (m_viewport.width < 200.0f || m_viewport.height < 120.0f || m_toolbarHeight <= 0.0f) return;
    // One row docked along the top of the scene tab (drawSceneWindow reserves it), the 3D picture below. A narrow tab clips the end of the row
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    const f32 cubeSize = std::floor(ImGui::GetFrameHeight() * 3.0f); // axis gizmo in the picture's top right corner
    ImGui::SetNextWindowPos(ImVec2(m_viewport.x, m_viewport.y - m_toolbarHeight));
    ImGui::SetNextWindowSize(ImVec2(m_viewport.width, m_toolbarHeight));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##ViewportToolbar", nullptr, flags)) {
        auto tip = [](const char* text) {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text);
        };
        const f32 gap = ImGui::GetFontSize();
        const auto beginGroup = [&](int g) {
            if (g > 0) {
                ImGui::SameLine(0.0f, gap * 0.5f);
                // Thin divider between groups
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const f32 h = ImGui::GetFrameHeight();
                ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + h * 0.2f), ImVec2(p.x, p.y + h * 0.8f), ImGui::GetColorU32(ImGuiCol_Separator));
                ImGui::Dummy(ImVec2(1.0f, h));
                ImGui::SameLine(0.0f, gap * 0.5f);
            }
            ImGui::BeginGroup();
        };
        const auto endGroup = [](int) { ImGui::EndGroup(); };

        beginGroup(1);
        if (iconButton(icon::kArea, tr("Grid"), m_showGrid)) m_showGrid = !m_showGrid;
        endGroup(1);

        beginGroup(2);
        if (iconButton(m_ortho ? icon::kOrtho : icon::kPerspective, tr("Switch between perspective and orthographic (wheel zooms in orthographic)"))) m_ortho = !m_ortho;
        ImGui::SameLine(0, 2);
        const char* views[] = {tr("Lit"), tr("Wireframe"), tr("Normals"), tr("LOD Level"), tr("Overdraw"),
                               tr("Over Budget"), tr("Material Features")};
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        if (beginCombo("##viewMode", views[static_cast<int>(m_debugView)])) {
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
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.5f);
        ImGui::DragFloat("##camSpeed", &m_cameraSpeed, 0.01f, 1.0f, 400.0f, "%.0f m/s", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        tip(tr("Camera speed"));
        endGroup(2);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);

    // Axis gizmo: when ImGuizmo rotates the view matrix, convert back to camera angles/position
    const f32 size = cubeSize;
    Mat4 view = m_view;
    const f32 pivot = 10.0f; // orbits around the point this far ahead
    ImGuizmo::ViewManipulate(&view.m[0][0], pivot, ImVec2(m_viewport.x + m_viewport.width - size - 8.0f, m_viewport.y + 8.0f),
                             ImVec2(size, size), IM_COL32(0, 0, 0, 0));
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

    // 2D projects (mealkit.project view2d): always orthographic, looking along +z at the XY plane; no turning
    const bool flat = m_project.settings.view2d;
    if (flat) {
        m_ortho = true;
        m_cameraYaw = 0.0f;
        m_cameraPitch = 0.0f;
        m_orbiting = false;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && inViewport && !sceneMouseBlocked()) m_looking = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) m_looking = false;
    m_window->setCursorCaptured(m_looking);

    const Vec3 forward = cameraForward();
    const Vec3 right = normalize(cross({0, 1, 0}, forward));
    const Vec3 up = cross(forward, right);
    Input& input = m_window->input();

    if (m_looking) {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        if (!flat) {
            m_cameraYaw += input.mouseDeltaX() * 0.0035f;
            m_cameraPitch = std::clamp(m_cameraPitch - input.mouseDeltaY() * 0.0035f, -1.55f, 1.55f);
        }
        if (io.MouseWheel != 0.0f)
            m_cameraSpeed = std::clamp(m_cameraSpeed * std::pow(1.25f, io.MouseWheel), 1.0f, 400.0f);
        Vec3 move{};
        const Vec3 ahead = flat ? Vec3{0, 1, 0} : forward; // 2D: W/S move up and down on the plane
        if (ImGui::IsKeyDown(ImGuiKey_W)) move += ahead;
        if (ImGui::IsKeyDown(ImGuiKey_S)) move -= ahead;
        if (ImGui::IsKeyDown(ImGuiKey_D)) move += right;
        if (ImGui::IsKeyDown(ImGuiKey_A)) move -= right;
        if (ImGui::IsKeyDown(ImGuiKey_E)) move += Vec3{0, 1, 0};
        if (ImGui::IsKeyDown(ImGuiKey_Q)) move -= Vec3{0, 1, 0};
        const f32 speed = m_cameraSpeed * (io.KeyShift ? 3.0f : 1.0f);
        m_cameraPos += normalize(move) * (speed * static_cast<f32>(dt));
    } else {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        // Alt + left drag: orbit around the selected object (or 10 m ahead if none)
        if (!flat && io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && inViewport && !sceneMouseBlocked()) {
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
        } else if (inViewport && !sceneMouseBlocked()) {
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
    // zNear -500 so objects behind the camera are visible too
    return orthographicReversedZ(m_orthoHalfHeight, aspect, -500.0f, 2000.0f);
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

// Terrain of a terrain object (its heightmap from the model cache; edited in MkRender)
asset::Terrain* Editor::terrainOf(const scene::Entity& e) const {
    return e.is(scene::kTypeTerrain) ? m_models->terrain(scene::terrainFile(e)) : nullptr;
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
        if (e.is(scene::kTypeScatter)) continue; // selected from the scene tree (instances aren't pickable)
        if (const asset::Terrain* terrain = terrainOf(e)) { // the surface, not its box (objects above stay clickable)
            const f32 t = asset::raycastTerrain(terrain->map, w.position, w.scale, origin, dir, bestT);
            if (t >= 0.0f && t < bestT) {
                bestT = t;
                best = e.id;
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

bool Editor::isSelected(u32 id) const {
    return id && (id == m_selected || std::find(m_multi.begin(), m_multi.end(), id) != m_multi.end());
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

void Editor::openSceneDialog() {
    const std::string path = openFileDialog(m_window->nativeHandle(), m_scenePath, "", kSceneFilters, 2);
    if (path.empty()) return;
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
    text += game::scenarioChecksText(*m_stateView.type, m_stateView.data, tick);
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
    m_selected = 0;
    return true; // the tree and the scene switcher show it; no notice
}

void Editor::setStatus(const std::string& text, bool failed, NoticeOpen open) {
    if (m_notices.size() >= 5) m_notices.erase(m_notices.begin());
    m_notices.push_back({text, m_time, failed, failed ? open : NoticeOpen::None});
}

void Editor::updateTitle() {
    std::string title = std::string("MealKit Editor ") + kEngineVersion + " - ";
    if (!m_project.name().empty()) title += m_project.name() + " - ";
    title += m_scenePath.empty() ? std::string(tr("Untitled")) : fileName(m_scenePath);
    if (m_tuningPanel.dirty()) title += std::string("  (") + tr("Tuning") + " *)";
    if (m_projectPanel.dirty()) title += std::string("  (") + tr("Project Settings") + " *)";
    if (m_mode == Mode::Play) title += std::string("  [") + tr("Playing") + "]";
    if (title != m_title) {
        m_window->setTitle(title.c_str());
        m_title = title;
    }
}

// ---------------------------------------------------------------- Project modules

void Editor::readProjectModules() {
    m_projectModules.clear();
    m_projectAllModules = true;
    if (m_project.directory.empty()) return;
    std::string dir = m_project.directory;
    std::replace(dir.begin(), dir.end(), '/', '\\');
    std::string samples = m_engineDirectory + "\\samples\\";
    std::replace(samples.begin(), samples.end(), '/', '\\');
    const auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    if (!m_engineDirectory.empty() && lower(dir + "\\").rfind(lower(samples), 0) == 0) return; // the engine's samples: every module
    m_projectAllModules = false;
    std::error_code ec;
    const std::string root = m_project.directory + "/modules";
    const std::filesystem::path modules(std::u8string(reinterpret_cast<const char8_t*>(root.data()), root.size()));
    for (std::filesystem::directory_iterator it(modules, ec), end; !ec && it != end; it.increment(ec)) {
        const std::u8string name8 = it->path().filename().u8string();
        const std::string name(reinterpret_cast<const char*>(name8.data()), name8.size());
        std::string text;
        const std::string manifest = root + "/" + name + "/module.txt";
        if (!readTextFile(manifest, text)) continue;
        if (text.find("\ntype engine") == std::string::npos && text.rfind("type engine", 0) != 0) continue;
        m_projectModules.push_back(name);
    }
    MEALKIT_INFO("Project engine modules: %s", m_projectModules.empty() ? "none (kernel only)" : [&] {
        std::string all;
        for (const std::string& m : m_projectModules) all += (all.empty() ? "" : " ") + m;
        return all;
    }().c_str());
}

// Table here, not in EditorCommands.cpp: tools\check_strings.ps1 reads that file's {"..."} pairs as UI text
void Editor::tagCommandModules() {
    // Commands of a window or page that belongs to an engine module: hidden and refused while the project doesn't have that module
    struct ModuleOf {
        const char* command;
        const char* module;
    };
    static constexpr ModuleOf kModuleOf[] = {
        {"Viewport", "scene"},
        {"Assets", "asset"},     {"Data", "data"},         {"Tuning", "data"},     {"Tables", "data"},
        {"Feedback", "feedback"}, {"Effects", "feedback"}, {"Sounds", "audio"},    {"State", "sim"},
        {"Previews", "render"},
    };
    for (Command& c : m_commands)
        for (const ModuleOf& m : kModuleOf)
            if (std::strcmp(c.name, m.command) == 0) {
                c.module = m.module;
                c.enabled = [this, module = m.module, enabled = std::move(c.enabled)] { return projectHas(module) && enabled(); };
            }
}

bool Editor::projectHas(const char* module) const {
    if (m_projectAllModules) return true;
    return std::find(m_projectModules.begin(), m_projectModules.end(), module) != m_projectModules.end();
}

void Editor::closePanelsOfMissingModules() {
    if (m_projectAllModules) return;
    if (!projectHas("scene")) m_sceneOpen = false;
    if (!projectHas("asset")) m_assetsOpen = false;
    if (!projectHas("data")) m_dataOpen = false;
    if (!projectHas("render")) m_previewOpen = false;
    if (!projectHas("ui")) m_uiWorkspaceOpen = false;
}

// ---------------------------------------------------------------- Project builds

void Editor::setProject(const ProjectInfo& project) {
    m_project = project;
    readProjectModules();
    loadViewState(); // the scene opens before the project
    // 2D project still on a 3D view (first open, or a view saved before view2d was set): face the XY plane around the origin
    if (m_project.settings.view2d && (!m_ortho || m_cameraPitch != 0.0f || m_cameraYaw != 0.0f)) {
        m_ortho = true;
        m_cameraYaw = 0.0f;
        m_cameraPitch = 0.0f;
        m_orthoHalfHeight = 8.0f;
        m_cameraPos = {0.0f, 3.0f, -20.0f};
    }
    loadRecentScenes();
    if (!m_scenePath.empty()) rememberRecentScene(m_scenePath);
    { // recent palette items, per project
        std::string text;
        m_paletteRecent.clear();
        if (readTextFile(project.directory + "\\build\\editor\\palette_recent.txt", text)) {
            size_t start = 0;
            for (size_t end = text.find('\n'); end != std::string::npos; start = end + 1, end = text.find('\n', start))
                if (end > start) m_paletteRecent.push_back(text.substr(start, end - start));
        }
    }
    // Window layout per project (build\editor\layout.ini). A project opened for the first time gets the default layout.
    // Automated runs (--frames, captures) always start from the default layout and save none, so they never change the user's
    if (m_automated) {
        ImGui::GetIO().IniFilename = nullptr;
        m_layoutReset = true;
    } else if (m_prefs.projectLayout && !project.directory.empty()) {
        const std::string folder = project.directory + "\\build\\editor";
        createDirectories(folder);
        if (!m_imgui.setIniPath(folder + "\\layout.ini")) m_layoutReset = true;
        m_panelStatePath = folder + "\\panels.txt";
        loadPanelState();
    }
    m_tablesPanel.setProjectDirectory(project.directory);
    m_tuningPanel.setProjectDirectory(project.directory);
    m_feedbackPanel.setProjectDirectory(project.directory);
    m_uiPanel.setOpen([this](const std::string& file) { return openInDesigner(file); });
    m_uiPanel.refresh();
    applyRenderStyle(project.settings);
    live::ServerConfig lc;
    lc.kind = "editor";
    lc.projectDir = project.directory;
    lc.projectName = project.name();
    lc.port = live::kEditorPort;
    m_live.start(lc);
    m_live.addTopic("stats"); // performance numbers in the editor status bar (game::FrameStats::json)
    audio::addLiveService(m_live, [this] { return m_audio; }); // MkAudio: playback in the editor, reload, status
    ui::addLiveService(m_live, [this] { return m_gameUi; }); // MkUI Designer: the play session's screens and view-model values
    addSceneLink(); // MkRender: play/stop/look through the saved scene
    m_tools = findTools(m_toolPaths);
    m_installableTools = notInstalledTools(m_tools);
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
    if (kind != BuildKind::Upgrade && !hasCompiler()) {
        setStatus(tr("This PC has no C++ compiler: building is for programmers. Use Get Latest Game Code."), true);
        return;
    }
    if (kind == BuildKind::Export && m_projectPanel.dirty() && !m_projectPanel.save(m_project)) return;
    m_buildPanel.start(kind, m_project, m_engineDirectory, m_time);
}

void Editor::pollBuild() {
    if (!m_buildPanel.poll(m_project, m_time)) return;
    const BuildKind kind = m_buildPanel.kind();
    if (!m_buildPanel.succeeded()) {
        if (m_buildPanel.engineBuildFailed()) m_engineRestartPrompt = true; // the engine changed under this editor
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

// The engine changed while this editor runs: its mealkit.dll is in use, so the build can't relink it. Offer to close, rebuild and reopen
void Editor::drawEngineRestartPrompt() {
    if (m_engineRestartPrompt) {
        ImGui::OpenPopup("##engineRestart");
        m_engineRestartPrompt = false;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("##engineRestart", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) return;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted(tr("The engine changed, but this editor is still using the old engine (mealkit.dll), so the engine can't be rebuilt while it is open."));
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("Restart saves your changes, closes the editor, builds the engine in a console window and opens this project again. The game code is built when it opens."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (ImGui::Button(tr("Save, Restart and Rebuild"))) {
        restartAndRebuild();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Later"))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

bool Editor::restartAndRebuild() {
    const std::string script = m_engineDirectory + "\\tools\\rebuild_editor.ps1";
    if (!fileExists(script)) {
        setStatus(tr("This engine has no tools\\rebuild_editor.ps1: close the editor and rebuild the engine's editor"), true);
        return false;
    }
    saveAll();
    const std::string args = "-NoProfile -ExecutionPolicy Bypass -File \"" + script + "\" -Project \"" + m_project.directory + "\" -WaitPid " +
                             std::to_string(processId());
    if (!launchDetached("powershell.exe", args, m_engineDirectory)) {
        setStatus(tr("Could not start the engine rebuild"), true);
        return false;
    }
    MEALKIT_INFO("Restarting to rebuild the engine: %s", script.c_str());
    m_quit = true;
    return true;
}

void Editor::drawBuildLog() {
    if (m_buildPanel.takeOpenRequest()) openDebugPage(DebugPage::Build);
}

void Editor::saveAll() {
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
                if (iconButton(icon::kClose, tr("Hide"))) n.time = -1.0;
                ImGui::SameLine();
            }
            ImGui::TextColored(n.failed ? ImVec4(1.0f, 0.50f, 0.45f, 1.0f) : ImVec4(0.60f, 0.88f, 0.60f, 1.0f), "%s",
                               n.text.c_str());
            if (n.open != NoticeOpen::None && ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (n.open != NoticeOpen::None && ImGui::IsItemClicked()) {
                const bool build = n.open == NoticeOpen::BuildLog;
                openDebugPage(build ? DebugPage::Build : DebugPage::Log);
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
    ImGui::GetBackgroundDrawList()->AddRect(a, b, ImGui::GetColorU32(color::kPlay), 0.0f, 0, 4.0f);
}

void Editor::drawEmptyViewport() {
    // A small card at the top of the picture, under every other window, that the user can close (the status bar keeps the game code status)
    if (m_mode != Mode::Edit || m_gameLoaded || m_project.directory.empty() || m_noGameHintClosed || !m_sceneVisible) return;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    ImGui::SetNextWindowPos(ImVec2(m_viewport.x + m_viewport.width * 0.5f, m_viewport.y + 12.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    if (ImGui::Begin("##NoGameCode", nullptr, flags)) {
        // Close at the right end of the first row; it stays closed until the project is opened again
        const auto closeButton = [this] {
            ImGui::SameLine();
            if (iconButton(icon::kClose, tr("Hide"))) m_noGameHintClosed = true;
        };
        if (!projectHas("game")) { // kernel-only or without the game module: there is no game for the editor to load
            ImGui::TextUnformatted(tr("This project has no game module, so there is nothing to play here."));
            closeButton();
            ImGui::TextDisabled("%s", tr("Add engine modules in Window > Modules."));
            if (ImGui::Button(tr("Modules"))) m_modulesOpen = true;
            ImGui::End();
            return;
        }
        if (!hasCompiler()) {
            ImGui::TextUnformatted(m_gameStatus.c_str());
            closeButton();
            ImGui::End();
            return;
        }
        ImGui::TextUnformatted(tr("The game code is not built yet. Build it to play."));
        closeButton();
        ImGui::BeginDisabled(m_buildPanel.running());
        if (iconButton(icon::kBuild, tr("Build Game Code"))) startBuild(BuildKind::GameCode);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (iconButton(icon::kLog, tr("Build Log"))) {
            openDebugPage(DebugPage::Build);
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
