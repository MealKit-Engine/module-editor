#include "mealkit/editor/Editor.h"

#include "mealkit/core/Log.h"
#include "mealkit/core/Profile.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Window.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>

#include <windows.h>
#include <commdlg.h>

namespace mealkit::editor {
namespace {

using scene::Entity;

constexpr f32 kFovY = 60.0f * 3.14159265f / 180.0f;
constexpr const char* kHierarchyWindow = "씬 트리###Hierarchy";
constexpr const char* kInspectorWindow = "인스펙터###Inspector";
constexpr const char* kEnvironmentWindow = "환경###Environment";
constexpr const char* kTuningWindow = "튜닝###Tuning";
constexpr const char* kStateWindow = "상태###State";
constexpr const char* kBuildLogWindow = "빌드 로그###BuildLog";
constexpr size_t kUndoLimit = 100;
const Vec3 kSpawnSize{0.8f, 1.8f, 0.8f};

constexpr const char* kEntityPayload = "MEALKIT_ENTITY";

// 행 벡터 규약 TRS 행렬 (ImGuizmo와 같은 메모리 배치: 이동이 [12..14])
void compose(const scene::Transform& e, f32 out[16]) {
    const Vec3 x = rotate(e.rotation, {1, 0, 0}) * e.scale.x;
    const Vec3 y = rotate(e.rotation, {0, 1, 0}) * e.scale.y;
    const Vec3 z = rotate(e.rotation, {0, 0, 1}) * e.scale.z;
    const f32 m[16] = {x.x, x.y, x.z, 0, y.x, y.y, y.z, 0, z.x, z.y, z.z, 0, e.position.x, e.position.y, e.position.z, 1};
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

std::wstring widen(const std::string& s) {
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(len > 0 ? len - 1 : 0, L'\0');
    if (len > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}

std::string narrow(const wchar_t* w) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}

constexpr const wchar_t* kSceneFilter = L"MealKit 씬 (*.scene)\0*.scene\0모든 파일 (*.*)\0*.*\0";
constexpr const wchar_t* kModelFilter = L"glTF 모델 (*.glb, *.gltf)\0*.glb;*.gltf\0모든 파일 (*.*)\0*.*\0";

std::string fileDialog(void* owner, bool save, const std::string& current, const wchar_t* filter = kSceneFilter,
                       const wchar_t* defaultExt = L"scene", const std::string& folder = {}) {
    wchar_t buffer[MAX_PATH] = {};
    const std::wstring initial = widen(current);
    const std::wstring initialDir = widen(folder);
    wcsncpy_s(buffer, initial.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(owner);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = defaultExt;
    ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
    ofn.Flags = OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    return ok ? narrow(buffer) : std::string();
}

bool toolButton(const char* label, bool active) {
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor();
    return pressed;
}

} // namespace

bool Editor::init(Window& window, render::Renderer& renderer, const std::string& scenePath, asset::ModelCache& models) {
    m_window = &window;
    m_renderer = &renderer;
    m_models = &models;
    m_focusPanel = kInspectorWindow; // 편집 모드로 시작: 인스펙터를 앞으로
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
        if (scene::save(m_scene, m_scenePath)) setStatus("기본 씬을 만들었습니다: " + fileName(m_scenePath));
    }
    return true;
}

void Editor::shutdown() { m_imgui.shutdown(); }

void Editor::update(f64 dt) {
    MEALKIT_ZONE("Editor UI");
    m_time += dt;
    if (dt > 0.0) m_fps = m_fps == 0.0 ? 1.0 / dt : m_fps * 0.95 + (1.0 / dt) * 0.05;

    m_imgui.newFrame();
    ImGuizmo::BeginFrame();
    if (!m_editActive) m_pending = capture();
    m_changed = false;

    drawMenuBar();
    drawStatusBar();

    const ImGuiID dockId = ImGui::GetID("MealKitDockSpace");
    if (m_layoutReset || !ImGui::DockBuilderGetNode(dockId)) {
        setupDefaultLayout(dockId);
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
    drawEnvironment(editing);
    drawTuning();
    drawState();
    pollBuild();
    drawBuildLog();

    if (editing) {
        drawGizmo();
        handlePicking();
        handleShortcuts();
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        m_stopRequest = true;
    }

    // 되돌리기: 편집이 시작된 순간의 상태(프레임 시작 시 캡처)를 한 번만 쌓는다. 드래그 중에는 쌓지 않는다.
    if (m_changed && !m_editActive) {
        pushUndo(std::move(m_pending));
        m_editActive = true;
    }
    if (m_editActive && !ImGui::IsAnyItemActive() && !ImGuizmo::IsUsing()) m_editActive = false;

    updateTitle();
}

void Editor::renderUi(bool frameActive) { m_imgui.render(*m_renderer, frameActive); }

void Editor::buildSceneView(render::FrameParams& frame, render::DrawList& out) const {
    frame.viewport = m_viewport;
    frame.viewProj = m_view * m_proj;
    frame.cameraPos = m_cameraPos;
    const scene::Environment& env = m_scene.environment;
    frame.sunDirection = env.sunDirection;
    frame.sunColor = env.sunColor;
    frame.skyColor = env.skyColor;
    frame.groundColor = env.groundColor;
    frame.fogColor = env.fogColor;
    frame.fogDensity = env.fogDensity;

    const render::MeshId box = m_models->boxMesh();
    for (const Entity& e : m_scene.entities) {
        const scene::Transform world = scene::worldTransform(m_scene, e);
        render::InstanceData inst;
        inst.position = world.position;
        inst.rotation = world.rotation;
        inst.scale = isMarker(e) ? kSpawnSize : world.scale;
        inst.color = e.color;
        inst.flags = e.flags;
        if (e.id == m_selected) inst.flags |= render::kInstanceHighlight;
        const asset::Model* model = e.model.empty() ? nullptr : m_models->get(e.model);
        out.add(model ? m_models->pickLod(*model, m_cameraPos, world.position, world.scale) : box, inst);
    }
}

void Editor::setMode(Mode mode) {
    m_mode = mode;
    m_looking = false;
    m_window->setCursorCaptured(false);
    if (mode == Mode::Edit) m_playStatus.clear();
    m_focusPanel = mode == Mode::Play ? kStateWindow : kInspectorWindow; // 플레이 중에는 [상태] 탭을 앞으로
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

void Editor::selectByIndex(int index) {
    if (index >= 0 && static_cast<size_t>(index) < m_scene.entities.size())
        m_selected = m_scene.entities[static_cast<size_t>(index)].id;
}

// ---------------------------------------------------------------- UI

void Editor::setupDefaultLayout(u32 dockId) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, viewport->WorkSize);
    ImGuiID center = dockId;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.19f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.27f, nullptr, &center);
    const ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.38f, nullptr, &right);
    ImGui::DockBuilderDockWindow(kHierarchyWindow, left);
    ImGui::DockBuilderDockWindow(kInspectorWindow, right);
    ImGui::DockBuilderDockWindow(kEnvironmentWindow, rightBottom);
    ImGui::DockBuilderDockWindow(kTuningWindow, rightBottom);
    ImGui::DockBuilderDockWindow(kStateWindow, right);
    ImGui::DockBuilderFinish(dockId);
}

void Editor::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    const bool editing = m_mode == Mode::Edit;

    if (ImGui::BeginMenu("파일", editing)) {
        if (ImGui::MenuItem("새 씬", "Ctrl+N")) newScene();
        if (ImGui::MenuItem("열기...", "Ctrl+O")) openSceneDialog();
        if (ImGui::MenuItem("저장", "Ctrl+S")) {
            saveScene();
            if (m_tuningDirty) saveTuning();
        }
        if (ImGui::MenuItem("다른 이름으로 저장...")) saveSceneAs();
        ImGui::Separator();
        if (ImGui::MenuItem("종료")) m_quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("편집", editing)) {
        if (ImGui::MenuItem("실행 취소", "Ctrl+Z", false, !m_undo.empty())) undo();
        if (ImGui::MenuItem("다시 실행", "Ctrl+Y", false, !m_redo.empty())) redo();
        ImGui::Separator();
        if (ImGui::MenuItem("복제", "Ctrl+D", false, m_selected != 0)) duplicateSelected();
        if (ImGui::MenuItem("삭제", "Delete", false, m_selected != 0)) deleteSelected();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("추가", editing)) {
        drawTypeMenuItems();
        ImGui::Separator();
        if (ImGui::MenuItem("모델...")) addModelEntity();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("보기")) {
        if (ImGui::MenuItem("선택한 물체로 이동", "F", false, editing && m_selected != 0)) focusSelected();
        if (ImGui::MenuItem("레이아웃 초기화")) m_layoutReset = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("빌드", !m_project.directory.empty())) {
        const bool idle = !m_build.running();
        if (ImGui::MenuItem("게임 코드 빌드 (핫 리로드)", "Ctrl+R", false, idle && editing)) startBuild(BuildKind::GameCode);
        if (ImGui::MenuItem("출시 빌드", "Ctrl+B", false, idle)) startBuild(BuildKind::Export);
        ImGui::Separator();
        if (ImGui::MenuItem("출시 폴더 열기", nullptr, false, directoryExists(m_project.exportDirectory())))
            openInExplorer(m_project.exportDirectory());
        if (ImGui::MenuItem("프로젝트 폴더 열기")) openInExplorer(m_project.directory);
        ImGui::Separator();
        ImGui::MenuItem("빌드 로그", nullptr, &m_buildLogOpen);
        ImGui::EndMenu();
    }

    if (editing) {
        ImGui::TextDisabled("|");
        if (toolButton("이동 W", m_gizmoOp == 0)) m_gizmoOp = 0;
        if (toolButton("회전 E", m_gizmoOp == 1)) m_gizmoOp = 1;
        if (toolButton("크기 R", m_gizmoOp == 2)) m_gizmoOp = 2;
        if (toolButton(m_gizmoLocal ? "로컬 축" : "월드 축", false)) m_gizmoLocal = !m_gizmoLocal;
    }

    const char* playLabel = editing ? "  ▶ 플레이 (F5)  " : "  ■ 정지 (Esc)  ";
    const f32 width = ImGui::CalcTextSize(playLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 8.0f, (ImGui::GetWindowWidth() - width) * 0.5f));
    const ImVec4 color = editing ? ImVec4(0.18f, 0.48f, 0.26f, 1.0f) : ImVec4(0.62f, 0.20f, 0.18f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x * 1.25f, color.y * 1.25f, color.z * 1.25f, 1.0f));
    ImGui::BeginDisabled(editing && !m_gameLoaded); // 게임 DLL이 없으면 플레이할 수 없다
    if (ImGui::Button(playLabel)) {
        if (editing) m_playRequest = true;
        else m_stopRequest = true;
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
    if (editing && !m_gameLoaded && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", m_gameStatus.c_str());
    ImGui::EndMainMenuBar();
}

void Editor::drawStatusBar() {
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, ImGui::GetFrameHeight(), flags)) {
        if (ImGui::BeginMenuBar()) {
            const std::string name = m_scenePath.empty() ? "제목 없음" : fileName(m_scenePath);
            ImGui::Text("%s%s", name.c_str(), m_dirty ? " *" : "");
            ImGui::TextDisabled("|");
            ImGui::Text("물체 %zu개", m_scene.entities.size());
            ImGui::TextDisabled("|");
            if (m_build.running()) {
                ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.35f, 1.0f), "%s 중... %.0f초",
                                   m_buildKind == BuildKind::GameCode ? "게임 코드 빌드" : "출시 빌드", m_time - m_buildStart);
                ImGui::TextDisabled("|");
            } else if (!m_gameStatus.empty()) {
                if (m_gameLoaded) ImGui::TextDisabled("%s", m_gameStatus.c_str());
                else ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "%s", m_gameStatus.c_str());
                ImGui::TextDisabled("|");
            }
            if (m_mode == Mode::Play) ImGui::TextUnformatted(m_playStatus.c_str());
            else if (m_time - m_statusTime < 4.0) ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s", m_status.c_str());
            else ImGui::TextDisabled("카메라: 오른쪽 드래그 + WASD/QE, 휠로 속도 | 선택: 클릭 | Ctrl 누르고 기즈모 = 스냅");
            const char* fps = "0000 fps";
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::CalcTextSize(fps).x - 16.0f);
            ImGui::TextDisabled("%.0f fps", m_fps);
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

void Editor::drawHierarchy(bool editing) {
    if (!ImGui::Begin(kHierarchyWindow)) {
        ImGui::End();
        return;
    }
    ImGui::BeginDisabled(!editing);
    if (ImGui::Button("+ 상자")) addEntity(scene::kTypeSolid);
    ImGui::SameLine();
    if (ImGui::Button("+ 모델")) addModelEntity();
    ImGui::SameLine();
    if (ImGui::Button("+ 스폰")) addEntity(scene::kTypeSpawn);
    ImGui::SameLine();
    if (ImGui::Button("+ 종류...")) ImGui::OpenPopup("##addType");
    if (ImGui::BeginPopup("##addType")) {
        drawTypeMenuItems();
        ImGui::EndPopup();
    }
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
        for (size_t i : roots) drawEntityNode(i, children);
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
    const f32 swatch = ImGui::GetTextLineHeight() * 0.8f;
    f32 rgb[3];
    unpackColor(e.color, rgb);
    std::string label = e.name.empty() ? "(이름 없음)" : e.name;
    if (!e.is(scene::kTypeSolid)) {
        const reflect::TypeInfo* type = entityType(e);
        label += std::string("  · ") + (type ? type->label : e.type.c_str());
    }
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
    if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (m_selected == id) flags |= ImGuiTreeNodeFlags_Selected;
    // 라벨 앞에 색 견본 자리를 비워 두고 그리기 목록에 직접 그린다
    const bool open = ImGui::TreeNodeEx("##node", flags, "      %s", label.c_str());
    {
        const ImVec2 min = ImGui::GetItemRectMin();
        const f32 x = min.x + ImGui::GetTreeNodeToLabelSpacing();
        const f32 y = min.y + (ImGui::GetItemRectSize().y - swatch) * 0.5f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x, y), ImVec2(x + swatch, y + swatch),
                                                  ImGui::ColorConvertFloat4ToU32(ImVec4(rgb[0], rgb[1], rgb[2], 1.0f)), 2.0f);
    }
    // 클릭 선택, 더블클릭 이동, 끌어서 부모 바꾸기
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        m_selected = id;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) focusSelected();
    }
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kEntityPayload, &id, sizeof(id));
        ImGui::Text("%s → (놓을 물체의 자식으로)", label.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload))
            m_treeAction = {TreeAction::Reparent, *static_cast<const u32*>(payload->Data), id};
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem()) {
        m_selected = id;
        if (ImGui::MenuItem("복제", "Ctrl+D")) m_treeAction = {TreeAction::Duplicate, id, 0};
        if (ImGui::MenuItem("삭제", "Delete")) m_treeAction = {TreeAction::Delete, id, 0};
        if (ImGui::MenuItem("최상위로 꺼내기", nullptr, false, e.parent != 0)) m_treeAction = {TreeAction::Reparent, id, 0};
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
        setStatus("자기 자신이나 자식 아래로는 옮길 수 없습니다");
        return;
    }
    pushUndo(capture());
    const scene::Transform world = scene::worldTransform(m_scene, *e); // 화면에서 제자리를 지킨다
    e->parent = parentId;
    scene::setWorldTransform(m_scene, *e, world);
    const Entity* parent = parentId ? m_scene.find(parentId) : nullptr;
    setStatus(parent ? e->name + " → " + parent->name + "의 자식" : e->name + " → 최상위");
}

void Editor::drawInspector(bool editing) {
    if (m_focusPanel == kInspectorWindow) {
        ImGui::SetNextWindowFocus();
        m_focusPanel = nullptr;
    }
    if (!ImGui::Begin(kInspectorWindow)) {
        ImGui::End();
        return;
    }
    ImGui::BeginDisabled(!editing);
    Entity* e = selected();
    if (!e) {
        ImGui::TextDisabled("선택한 물체가 없습니다.");
        ImGui::TextDisabled("뷰포트를 클릭하거나 씬 트리에서 고르세요.");
    } else {
        ImGui::PushItemWidth(-ImGui::CalcTextSize("회전 (도)").x - 12.0f);
        if (m_nameBufferId != e->id) {
            strncpy_s(m_nameBuffer, e->name.c_str(), _TRUNCATE);
            m_nameBufferId = e->id;
        }
        if (ImGui::InputText("이름", m_nameBuffer, sizeof(m_nameBuffer))) {
            e->name = m_nameBuffer;
            markChanged();
        }
        const reflect::TypeInfo* type = entityType(*e);
        const std::string typeLabel = type ? type->label : e->type + " (게임 코드에 없음)";
        if (ImGui::BeginCombo("종류", typeLabel.c_str())) {
            for (const reflect::TypeInfo* t : allTypes()) {
                if (ImGui::Selectable(t->label, t == type) && t != type) {
                    e->type = t->name;
                    e->fields = scene::defaultFields(*t);
                    markChanged();
                }
            }
            ImGui::EndCombo();
        }
        drawObjectFields(*e);

        if (!isMarker(*e)) {
            ImGui::SeparatorText("모델");
            if (e->model.empty()) {
                ImGui::TextDisabled("기본 상자");
            } else {
                ImGui::TextUnformatted(e->model.c_str());
                if (const asset::Model* model = m_models->get(e->model))
                    ImGui::TextDisabled("삼각형 %u개 · LOD %u단계", model->triangles[0], model->lodCount);
                else ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "불러오지 못했습니다 (로그 확인)");
            }
            if (ImGui::Button("모델 선택...")) {
                const std::string path = chooseModel();
                if (!path.empty()) {
                    e->model = path;
                    markChanged();
                }
            }
            if (!e->model.empty()) {
                ImGui::SameLine();
                if (ImGui::Button("상자로 되돌리기")) {
                    e->model.clear();
                    markChanged();
                }
            }
        }

        ImGui::SeparatorText(e->parent ? "변환 (부모 기준)" : "변환");
        f32 matrix[16], t[3], r[3], s[3];
        compose(scene::localTransform(*e), matrix);
        ImGuizmo::DecomposeMatrixToComponents(matrix, t, r, s);
        bool changed = ImGui::DragFloat3("위치", t, 0.05f, 0.0f, 0.0f, "%.2f");
        changed |= ImGui::DragFloat3("회전 (도)", r, 0.5f, 0.0f, 0.0f, "%.1f");
        if (!isMarker(*e)) changed |= ImGui::DragFloat3("크기", s, 0.02f, 0.01f, 10000.0f, "%.2f");
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

        ImGui::SeparatorText("모양");
        f32 rgb[3];
        unpackColor(e->color, rgb);
        if (ImGui::ColorEdit3("색상", rgb)) {
            e->color = packColor(rgb, e->color);
            markChanged();
        }
        bool checker = (e->flags & render::kInstanceChecker) != 0;
        if (ImGui::Checkbox("체커 무늬", &checker)) {
            e->flags = checker ? (e->flags | render::kInstanceChecker) : (e->flags & ~render::kInstanceChecker);
            markChanged();
        }
        ImGui::PopItemWidth();
        ImGui::TextDisabled("ID %u", e->id);

        ImGui::Spacing();
        if (ImGui::Button("복제 (Ctrl+D)")) duplicateSelected();
        ImGui::SameLine();
        if (ImGui::Button("삭제 (Del)")) deleteSelected(); // 이후 e를 쓰지 않는다
    }
    ImGui::EndDisabled();
    ImGui::End();
}

void Editor::drawEnvironment(bool editing) {
    if (!ImGui::Begin(kEnvironmentWindow)) {
        ImGui::End();
        return;
    }
    ImGui::BeginDisabled(!editing);
    scene::Environment& env = m_scene.environment;
    const ImGuiColorEditFlags hdr = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR;
    bool c = false;
    ImGui::PushItemWidth(-ImGui::CalcTextSize("지면 반사광").x - 12.0f);
    c |= ImGui::DragFloat3("태양 방향", &env.sunDirection.x, 0.01f, -1.0f, 1.0f, "%.2f");
    c |= ImGui::ColorEdit3("태양 빛", &env.sunColor.x, hdr);
    c |= ImGui::ColorEdit3("하늘 환경광", &env.skyColor.x, hdr);
    c |= ImGui::ColorEdit3("지면 반사광", &env.groundColor.x, hdr);
    c |= ImGui::ColorEdit3("안개·배경", &env.fogColor.x, hdr);
    c |= ImGui::DragFloat("안개 밀도", &env.fogDensity, 0.0002f, 0.0f, 0.1f, "%.4f");
    ImGui::PopItemWidth();
    if (c) markChanged();
    ImGui::TextDisabled("색은 선형(linear) 값입니다.");
    ImGui::EndDisabled();
    ImGui::End();
}

// ---------------------------------------------------------------- 튜닝 (assets/game.tuning)

std::string Editor::tuningPath() const {
    return m_project.directory.empty() ? std::string() : m_project.directory + "\\assets\\game.tuning";
}

void Editor::setTuningType(const reflect::TypeInfo* type) {
    if (m_tuning.type()) { // 지금 값을 텍스트로 보존
        m_tuningText.clear();
        reflect::toText(*m_tuning.type(), m_tuning.data(), m_tuningText);
    }
    m_tuning = type ? reflect::Instance(*type) : reflect::Instance();
    if (!type) return;
    if (!m_tuningLoaded) {
        loadTuningFile();
        m_tuningLoaded = true;
    } else {
        std::string unknown;
        reflect::fromText(*type, m_tuning.data(), m_tuningText, &unknown);
        if (!unknown.empty()) MEALKIT_WARN("Tuning fields no longer declared: %s", unknown.c_str());
    }
}

bool Editor::loadTuningFile() {
    if (!m_tuning.type()) return false;
    m_tuning = reflect::Instance(*m_tuning.type()); // 기본값부터
    const std::string path = tuningPath();
    m_tuningDirty = false;
    m_tuningChanged = true;
    if (path.empty() || !fileExists(path)) return false;
    std::string error;
    if (!reflect::loadTextFile(*m_tuning.type(), m_tuning.data(), path, &error)) {
        setStatus("튜닝 파일을 읽지 못했습니다: " + error);
        return false;
    }
    return true;
}

bool Editor::saveTuning() {
    if (!m_tuning.type() || tuningPath().empty()) return false;
    if (!reflect::saveTextFile(*m_tuning.type(), m_tuning.data(), tuningPath(),
                               (std::string("MealKit tuning (") + m_tuning.type()->name + "). 에디터 [튜닝] 패널에서 고친다.").c_str())) {
        setStatus("튜닝 저장 실패: " + tuningPath());
        return false;
    }
    m_tuningDirty = false;
    setStatus("튜닝 저장됨: assets\\game.tuning");
    return true;
}

std::string Editor::writePlayTuning() {
    if (!m_tuning.type() || m_project.directory.empty()) return {};
    const std::string dir = m_project.directory + "\\build\\Editor\\play";
    createDirectories(dir);
    const std::string path = dir + "\\play.tuning";
    return reflect::saveTextFile(*m_tuning.type(), m_tuning.data(), path, "editor play (unsaved values)") ? path : std::string();
}

bool Editor::takeTuningChanged() {
    const bool changed = m_tuningChanged;
    m_tuningChanged = false;
    return changed;
}

void Editor::drawTuning() {
    // 예전 레이아웃 파일에는 이 창이 없다: 처음 열 때 [환경] 창과 같은 자리에 탭으로 붙인다
    if (ImGuiWindow* env = ImGui::FindWindowByName(kEnvironmentWindow); env && env->DockId)
        ImGui::SetNextWindowDockID(env->DockId, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(kTuningWindow)) {
        ImGui::End();
        return;
    }
    const reflect::TypeInfo* type = m_tuning.type();
    if (!type) {
        ImGui::TextDisabled("게임 코드를 불러오면 튜닝 값이 나옵니다.");
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("assets\\game.tuning (%s)%s", type->name, m_tuningDirty ? " *" : "");
    ImGui::BeginDisabled(!m_tuningDirty);
    if (ImGui::Button("저장")) saveTuning();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("파일에서 다시 읽기")) loadTuningFile();
    ImGui::SameLine();
    if (ImGui::Button("기본값")) {
        m_tuning = reflect::Instance(*type);
        m_tuningDirty = m_tuningChanged = true;
    }
    if (m_mode == Mode::Play) ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "플레이 중: 바꾸면 바로 적용됩니다");
    ImGui::Separator();
    ImGui::PushItemWidth(-ImGui::GetContentRegionAvail().x * 0.45f);
    if (drawFields(*type, m_tuning.data())) m_tuningDirty = m_tuningChanged = true;
    ImGui::PopItemWidth();
    ImGui::End();
}

// 플레이 중인 시뮬레이션 상태 (상태 필드 선언을 그대로 보여 준다)
void Editor::drawState() {
    if (ImGuiWindow* inspector = ImGui::FindWindowByName(kInspectorWindow); inspector && inspector->DockId)
        ImGui::SetNextWindowDockID(inspector->DockId, ImGuiCond_FirstUseEver);
    if (m_focusPanel == kStateWindow) {
        ImGui::SetNextWindowFocus();
        m_focusPanel = nullptr;
    }
    if (!ImGui::Begin(kStateWindow)) {
        ImGui::End();
        return;
    }
    if (m_mode != Mode::Play || !m_stateView.type || !m_stateView.data) {
        ImGui::TextDisabled("플레이 중에 시뮬레이션 상태(MEALKIT_FIELD로 선언한 값)가 나옵니다.");
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("%s (읽기 전용)", m_stateView.type->name);
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
    const bool snapping = ImGui::GetIO().KeyCtrl;

    if (ImGuizmo::Manipulate(&m_view.m[0][0], &m_proj.m[0][0], op, mode, matrix, nullptr, snapping ? snap : nullptr)) {
        scene::Transform world;
        decompose(matrix, world);
        scene::setWorldTransform(m_scene, *e, world);
        markChanged();
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
        if (io.MouseWheel != 0.0f) m_cameraSpeed = std::clamp(m_cameraSpeed * std::pow(1.25f, io.MouseWheel), 1.0f, 400.0f);
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

void Editor::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || m_looking) return;
    auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, false); };
    if (io.KeyCtrl) {
        if (pressed(ImGuiKey_S)) {
            saveScene();
            if (m_tuningDirty) saveTuning();
        }
        if (pressed(ImGuiKey_Z)) io.KeyShift ? redo() : undo();
        if (pressed(ImGuiKey_Y)) redo();
        if (pressed(ImGuiKey_D)) duplicateSelected();
        if (pressed(ImGuiKey_N)) newScene();
        if (pressed(ImGuiKey_O)) openSceneDialog();
        if (pressed(ImGuiKey_B)) startBuild(BuildKind::Export);
        if (pressed(ImGuiKey_R)) startBuild(BuildKind::GameCode);
        return;
    }
    if (pressed(ImGuiKey_W)) m_gizmoOp = 0;
    if (pressed(ImGuiKey_E)) m_gizmoOp = 1;
    if (pressed(ImGuiKey_R)) m_gizmoOp = 2;
    if (pressed(ImGuiKey_Delete)) deleteSelected();
    if (pressed(ImGuiKey_F)) focusSelected();
    if (pressed(ImGuiKey_F5) && m_gameLoaded) m_playRequest = true;
    if (pressed(ImGuiKey_Escape)) m_selected = 0;
}

void Editor::handlePicking() {
    ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left) || io.WantCaptureMouse || ImGuizmo::IsOver() || ImGuizmo::IsUsing())
        return;
    const ImVec2 mouse = io.MousePos;
    if (mouse.x < m_viewport.x || mouse.y < m_viewport.y || mouse.x >= m_viewport.x + m_viewport.width ||
        mouse.y >= m_viewport.y + m_viewport.height)
        return;

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
        const scene::Transform w = scene::worldTransform(m_scene, e);
        Vec3 center{}, half = (isMarker(e) ? kSpawnSize : w.scale) * 0.5f;
        if (const asset::Model* model = e.model.empty() ? nullptr : m_models->get(e.model)) {
            const Vec3 c = (model->boundsMin + model->boundsMax) * 0.5f, h = (model->boundsMax - model->boundsMin) * 0.5f;
            center = {c.x * w.scale.x, c.y * w.scale.y, c.z * w.scale.z};
            half = {std::fabs(h.x * w.scale.x), std::fabs(h.y * w.scale.y), std::fabs(h.z * w.scale.z)};
        }
        const f32 t = rayBox(m_cameraPos, dir, w, center, half);
        if (t >= 0.0f && t < bestT) {
            bestT = t;
            best = e.id;
        }
    }
    m_selected = best;
}

// ---------------------------------------------------------------- 편집 동작

Entity* Editor::selected() { return m_selected ? m_scene.find(m_selected) : nullptr; }

const reflect::TypeInfo* Editor::entityType(const Entity& e) const { return scene::findType(e.type, m_gameTypes); }

bool Editor::isMarker(const Entity& e) const {
    const reflect::TypeInfo* type = entityType(e);
    return type && (type->flags & reflect::kObjectMarker);
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
        if (i == builtins) ImGui::SeparatorText("게임");
        if (ImGui::MenuItem(types[i]->label)) addEntity(types[i]->name);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", types[i]->name);
    }
    if (!m_gameTypes) ImGui::TextDisabled("(게임 코드를 불러오면 게임 종류가 나옵니다)");
}

// 게임 종류의 필드: 텍스트 값을 선언된 타입의 인스턴스로 읽어 위젯으로 고치고, 다시 텍스트로 쓴다
void Editor::drawObjectFields(Entity& e) {
    const reflect::TypeInfo* type = entityType(e);
    if (type && type->fields.empty()) return;
    ImGui::SeparatorText("필드");
    if (!type) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f), "게임 코드에 '%s' 종류가 없습니다. 값은 그대로 보존됩니다.", e.type.c_str());
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
    if (drawFields(*type, instance.data())) {
        reflect::TextFields fields;
        reflect::toText(*type, instance.data(), fields);
        e.fields = std::move(fields);
        markChanged();
    }
    if (!warnings.empty()) ImGui::TextDisabled("선언에 없는 필드: %s (저장하면 사라집니다)", warnings.c_str());
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
    Entity& e = m_scene.add(type->name, solid ? "상자" : type->label);
    e.name += " " + std::to_string(e.id);
    e.fields = scene::defaultFields(*type);
    if (type->flags & reflect::kObjectMarker) {
        e.position = {pos.x, std::max(pos.y, 0.0f) + 0.9f, pos.z};
        e.color = 0xFF5ADCFF;
    } else {
        e.scale = {2, 2, 2};
        e.position = {pos.x, std::max(pos.y, 0.0f) + 1.0f, pos.z};
        e.color = solid ? 0xFFD2C8C8 : typeName == scene::kTypeDecoration ? 0xFFDCAA78 : 0xFF8CD2F0;
    }
    m_selected = e.id;
    setStatus("추가: " + e.name);
}

std::string Editor::chooseModel() {
    const std::string root = asset::ModelCache::assetsRoot();
    const std::string path = fileDialog(m_window->nativeHandle(), false, "", kModelFilter, L"glb", root + "\\models");
    if (path.empty()) return {};
    // assets 폴더 안이면 상대 경로로 저장한다 (다른 PC에서도 같은 씬 파일이 동작하도록)
    std::string lowerPath = path, lowerRoot = root + "\\";
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    std::transform(lowerRoot.begin(), lowerRoot.end(), lowerRoot.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    if (lowerPath.rfind(lowerRoot, 0) == 0) {
        std::string relative = path.substr(lowerRoot.size());
        std::replace(relative.begin(), relative.end(), '\\', '/');
        return relative;
    }
    setStatus("assets 폴더 밖의 파일입니다. 멀티플레이에서는 모두 같은 경로에 있어야 합니다");
    return path;
}

void Editor::addModelEntity() {
    const std::string path = chooseModel();
    if (path.empty()) return;
    const asset::Model* model = m_models->get(path);
    if (!model) {
        setStatus("모델을 불러오지 못했습니다: " + path);
        return;
    }
    addEntity(scene::kTypeSolid);
    Entity* e = selected();
    e->model = path;
    e->scale = {1, 1, 1};
    e->color = 0xFFFFFFFF; // 재질 색을 그대로 보이게
    e->position.y += -1.0f - model->boundsMin.y; // 바닥에 닿게 (addEntity가 1만큼 띄워 둔다)
    const std::string file = fileName(path);
    e->name = file.substr(0, file.find_last_of('.'));
    setStatus("모델 추가: " + path);
}

// 선택한 물체와 그 자식들을 같은 구조로 복사한다
void Editor::duplicateSelected() {
    const Entity* e = selected();
    if (!e) return;
    pushUndo(capture());
    const std::vector<u32> ids = scene::subtree(m_scene, e->id);
    std::unordered_map<u32, u32> remap;
    for (u32 id : ids) remap[id] = m_scene.nextId++;
    std::vector<Entity> copies;
    for (u32 id : ids) {
        Entity copy = *m_scene.find(id);
        const bool top = id == m_selected;
        copy.id = remap[id];
        copy.parent = top ? copy.parent : remap[copy.parent];
        if (top) {
            copy.name += " 복사";
            copy.position.x += 1.0f;
        }
        copies.push_back(std::move(copy));
    }
    const std::string name = copies.front().name;
    const u32 newId = copies.front().id;
    m_scene.entities.insert(m_scene.entities.end(), copies.begin(), copies.end());
    m_selected = newId;
    setStatus("복제: " + name + (ids.size() > 1 ? " (자식 " + std::to_string(ids.size() - 1) + "개 포함)" : ""));
}

// 선택한 물체와 그 자식들을 지운다
void Editor::deleteSelected() {
    const Entity* e = selected();
    if (!e) return;
    pushUndo(capture());
    const std::vector<u32> ids = scene::subtree(m_scene, e->id);
    setStatus("삭제: " + e->name + (ids.size() > 1 ? " (자식 " + std::to_string(ids.size() - 1) + "개 포함)" : ""));
    std::erase_if(m_scene.entities, [&](const Entity& x) { return std::find(ids.begin(), ids.end(), x.id) != ids.end(); });
    m_selected = 0;
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
    setStatus("새 씬 (Ctrl+Z로 되돌릴 수 있습니다)");
}

void Editor::openSceneDialog() {
    const std::string path = fileDialog(m_window->nativeHandle(), false, m_scenePath);
    if (path.empty()) return;
    saveBackupIfDirty();
    loadScene(path);
}

bool Editor::loadScene(const std::string& path) {
    scene::Scene loaded;
    std::string error;
    if (!scene::load(loaded, path, &error)) {
        MEALKIT_ERROR("Cannot open scene %s: %s", path.c_str(), error.c_str());
        setStatus("열기 실패: " + error);
        return false;
    }
    m_scene = std::move(loaded);
    m_scenePath = path;
    m_dirty = false;
    m_undo.clear();
    m_redo.clear();
    m_selected = 0;
    m_nameBufferId = 0;
    setStatus("열림: " + fileName(path));
    return true;
}

bool Editor::saveScene() {
    if (m_scenePath.empty()) return saveSceneAs();
    if (!scene::save(m_scene, m_scenePath)) {
        setStatus("저장 실패: " + m_scenePath);
        return false;
    }
    m_dirty = false;
    setStatus("저장됨: " + fileName(m_scenePath));
    return true;
}

bool Editor::saveSceneAs() {
    const std::string path = fileDialog(m_window->nativeHandle(), true, m_scenePath);
    if (path.empty()) return false;
    m_scenePath = path;
    return saveScene();
}

Editor::Snapshot Editor::capture() const {
    return {m_scene.entities, m_scene.environment, m_scene.nextId, m_selected};
}

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
    setStatus("실행 취소");
}

void Editor::redo() {
    if (m_redo.empty()) return;
    m_undo.push_back(capture());
    restore(m_redo.back());
    m_redo.pop_back();
    setStatus("다시 실행");
}

void Editor::setStatus(const std::string& text) {
    m_status = text;
    m_statusTime = m_time;
}

void Editor::updateTitle() {
    std::string title = "MealKit Editor - ";
    if (!m_project.name.empty()) title += m_project.name + " - ";
    title += m_scenePath.empty() ? std::string("제목 없음") : fileName(m_scenePath);
    if (m_dirty) title += " *";
    if (m_tuningDirty) title += "  (튜닝 *)";
    if (m_mode == Mode::Play) title += "  [플레이 중]";
    if (title != m_title) {
        m_window->setTitle(title.c_str());
        m_title = title;
    }
}

// ---------------------------------------------------------------- 프로젝트 빌드

void Editor::setProject(const ProjectInfo& project) { m_project = project; }

void Editor::setGameCodeStatus(bool loaded, const std::string& text) {
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

// 프로젝트의 build.bat을 창 없이 실행한다.
//   GameCode: build.bat Editor → build\Editor\bin\<이름>_game.dll (엔진 mealkit.dll은 이미 빌드되어 있으면 건드리지 않는다)
//   Export:   build.bat Export → 출시용 exe만 다시 빌드하고 export\<이름>에 모은다
void Editor::startBuild(BuildKind kind) {
    if (m_build.running() || m_project.directory.empty()) return;
    // 출시본은 디스크의 assets를 복사하므로 저장 안 된 씬을 먼저 저장한다
    if (kind == BuildKind::Export && m_dirty && !m_scenePath.empty() && !saveScene()) return;

    const std::string script = m_project.directory + "\\build.bat";
    if (!fileExists(script)) {
        setStatus("build.bat이 없습니다: " + script);
        return;
    }
    const char* arg = kind == BuildKind::GameCode ? "Editor" : "Export";
    m_buildKind = kind;
    m_buildLog = std::string("> build.bat ") + arg + "\n";
    m_buildLogScroll = true;
    if (kind == BuildKind::Export) m_buildLogOpen = true; // 게임 코드 빌드는 실패할 때만 연다
    if (!m_build.start("\"" + script + "\" " + arg, m_project.directory)) {
        m_buildLog += "빌드를 시작하지 못했습니다.\n";
        m_buildLogOpen = true;
        return;
    }
    m_buildStart = m_time;
    MEALKIT_INFO("Build started: %s %s", script.c_str(), arg);
}

void Editor::pollBuild() {
    if (!m_build.running()) return;
    // 실패한 명령은 ninja가 /showIncludes 줄까지 그대로 보여 준다. 에러를 찾기 쉽게 그 줄은 뺀다 (영문/한국어 MSVC).
    std::string fresh;
    const bool running = m_build.poll(fresh);
    size_t start = 0;
    for (size_t nl = fresh.find('\n'); nl != std::string::npos; nl = fresh.find('\n', start)) {
        const std::string line = fresh.substr(start, nl + 1 - start);
        start = nl + 1;
        if (line.find("Note: including file:") != std::string::npos || line.find("참고: 포함 파일:") != std::string::npos)
            continue;
        m_buildLog += line;
        m_buildLogScroll = true;
    }
    if (running) return;
    m_buildLogScroll = true;
    const std::string seconds = std::to_string(static_cast<int>(m_time - m_buildStart));
    if (m_build.exitCode() != 0) {
        m_buildLog += "\n실패 (종료 코드 " + std::to_string(m_build.exitCode()) + ")\n";
        m_buildLogOpen = true;
        setStatus(m_buildKind == BuildKind::GameCode ? "게임 코드 빌드 실패 - 빌드 로그를 확인하세요"
                                                     : "출시 빌드 실패 - 빌드 로그를 확인하세요");
        if (m_buildKind == BuildKind::GameCode) {
            m_gameCodeFailed = true;
            if (!m_gameLoaded) m_gameStatus = "게임 코드 빌드 실패 - 고친 뒤 Ctrl+R";
        }
        MEALKIT_ERROR("Build failed with exit code %d", m_build.exitCode());
    } else if (m_buildKind == BuildKind::GameCode) {
        m_buildLog += "\n완료 (" + seconds + "초): 게임 코드\n";
        m_gameCodeBuilt = true;
        MEALKIT_INFO("Game code build finished");
    } else {
        m_buildLog += "\n완료 (" + seconds + "초): " + m_project.exportDirectory() + "\n";
        setStatus("출시 빌드 완료: export\\" + m_project.name);
        MEALKIT_INFO("Export finished: %s", m_project.exportDirectory().c_str());
    }
}

void Editor::drawBuildLog() {
    if (!m_buildLogOpen) return;
    ImGui::SetNextWindowSize(ImVec2(720, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(kBuildLogWindow, &m_buildLogOpen)) {
        ImGui::End();
        return;
    }
    const bool editing = m_mode == Mode::Edit;
    ImGui::BeginDisabled(m_build.running() || m_project.directory.empty() || !editing);
    if (ImGui::Button("게임 코드 빌드")) startBuild(BuildKind::GameCode);
    ImGui::SameLine();
    if (ImGui::Button("출시 빌드")) startBuild(BuildKind::Export);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!directoryExists(m_project.exportDirectory()));
    if (ImGui::Button("출시 폴더 열기")) openInExplorer(m_project.exportDirectory());
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (m_build.running()) ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.35f, 1.0f), "빌드 중... %.0f초", m_time - m_buildStart);
    ImGui::Separator();
    if (ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::TextUnformatted(m_buildLog.data(), m_buildLog.data() + m_buildLog.size());
        if (m_buildLogScroll) {
            ImGui::SetScrollHereY(1.0f);
            m_buildLogScroll = false;
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace mealkit::editor
