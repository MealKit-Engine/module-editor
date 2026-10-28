#include "mealkit/editor/Editor.h"

#include "mealkit/core/Log.h"
#include "mealkit/core/Profile.h"
#include "mealkit/core/Version.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Strings.h"
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
// 아래 가로 칸에 탭으로 붙는 창 (기본 레이아웃)
const ImVec4 kPlayColor(0.42f, 0.24f, 0.08f, 1.0f); // 플레이 중 메뉴 막대
const char* const kBottomWindows[] = {LogPanel::kWindow, BuildPanel::kWindow, TablesPanel::kWindow};

// 대소문자를 무시한 부분 문자열 (씬 트리 찾기)
bool containsNoCase(const std::string& text, const char* needle) {
    const size_t n = strlen(needle);
    for (size_t i = 0; i + n <= text.size(); ++i)
        if (_strnicmp(text.data() + i, needle, n) == 0) return true;
    return false;
}

constexpr size_t kUndoLimit = 100;
const Vec3 kSpawnSize{0.8f, 1.8f, 0.8f};

constexpr const char* kEntityPayload = "MEALKIT_ENTITY";
constexpr f32 kAreaEdgeDraw = 0.0025f; // 영역 모서리 두께 (카메라 거리의 비율, 멀어도 보이게)
constexpr f32 kAreaEdgePick = 0.6f;   // 영역 모서리를 누를 수 있는 두께 (m)

// 영역 상자의 모서리 12개: 물체 로컬(크기 적용) 공간의 중심과 반 크기
void areaEdges(Vec3 half, f32 thickness, Vec3 centers[12], Vec3 halves[12]) {
    const f32 t = thickness * 0.5f;
    int n = 0;
    for (int a = -1; a <= 1; a += 2)
        for (int b = -1; b <= 1; b += 2) {
            const f32 fa = static_cast<f32>(a), fb = static_cast<f32>(b);
            centers[n] = {0.0f, fa * half.y, fb * half.z}, halves[n++] = {half.x + t, t, t}; // x 방향
            centers[n] = {fa * half.x, 0.0f, fb * half.z}, halves[n++] = {t, half.y + t, t}; // y 방향
            centers[n] = {fa * half.x, fb * half.y, 0.0f}, halves[n++] = {t, t, half.z + t}; // z 방향
        }
}

// 행 벡터 규약 TRS 행렬 (ImGuizmo와 같은 메모리 배치: 이동이 [12..14])
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
    // 회전 행렬 R(열 규약)의 원소 Rij = (행 j)의 i번째 성분
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

// 광선과 회전된 상자(엔티티 로컬 공간의 center ± half)의 교차 거리. 맞지 않으면 음수.
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

const FileFilter kSceneFilters[] = {{"MealKit scene (*.scene)", "*.scene"}, {"All files (*.*)", "*.*"}};
const FileFilter kModelFilters[] = {{"glTF model (*.glb, *.gltf)", "*.glb;*.gltf"}, {"All files (*.*)", "*.*"}};

bool toolButton(const char* label, bool active) {
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor();
    return pressed;
}

// root 폴더 안의 파일이면 root 기준 상대 경로('/' 구분), 밖이면 빈 문자열
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
    focusPanel(kInspectorWindow); // 편집 모드로 시작: 인스펙터를 앞으로
    if (!m_imgui.init(window, renderer, executableDirectory() + "\\mealkit_editor.ini")) {
        MEALKIT_ERROR("Editor UI initialization failed");
        return false;
    }
    if (fileExists(scenePath)) {
        if (!loadScene(scenePath)) {
            m_scene = scene::makeDefault();
            m_scenePath.clear(); // 망가진 파일을 덮어쓰지 않는다
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
    if (m_perf.update(dt, {}) && m_live.listening("stats")) m_live.publish("stats", m_perf.json());

    m_live.update();
    for (const std::string& e : m_import.takeEvents()) {
        setStatus(e);
        m_uiPanel.refresh(); // 새 .ui 파일 (고친 파일은 ui::System이 다시 읽는다)
    }
    updateHotReload(dt);

    m_imgui.newFrame();
    ImGuizmo::BeginFrame();
    if (!m_editActive) m_pending = capture();
    m_changed = false;

    ImGui::GetStyle().FontScaleMain = m_prefs.uiScale;
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
    m_proj = perspectiveReversedZ(kFovY, aspect, 0.1f, 2000.0f);
    m_view = lookAtLH(m_cameraPos, m_cameraPos + cameraForward(), {0, 1, 0});

    const bool editing = m_mode == Mode::Edit;
    drawHierarchy(editing);
    drawInspector(editing);
    m_tuningPanel.draw(kInspectorWindow, m_mode == Mode::Play, m_playPlayers > 1, m_tablesPanel.index());
    if (m_tablesOpen) m_tablesPanel.draw(m_window->nativeHandle(), editing, &m_tablesOpen);
    if (m_soundsOpen && m_audio) m_soundPanel.draw(*m_audio, &m_soundsOpen);
    if (m_feedbackOpen) m_feedbackPanel.draw(m_feedback, m_cameraPos + cameraForward() * 10.0f, dt, &m_feedbackOpen);
    if (m_uiPanelOpen) m_uiPanel.draw(&m_uiPanelOpen);
    if (m_lookdevOpen && m_renderer) m_lookdev.draw(*m_renderer, &m_lookdevOpen);
    // 소리 이벤트 표(화면 전용)를 고치면 저장해 바로 들리게 한다 (오디오가 파일을 다시 읽는다)
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
    drawState();
    pollBuild();
    pollLayoutCheck();
    drawBuildLog();
    m_logPanel.draw();
    takePanelStatus();
    drawNotices();
    m_prefsPanel.draw(m_prefs);
    drawShortcutHelp();
    drawPlayFrame();
    drawEmptyViewport();
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false) && rhi::gpuCaptureTool()) captureGpuFrame(); // 편집·플레이 모두

    runShortcuts();
    drawCommandPalette();
    if (editing) {
        drawGizmo();
        drawViewportToolbar();
        handlePicking();
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        m_stopRequest = true;
    } else if (m_simDebug && m_canPause && m_simDebug->timeline().recording) {
        if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) m_simDebug->setPaused(!m_simDebug->timeline().paused);
        if (ImGui::IsKeyPressed(ImGuiKey_F7)) m_simDebug->stepOnce();
    }
    reportSimProblems();

    // 되돌리기: 편집이 시작된 순간의 상태(프레임 시작 시 캡처)를 한 번만 쌓는다. 드래그 중에는 쌓지 않는다.
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

// 플레이 중 메뉴 막대: 일시정지, 한 틱, 타임라인(기록된 범위), 그림자 시뮬레이션 상태
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
    m_focusFrames = 3; // 도킹 노드가 아직 없는 첫 프레임에는 포커스가 안 먹으므로 몇 프레임 동안 요청한다
}

void Editor::focusPanelNow() {
    ImGui::SetNextWindowFocus();
    if (--m_focusFrames <= 0) m_focusPanel = nullptr;
}

// 에셋 핫 리로드: 외부 도구(Blender 등)가 내보낸 모델과 밖에서 고친 씬을 다시 읽는다. 플레이 중에는 하지 않는다
// (모델의 충돌 껍질은 시뮬레이션에 닿으므로 매치 중에 바꾸면 피어가 갈린다). 플레이가 끝나면 그때 바뀐 것을 읽는다
void Editor::updateHotReload(f64 dt) {
    constexpr f64 kHotReloadSeconds = 0.5; // 파일 시각을 보는 간격
    m_hotReloadTimer -= dt;
    if (m_hotReloadTimer > 0.0 || m_mode != Mode::Edit) return;
    m_hotReloadTimer = kHotReloadSeconds;

    // 프로젝트 설정 창에서 바꾼 조명 모델·툰 램프, 밖에서 고친 램프 그림
    applyRenderStyle(m_project.settings);

    // 프로젝트 설정 창에서 바꾼 에셋 예산 (저장하면 m_project가 바뀐다)
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
    if (m_dirty) { // 고친 것이 있으면 덮어쓰지 않는다 (저장하면 디스크 쪽이 덮인다)
        if (!m_sceneChangedWarned) setStatus(tr("The scene file changed on disk; you have unsaved changes"));
        m_sceneChangedWarned = true;
        return;
    }
    const u32 selected = m_selected;
    if (loadScene(m_scenePath)) {
        if (m_scene.find(selected)) m_selected = selected; // 같은 id가 있으면 선택을 지킨다
        MEALKIT_INFO("Hot reload: scene %s", m_scenePath.c_str());
        setStatus(trf("Reloaded scene: %s", fileName(m_scenePath).c_str()));
    }
}

void Editor::updateAutosave(f64 dt) {
    if (m_changed) m_autosavePending = true;
    if (m_mode != Mode::Edit) return;
    m_autosaveTimer += dt;
    // 저장 안 된 변경이 있으면 환경설정 간격으로 <씬>.autosave에 쓴다 (0 = 끄기)
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
    frame.cameraPos = m_cameraPos;
    fillEnvironment(frame);
    m_models->probes(m_scene, m_probeList);
    frame.probes = m_probeList.data();
    frame.probeCount = static_cast<u32>(m_probeList.size());
    frame.debugView = m_debugView;
    addSceneObjects(out, m_cameraPos, false);
    if (m_showGrid && m_mode == Mode::Edit && !m_automated) addGrid(out); // 자동 캡처·골든 이미지는 그리드 없이
}

void Editor::addSceneObjects(render::DrawList& out, Vec3 eye, bool gameView) const {
    const render::MeshId box = m_models->boxMesh();
    for (const Entity& e : m_scene.entities) {
        if (gameView && !scene::visibleInGame(e, m_gameTypes)) continue;
        if (!gameView && isHidden(e)) continue;
        const scene::Transform world = scene::worldTransform(m_scene, e);
        if (isArea(e)) { // 보이지 않는 영역: 안이 가려지지 않게 모서리만
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
        if (e.is(scene::kTypePointLight)) { // 점광원: 빛 색의 작은 상자 (빛을 가리지 않게)
            inst.scale = {0.2f, 0.2f, 0.2f};
            inst.color = scene::readObject<scene::PointLightFields>(e).color.value;
        }
        if (!gameView && isSelected(e.id)) inst.flags |= render::kInstanceHighlight;
        const asset::Model* model = e.model.empty() ? nullptr : m_models->get(e.model);
        const render::MeshId mesh = model ? m_models->pickLod(*model, eye, world.position, world.scale) : box;
        if (model && m_debugView != render::DebugView::None && !gameView) { // 보기 모드: 그린 LOD 단계, 예산 초과
            for (u32 lod = 0; lod < model->lodCount; ++lod)
                if (model->lods[lod] == mesh) inst.flags |= lod << render::kInstanceLodShift;
            if (!model->budgetProblems.empty()) inst.flags |= render::kInstanceOverBudget;
        }
        if (model) asset::addModel(out, *model, mesh, inst, scene::animField(e), m_animTime); // 스킨 모델은 anim 필드의 클립
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
    u32 size = 16; // 2의 거듭제곱 (밉이 반씩 줄어든다)
    while (size * 2 <= std::clamp<u32>(m_project.settings.probeSize, 16, 512)) size *= 2;
    if (!m_probeTarget || m_probeTargetSize != size) {
        m_probeTarget = renderer.createRenderTarget(size, size); // 크기를 바꾸면 옛 대상은 남는다 (렌더러가 지우지 않는다)
        m_probeTargetSize = size;
        m_probeFace = 0;
        m_probeCapturePending = false;
    }
    if (m_probeCapturePending) { // 지난 프레임에 그린 면: sRGB 8비트 → 선형
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
                    m_models->reloadChanged(); // 같은 경로를 다시 구웠으면 다시 올린다
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
    // 큐브 면 방향 (asset::cubeDirection과 같은 배치: 왼손 lookAt의 오른쪽 = cross(위, 앞))
    static const Vec3 kForward[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const Vec3 kUp[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
    const Vec3 eye = scene::worldTransform(m_scene, *e).position;
    render::FrameParams frame;
    frame.viewProj = lookAtLH(eye, eye + kForward[m_probeFace], kUp[m_probeFace]) *
                     perspectiveReversedZ(3.14159265f * 0.5f, 1.0f, 0.05f, 2000.0f);
    frame.cameraPos = eye;
    frame.cameraForward = kForward[m_probeFace];
    fillEnvironment(frame); // 프로브는 쓰지 않는다 (다른 프로브의 옛 그림이 섞이지 않게)
    frame.post.toneMapping = false; // 반사는 선형 값 그대로 (톤 매핑은 그 반사를 받는 화면에서 한 번)
    render::DrawList list;
    addSceneObjects(list, eye, true);
    renderer.drawToTarget(m_probeTarget, frame, list, static_cast<render::LightingModel>(m_project.settings.lightingModel));
    renderer.captureTarget(m_probeTarget, &m_probeCapture);
    m_probeCapturePending = true;
}

void Editor::renderGradeCapture(render::Renderer& renderer) {
    if (m_gradeCapturePending) { // 지난 프레임에 그린 것을 endFrame이 읽었다
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
    if (!m_gradeTarget || m_gradeTargetSize[0] != w || m_gradeTargetSize[1] != h) { // 크기가 바뀌면 옛 대상은 남는다
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
    frame.post.lut = 0; // 보정 전 그림 (톤 매핑까지)
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
    m_selected = 0; // 선택 강조 없이
    m_animTime = 0.0; // 애니메이션은 클립의 어긋남(anim 필드)만
    m_viewport = {0.0f, 0.0f, width, height};
    m_proj = perspectiveReversedZ(kFovY, width / std::max(height, 1.0f), 0.1f, 2000.0f);
    m_view = lookAtLH(m_cameraPos, m_cameraPos + cameraForward(), {0, 1, 0});
    buildSceneView(frame, drawList);
}

void Editor::renderLookdev(render::Renderer& renderer, f64 dt) {
    if (!m_lookdevOpen || !m_models) return;
    // 고른 물체의 모델을 따라간다 (모델 없는 물체를 고르면 앞의 모델을 그대로 둔다)
    if (const Entity* e = selected(); e && !e->model.empty()) m_lookdev.setModel(e->model);
    m_lookdev.render(renderer, *m_models, static_cast<render::LightingModel>(m_project.settings.lightingModel), dt);
}

void Editor::captureGpuFrame() {
    if (rhi::captureNextGpuFrame()) setStatus(trf("GPU capture: next frame -> %s", rhi::gpuCaptureFolder().c_str()));
}

void Editor::setMode(Mode mode) {
    m_mode = mode;
    m_looking = false;
    m_window->setCursorCaptured(false);
    if (mode == Mode::Edit) m_playStatus.clear();
    focusPanel(mode == Mode::Play ? kStateWindow : kInspectorWindow); // 플레이 중에는 [상태] 탭을 앞으로
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
    if (index == -2) { // 씬 줄 (인스펙터 = 환경)
        m_selected = 0;
        m_sceneRow = true;
        return;
    }
    if (index >= 0 && static_cast<size_t>(index) < m_scene.entities.size())
        selectMany({m_scene.entities[static_cast<size_t>(index)].id}, add);
}

// ---------------------------------------------------------------- UI

// 미리 정한 레이아웃: 왼쪽 씬 트리, 오른쪽 기둥 하나(인스펙터 + [상태]·[튜닝] 탭), 아래 가로 칸(빌드 로그·표).
// 플레이는 아래 칸을 줄이고 [상태]를 앞으로, 룩데브는 오른쪽 위에 [룩데브] 창을 크게 둔다.
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
    if (!editing) ImGui::PushStyleColor(ImGuiCol_MenuBarBg, kPlayColor); // 플레이 중인 것을 헷갈리지 않게
    const bool open = ImGui::BeginMainMenuBar();
    if (!editing) ImGui::PopStyleColor();
    if (!open) return;

    if (ImGui::BeginMenu(tr("File"), editing)) {
        commandMenuItem("New Scene");
        commandMenuItem("Open...");
        commandMenuItem("Save");
        commandMenuItem("Save As...");
        ImGui::Separator();
        commandMenuItem("Project Settings...");
        commandMenuItem("Preferences...");
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Exit"))) m_quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Edit"), editing)) {
        commandMenuItem("Undo");
        commandMenuItem("Redo");
        ImGui::Separator();
        commandMenuItem("Select All");
        commandMenuItem("Duplicate");
        commandMenuItem("Delete");
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
        // 보기 모드: 무엇이 비싼지 색으로 (편집 화면, 출시 빌드에는 없다)
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
        // GPU 캡처 (--gpu-capture로 RenderDoc·PIX를 불러왔을 때만)
        if (const char* tool = rhi::gpuCaptureTool()) {
            if (ImGui::MenuItem(trf("GPU Capture (%s)", tool).c_str(), "F11")) captureGpuFrame();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Build"), !m_project.directory.empty())) {
        commandMenuItem("Build Game Code (Hot Reload)");
        commandMenuItem("Release Build");
        ImGui::Separator();
        commandMenuItem("Open Release Folder");
        commandMenuItem("Open Project Folder");
        ImGui::Separator();
        commandMenuItem("Build Log", m_buildPanel.openFlag());
        commandMenuItem("Log", m_logPanel.openFlag());
        ImGui::EndMenu();
    }
    // 도구 제품군 (editor/Tools.h): 별도 저장소의 MealKit 도구를 이 프로젝트로 연다
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
    ImGui::BeginDisabled(editing && !m_gameLoaded); // 게임 DLL이 없으면 플레이할 수 없다
    if (ImGui::Button(playLabel.c_str())) {
        if (editing) m_playRequest = true;
        else m_stopRequest = true;
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
    if (editing && !m_gameLoaded && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", m_gameStatus.c_str());

    // 플레이 인원 (여러 명이면 참가 창을 더 띄운다)
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
            ImGui::TextDisabled("%.0f fps", m_perf.sample().fps);
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
    // 찾기 (이름·종류, Ctrl+F): 글자가 있으면 맞는 물체만 평평하게
    if (m_treeSearchFocus) {
        ImGui::SetKeyboardFocusHere();
        m_treeSearchFocus = false;
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##treeSearch", (std::string(icon::kSearch) + "  " + tr("Find (Ctrl+F)")).c_str(), m_treeSearch,
                             sizeof(m_treeSearch));
    ImGui::Separator();

    if (ImGui::BeginChild("##entities")) {
        // 부모 → 자식 목록 (entities 순서)
        std::unordered_map<u32, std::vector<size_t>> children;
        std::vector<size_t> roots;
        for (size_t i = 0; i < m_scene.entities.size(); ++i) {
            const Entity& e = m_scene.entities[i];
            if (e.parent && m_scene.find(e.parent)) children[e.parent].push_back(i);
            else roots.push_back(i);
        }
        m_treeAction = {};
        // 맨 위 고정 줄 "씬": 고르면 인스펙터가 씬 환경을 보여 준다 (docs/DECISIONS.md 2026-10-28)
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
        // 빈 곳에 놓으면 최상위로
        const ImVec2 rest = ImGui::GetContentRegionAvail();
        ImGui::InvisibleButton("##treeRoot", ImVec2(std::max(rest.x, 1.0f), std::max(rest.y, ImGui::GetFrameHeight())));
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload))
                m_treeAction = {TreeAction::Reparent, *static_cast<const u32*>(payload->Data), 0};
            ImGui::EndDragDropTarget();
        }
    }
    ImGui::EndChild();
    // 트리를 다 그린 뒤에 구조를 바꾼다 (그리는 도중에 목록이 바뀌지 않게)
    switch (m_treeAction.kind) {
    case TreeAction::Reparent: reparent(m_treeAction.entity, m_treeAction.parent); break;
    case TreeAction::Duplicate: duplicateSelected(); break;
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
    // 새로 고른 물체는 조상을 펼치고 그 줄로 스크롤한다 (뷰포트에서 골랐을 때)
    const bool reveal = m_selected != m_treeRevealed && m_selected != 0;
    if (reveal && !leaf && m_scene.find(m_selected) && scene::isDescendant(m_scene, m_selected, id))
        ImGui::SetNextItemOpen(true);
    // 라벨 앞에 종류 아이콘 자리를 비워 두고 물체 색으로 그린다
    const bool hiddenRow = isHidden(e);
    if (hiddenRow) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
    const bool open = ImGui::TreeNodeEx("##node", flags, "      %s", label.c_str());
    if (reveal && m_selected == id) ImGui::SetScrollHereY();
    const ImVec2 rowMin = ImGui::GetItemRectMin(), rowMax = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddText(ImVec2(rowMin.x + ImGui::GetTreeNodeToLabelSpacing(), rowMin.y),
                                        ImGui::ColorConvertFloat4ToU32(ImVec4(rgb[0], rgb[1], rgb[2], 1.0f)),
                                        entityIcon(e));
    if (hiddenRow) ImGui::PopStyleVar();
    // 클릭 선택, 더블클릭 이동, 끌어서 부모 바꾸기
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
    // 줄 오른쪽 보이기·잠금 토글 (마우스를 올렸거나 켜져 있을 때만 보인다)
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
    const scene::Transform world = scene::worldTransform(m_scene, *e); // 화면에서 제자리를 지킨다
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
    if (m_selected != 0) m_sceneRow = false; // 물체를 고르면 씬 줄 선택은 풀린다
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
        drawObjectFields(*e);

        if (const asset::Model* model = e->model.empty() ? nullptr : m_models->get(e->model); model && model->skinned()) {
            // 스킨 모델: 화면용 반복 클립 (씬 필드 anim, 게임 화면도 같이 재생한다)
            ImGui::SeparatorText(tr("Animation"));
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

        if (e->is(scene::kTypeProbe)) { // 반사 프로브: 굽기 (편집 모드, 구운 파일은 assets/probes/)
            ImGui::SeparatorText(tr("Reflection probe"));
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

        if (!isMarker(*e)) {
            ImGui::SeparatorText(tr("Model"));
            if (e->model.empty()) {
                ImGui::TextDisabled("%s", tr("Default box"));
            } else {
                ImGui::TextUnformatted(e->model.c_str());
                if (const asset::Model* model = m_models->get(e->model)) {
                    ImGui::TextDisabled(tr("%u triangles · %u LODs"), model->triangles[0], model->lodCount);
                    // 에셋 예산: 종류(폴더)의 한도 대비 사용량, 넘으면 빨강 (mealkit.project budget*)
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

        ImGui::SeparatorText(e->parent ? tr("Transform (relative to parent)") : tr("Transform"));
        f32 matrix[16], t[3], r[3], s[3];
        compose(scene::localTransform(*e), matrix);
        ImGuizmo::DecomposeMatrixToComponents(matrix, t, r, s);
        bool changed = ImGui::DragFloat3(tr("Position"), t, 0.05f, 0.0f, 0.0f, "%.2f");
        changed |= ImGui::DragFloat3(tr("Rotation (deg)"), r, 0.5f, 0.0f, 0.0f, "%.1f");
        if (!isMarker(*e)) changed |= ImGui::DragFloat3(tr("Scale"), s, 0.02f, 0.01f, 10000.0f, "%.2f");
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

        ImGui::SeparatorText(tr("Look"));
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
        ImGui::PopItemWidth();
        ImGui::TextDisabled("ID %u", e->id);

        ImGui::Spacing();
        if (ImGui::Button(tr("Duplicate (Ctrl+D)"))) duplicateSelected();
        ImGui::SameLine();
        if (ImGui::Button(tr("Delete (Del)"))) deleteSelected(); // 이후 e를 쓰지 않는다
    }
    ImGui::EndDisabled();
    ImGui::End();
}

// 씬 환경 (인스펙터에서 씬 줄을 골랐을 때)
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
    // 하늘 HDRI: assets/textures/ 아래의 .hdr 하나 (없음 = 위의 색 그라디언트). 처음 고르면 굽느라 잠깐 멈춘다
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

    // 포스트 프로세싱 (scene::PostFields → render::PostParams). 화면용 값, 플레이어 설정이 효과를 끌 수 있다
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

// ---------------------------------------------------------------- 정의 데이터 표 ([표] 패널)

void Editor::saveTablesForPlay() {
    std::string status;
    if (!m_tablesPanel.dirty()) return;
    if (m_tablesPanel.saveAll(&status)) setStatus(trf("Tables saved for play: %s", status.c_str()));
    else setStatus(status);
}

// 패널 밖 데이터(튜닝, 씬 물체 필드)의 행 참조도 [표] 패널의 "없는 행 참조" 목록에 넣는다 (0.5초마다)
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

// 플레이 중인 시뮬레이션 상태 (상태 필드 선언을 그대로 보여 준다)
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

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetRect(m_viewport.x, m_viewport.y, m_viewport.width, m_viewport.height);
    f32 matrix[16];
    compose(scene::worldTransform(m_scene, *e), matrix); // 기즈모는 월드 공간, 결과는 부모 기준으로 되돌려 넣는다

    const ImGuizmo::OPERATION ops[3] = {ImGuizmo::TRANSLATE, ImGuizmo::ROTATE, ImGuizmo::SCALE};
    ImGuizmo::OPERATION op = ops[m_gizmoOp];
    if (isMarker(*e) && op == ImGuizmo::SCALE) op = ImGuizmo::TRANSLATE;
    const ImGuizmo::MODE mode = (m_gizmoLocal || op == ImGuizmo::SCALE) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    const f32 step = op == ImGuizmo::TRANSLATE ? 0.5f : op == ImGuizmo::ROTATE ? 15.0f : 0.1f;
    const f32 snap[3] = {step, step, step};
    const bool snapping = ImGui::GetIO().KeyCtrl != m_snapOn; // 툴바 스냅이 켜져 있으면 Ctrl은 잠시 끄기

    if (ImGuizmo::Manipulate(&m_view.m[0][0], &m_proj.m[0][0], op, mode, matrix, nullptr, snapping ? snap : nullptr)) {
        scene::Transform world;
        decompose(matrix, world);
        const scene::Transform before = scene::worldTransform(m_scene, *e);
        scene::setWorldTransform(m_scene, *e, world);
        if (!m_multi.empty()) moveOthersWith(before, world);
        markChanged();
    }
}

// 뷰포트 위에 겹쳐 그리는 툴바(왼쪽 위)와 축 방향 표시(오른쪽 위, 축을 누르면 그쪽에서 본다)
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
        ImGui::SameLine(0, 2);
        if (toolButton(tr("Grid"), m_showGrid)) m_showGrid = !m_showGrid;
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

    // 축 방향 표시: ImGuizmo가 보기 행렬을 돌리면 카메라 각도·위치로 되돌려 넣는다
    const f32 size = 96.0f;
    Mat4 view = m_view;
    const f32 pivot = 10.0f; // 이 거리 앞의 점을 중심으로 돈다
    ImGuizmo::ViewManipulate(&view.m[0][0], pivot, ImVec2(m_viewport.x + m_viewport.width - size - 8.0f, m_viewport.y + 8.0f),
                             ImVec2(size, size), IM_COL32(30, 30, 34, 120));
    if (ImGuizmo::IsUsingViewManipulate() || std::memcmp(&view, &m_view, sizeof(view)) != 0) {
        // 행 벡터 보기 행렬: 열 2가 앞 방향, 카메라 위치 = -t * R^T
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

// 바닥(y = 0) 그리드: 카메라 아래 주변만, 스냅 이동 단위 간격. 깊이가 있게 얇은 상자로 그린다
void Editor::addGrid(render::DrawList& out) const {
    const f32 cell = 0.5f * 2.0f; // 스냅 0.5 m의 두 칸 = 1 m (스냅 설정 항목에서 바꾼다)
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

// ---------------------------------------------------------------- 입력

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
        if (inViewport && !io.WantCaptureMouse) {
            if (io.MouseWheel != 0.0f) m_cameraPos += forward * (io.MouseWheel * 3.0f);
            if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) // 가운데 버튼 드래그: 화면 이동
                m_cameraPos += (right * -input.mouseDeltaX() + up * input.mouseDeltaY()) * 0.03f;
        }
    }
}

// 왼쪽 클릭 = 고르기(Shift = 더하기·빼기), 빈 곳에서 끌기 = 상자 안의 물체 고르기
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
                if (cw <= 0.0f) continue; // 카메라 뒤
                const f32 sx = m_viewport.x + (cx / cw * 0.5f + 0.5f) * m_viewport.width;
                const f32 sy = m_viewport.y + (0.5f - cy / cw * 0.5f) * m_viewport.height;
                if (sx >= lo.x && sx <= hi.x && sy >= lo.y && sy <= hi.y) inside.push_back(e.id);
            }
            selectMany(inside, io.KeyShift);
            return;
        }
    } else {
        if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left) || io.WantCaptureMouse || ImGuizmo::IsOver() ||
            ImGuizmo::IsUsing() || ImGuizmo::IsViewManipulateHovered() || ImGuizmo::IsUsingViewManipulate())
            return;
        const ImVec2 mouse = io.MousePos;
        if (mouse.x < m_viewport.x || mouse.y < m_viewport.y || mouse.x >= m_viewport.x + m_viewport.width ||
            mouse.y >= m_viewport.y + m_viewport.height)
            return;
        m_boxSelecting = true; // 놓을 때 클릭인지 끌기인지 정한다
        m_boxStart = {mouse.x, mouse.y};
        return;
    }
    const ImVec2 mouse(m_boxStart.x, m_boxStart.y);

    const f32 nx = (mouse.x - m_viewport.x) / m_viewport.width * 2.0f - 1.0f;
    const f32 ny = 1.0f - (mouse.y - m_viewport.y) / m_viewport.height * 2.0f;
    const f32 tanHalf = std::tan(kFovY * 0.5f);
    const f32 aspect = m_viewport.width / std::max(m_viewport.height, 1.0f);
    const Vec3 forward = cameraForward();
    const Vec3 right = normalize(cross({0, 1, 0}, forward));
    const Vec3 up = cross(forward, right);
    const Vec3 dir = normalize(forward + right * (nx * tanHalf * aspect) + up * (ny * tanHalf));

    u32 best = 0;
    f32 bestT = 1e30f;
    for (const Entity& e : m_scene.entities) {
        if (isLocked(e) || isHidden(e)) continue;
        const scene::Transform w = scene::worldTransform(m_scene, e);
        if (isArea(e)) { // 영역은 모서리를 눌러야 고른다 (안을 누르면 안의 물체)
            Vec3 centers[12], halves[12];
            areaEdges(w.scale * 0.5f, kAreaEdgePick, centers, halves);
            for (int i = 0; i < 12; ++i) {
                const f32 t = rayBox(m_cameraPos, dir, w, centers[i], halves[i]);
                if (t >= 0.0f && t < bestT) {
                    bestT = t;
                    best = e.id;
                }
            }
            continue;
        }
        Vec3 center{}, half = (isMarker(e) ? kSpawnSize : w.scale) * 0.5f;
        if (const asset::Model* model = e.model.empty() ? nullptr : m_models->get(e.model)) {
            const Vec3 c = (model->boundsMin + model->boundsMax) * 0.5f,
                       h = (model->boundsMax - model->boundsMin) * 0.5f;
            center = {c.x * w.scale.x, c.y * w.scale.y, c.z * w.scale.z};
            half = {std::fabs(h.x * w.scale.x), std::fabs(h.y * w.scale.y), std::fabs(h.z * w.scale.z)};
        }
        const f32 t = rayBox(m_cameraPos, dir, w, center, half);
        if (t >= 0.0f && t < bestT) {
            bestT = t;
            best = e.id;
        }
    }
    if (io.KeyShift) {
        if (best) toggleSelect(best);
    } else {
        m_selected = best;
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
    if (id == m_selected) { // 주 선택을 빼면 다음 것이 주 선택
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

// 주 선택의 월드 변환이 before → after로 바뀐 만큼 나머지 고른 물체(의 최상위)를 같이 옮긴다.
// 회전은 주 선택 위치를 중심으로, 크기는 각자 비율로
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

// ---------------------------------------------------------------- 편집 동작

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

// "hidden 3 5" / "locked 7" 두 줄
void Editor::loadViewState() {
    m_hidden.clear();
    m_locked.clear();
    std::string text;
    const std::string path = viewStatePath();
    if (path.empty() || !readTextFile(path, text)) return;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
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

// 게임 종류의 필드: 텍스트 값을 선언된 타입의 인스턴스로 읽어 위젯으로 고치고, 다시 텍스트로 쓴다
void Editor::drawObjectFields(Entity& e) {
    const reflect::TypeInfo* type = entityType(e);
    if (type && type->fields.empty()) return;
    ImGui::SeparatorText(tr("Fields"));
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
    const FieldContext context{&m_tablesPanel.index()};
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
    // assets 폴더 안이면 상대 경로로 저장한다 (다른 PC에서도 같은 씬 파일이 동작하도록)
    const std::string relative = relativeTo(root, path);
    if (!relative.empty()) return relative;
    setStatus(tr("The file is outside the assets folder. In multiplayer everyone needs it at the same path"));
    return path;
}

void Editor::addModelEntity() {
    const std::string path = chooseModel();
    if (path.empty()) return;
    const asset::Model* model = m_models->get(path);
    if (!model) {
        setStatus(trf("Could not load the model: %s", path.c_str()), true);
        return;
    }
    addEntity(scene::kTypeSolid);
    Entity* e = selected();
    e->model = path;
    e->scale = {1, 1, 1};
    e->color = 0xFFFFFFFF;                       // 재질 색을 그대로 보이게
    e->position.y += -1.0f - model->boundsMin.y; // 바닥에 닿게 (addEntity가 1만큼 띄워 둔다)
    const std::string file = fileName(path);
    e->name = file.substr(0, file.find_last_of('.'));
    setStatus(trf("Model added: %s", path.c_str()));
}

// 선택한 물체와 그 자식들을 같은 구조로 복사한다
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

// 선택한 물체와 그 자식들을 지운다
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
    m_sceneChangedWarned = false;
    m_dirty = false;
    m_autosavePending = false;
    // 지난번에 저장하지 않고 끝났거나 크래시가 났으면 자동 저장본이 더 새롭다
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
    removeFile(m_scenePath + ".autosave"); // 저장했으니 자동 저장본은 필요 없다
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
    m_undo.push_back(std::move(snapshot));
    if (m_undo.size() > kUndoLimit) m_undo.erase(m_undo.begin());
    m_redo.clear();
    m_dirty = true;
}

void Editor::undo() {
    if (m_undo.empty()) return;
    m_redo.push_back(capture());
    restore(m_undo.back());
    m_undo.pop_back();
    setStatus(tr("Undo"));
}

void Editor::redo() {
    if (m_redo.empty()) return;
    m_undo.push_back(capture());
    restore(m_redo.back());
    m_redo.pop_back();
    setStatus(tr("Redo"));
}

void Editor::setStatus(const std::string& text, bool failed, NoticeOpen open) {
    m_status = text;
    m_statusTime = m_time;
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

// ---------------------------------------------------------------- 프로젝트 빌드

void Editor::setProject(const ProjectInfo& project) {
    m_project = project;
    loadViewState(); // 씬은 프로젝트보다 먼저 열린다
    // 창 배치를 프로젝트마다 (build\editor\layout.ini). 처음 여는 프로젝트는 기본 레이아웃
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
    m_live.addTopic("stats"); // 에디터 상태 표시줄의 성능 숫자 (game::FrameStats::json)
    audio::addLiveService(m_live, [this] { return m_audio; }); // MealKit Audio: 에디터에서 재생, 다시 읽기, 상태
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
    // 엔진 저장소 자신(engine 없음)은 묻지 않는다
    const game::ProjectSettings& s = project.settings;
    m_upgradeOffer = !s.engine.empty() && s.engineVersion != kEngineVersion;
    if (m_upgradeOffer)
        MEALKIT_WARN("Project template is from engine %s, this engine is %s - tools\\upgrade_project.ps1 can update it",
                     s.engineVersion.empty() ? "(unknown)" : s.engineVersion.c_str(), kEngineVersion);
    startLayoutCheck();
}

// 폴더 구조 검사 (docs/CODING_MODEL.md "프로젝트 폴더 구조"): 규칙은 엔진의 tools\check_layout.ps1 한 곳에 있고, check.ps1·Export와 같은 결과다
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
    if (loaded && m_gameLoaded && text != m_gameStatus) setStatus(text); // 핫 리로드
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

// 출시본은 디스크의 assets와 mealkit.project를 복사하므로 저장 안 된 씬·프로젝트 설정을 먼저 저장한다
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
        // 업그레이드 도구가 mealkit.project의 engine·engineVersion을 고쳤다: 도구가 관리하는 값만 다시 읽는다
        ProjectInfo upgraded;
        if (loadProject(m_project.directory, upgraded)) {
            m_project.settings.engine = upgraded.settings.engine;
            m_project.settings.engineVersion = upgraded.settings.engineVersion;
            m_projectPanel.setToolValues(upgraded.settings);
        }
        MEALKIT_INFO("Project upgraded to engine %s", kEngineVersion);
        startBuild(BuildKind::GameCode); // 템플릿(CMakeLists 등)이 바뀌었으니 다시 빌드
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
    for (std::string status : {m_tuningPanel.takeStatus(), m_projectPanel.takeStatus()})
        if (!status.empty()) setStatus(status);
    if (std::string status = m_buildPanel.takeStatus(); !status.empty())
        setStatus(status, !m_buildPanel.running() && !m_buildPanel.succeeded(), NoticeOpen::BuildLog);
}

// 뷰포트 오른쪽 아래 알림: 성공은 4초 뒤 사라지고, 실패는 닫을 때까지 남는다. 빌드 중에는 진행 표시
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
            // 끝을 모르는 진행 막대 (음수 = 왕복 애니메이션)
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
