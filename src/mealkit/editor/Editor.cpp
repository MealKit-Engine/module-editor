#include "mealkit/editor/Editor.h"

#include "mealkit/core/Log.h"
#include "mealkit/core/Profile.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Window.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>

#include <windows.h>
#include <commdlg.h>

namespace mealkit::editor {
namespace {

using scene::Entity;
using scene::EntityKind;

constexpr f32 kFovY = 60.0f * 3.14159265f / 180.0f;
constexpr const char* kHierarchyWindow = "씬 트리###Hierarchy";
constexpr const char* kInspectorWindow = "인스펙터###Inspector";
constexpr const char* kEnvironmentWindow = "환경###Environment";
constexpr const char* kBuildLogWindow = "빌드 로그###BuildLog";
constexpr size_t kUndoLimit = 100;
const Vec3 kSpawnSize{0.8f, 1.8f, 0.8f};

// 행 벡터 규약 TRS 행렬 (ImGuizmo와 같은 메모리 배치: 이동이 [12..14])
void compose(const Entity& e, f32 out[16]) {
    const Vec3 x = rotate(e.rotation, {1, 0, 0}) * e.scale.x;
    const Vec3 y = rotate(e.rotation, {0, 1, 0}) * e.scale.y;
    const Vec3 z = rotate(e.rotation, {0, 0, 1}) * e.scale.z;
    const f32 m[16] = {x.x, x.y, x.z, 0, y.x, y.y, y.z, 0, z.x, z.y, z.z, 0, e.position.x, e.position.y, e.position.z, 1};
    std::memcpy(out, m, sizeof(m));
}

void decompose(const f32 m[16], Entity& e) {
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
f32 rayBox(Vec3 origin, Vec3 dir, const Entity& e, Vec3 center, Vec3 half) {
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
        render::InstanceData inst;
        inst.position = e.position;
        inst.rotation = e.rotation;
        inst.scale = e.kind == EntityKind::SpawnPoint ? kSpawnSize : e.scale;
        inst.color = e.color;
        inst.flags = e.flags;
        if (e.id == m_selected) inst.flags |= render::kInstanceHighlight;
        const asset::Model* model = e.model.empty() ? nullptr : m_models->get(e.model);
        out.add(model ? m_models->pickLod(*model, m_cameraPos, e.position, e.scale) : box, inst);
    }
}

void Editor::setMode(Mode mode) {
    m_mode = mode;
    m_looking = false;
    m_window->setCursorCaptured(false);
    if (mode == Mode::Edit) m_playStatus.clear();
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
    ImGui::DockBuilderFinish(dockId);
}

void Editor::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    const bool editing = m_mode == Mode::Edit;

    if (ImGui::BeginMenu("파일", editing)) {
        if (ImGui::MenuItem("새 씬", "Ctrl+N")) newScene();
        if (ImGui::MenuItem("열기...", "Ctrl+O")) openSceneDialog();
        if (ImGui::MenuItem("저장", "Ctrl+S")) saveScene();
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
        if (ImGui::MenuItem("충돌 상자")) addEntity(EntityKind::Solid);
        if (ImGui::MenuItem("장식 상자 (충돌 없음)")) addEntity(EntityKind::Decoration);
        if (ImGui::MenuItem("스폰 지점")) addEntity(EntityKind::SpawnPoint);
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
    if (ImGui::Button("+ 상자")) addEntity(EntityKind::Solid);
    ImGui::SameLine();
    if (ImGui::Button("+ 모델")) addModelEntity();
    ImGui::SameLine();
    if (ImGui::Button("+ 장식")) addEntity(EntityKind::Decoration);
    ImGui::SameLine();
    if (ImGui::Button("+ 스폰")) addEntity(EntityKind::SpawnPoint);
    ImGui::Separator();

    if (ImGui::BeginChild("##entities")) {
        const f32 swatch = ImGui::GetTextLineHeight() * 0.8f;
        for (size_t i = 0; i < m_scene.entities.size(); ++i) {
            const Entity& e = m_scene.entities[i];
            ImGui::PushID(static_cast<int>(e.id));
            f32 rgb[3];
            unpackColor(e.color, rgb);
            ImGui::ColorButton("##swatch", ImVec4(rgb[0], rgb[1], rgb[2], 1.0f),
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(swatch, swatch));
            ImGui::SameLine();
            std::string label = e.name.empty() ? "(이름 없음)" : e.name;
            if (e.kind == EntityKind::Decoration) label += "  · 장식";
            else if (e.kind == EntityKind::SpawnPoint) label += "  · 스폰";
            const u32 id = e.id;
            if (ImGui::Selectable(label.c_str(), m_selected == id, ImGuiSelectableFlags_AllowDoubleClick)) {
                m_selected = id;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) focusSelected();
            }
            bool structural = false;
            if (ImGui::BeginPopupContextItem()) {
                m_selected = id;
                if (ImGui::MenuItem("복제", "Ctrl+D")) {
                    duplicateSelected();
                    structural = true;
                }
                if (ImGui::MenuItem("삭제", "Delete")) {
                    deleteSelected();
                    structural = true;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
            if (structural) break; // 목록이 바뀌었으므로 다음 프레임에 다시 그린다
        }
    }
    ImGui::EndChild();
    ImGui::EndDisabled();
    ImGui::End();
}

void Editor::drawInspector(bool editing) {
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
        int kind = static_cast<int>(e->kind);
        const char* kinds[] = {"충돌 상자", "장식 (충돌 없음)", "스폰 지점"};
        if (ImGui::Combo("종류", &kind, kinds, 3)) {
            e->kind = static_cast<EntityKind>(kind);
            markChanged();
        }

        if (e->kind != EntityKind::SpawnPoint) {
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

        ImGui::SeparatorText("변환");
        f32 matrix[16], t[3], r[3], s[3];
        compose(*e, matrix);
        ImGuizmo::DecomposeMatrixToComponents(matrix, t, r, s);
        bool changed = ImGui::DragFloat3("위치", t, 0.05f, 0.0f, 0.0f, "%.2f");
        changed |= ImGui::DragFloat3("회전 (도)", r, 0.5f, 0.0f, 0.0f, "%.1f");
        if (e->kind != EntityKind::SpawnPoint) changed |= ImGui::DragFloat3("크기", s, 0.02f, 0.01f, 10000.0f, "%.2f");
        if (changed) {
            for (f32& v : s) v = std::max(v, 0.01f);
            ImGuizmo::RecomposeMatrixFromComponents(t, r, s, matrix);
            decompose(matrix, *e);
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

void Editor::drawGizmo() {
    Entity* e = selected();
    if (!e || m_viewport.width < 1.0f || m_viewport.height < 1.0f) return;

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetRect(m_viewport.x, m_viewport.y, m_viewport.width, m_viewport.height);
    f32 matrix[16];
    compose(*e, matrix);

    const ImGuizmo::OPERATION ops[3] = {ImGuizmo::TRANSLATE, ImGuizmo::ROTATE, ImGuizmo::SCALE};
    ImGuizmo::OPERATION op = ops[m_gizmoOp];
    if (e->kind == EntityKind::SpawnPoint && op == ImGuizmo::SCALE) op = ImGuizmo::TRANSLATE;
    const ImGuizmo::MODE mode = (m_gizmoLocal || op == ImGuizmo::SCALE) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    const f32 step = op == ImGuizmo::TRANSLATE ? 0.5f : op == ImGuizmo::ROTATE ? 15.0f : 0.1f;
    const f32 snap[3] = {step, step, step};
    const bool snapping = ImGui::GetIO().KeyCtrl;

    if (ImGuizmo::Manipulate(&m_view.m[0][0], &m_proj.m[0][0], op, mode, matrix, nullptr, snapping ? snap : nullptr)) {
        decompose(matrix, *e);
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
        if (pressed(ImGuiKey_S)) saveScene();
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
        Vec3 center{}, half = (e.kind == EntityKind::SpawnPoint ? kSpawnSize : e.scale) * 0.5f;
        if (const asset::Model* model = e.model.empty() ? nullptr : m_models->get(e.model)) {
            const Vec3 c = (model->boundsMin + model->boundsMax) * 0.5f, h = (model->boundsMax - model->boundsMin) * 0.5f;
            center = {c.x * e.scale.x, c.y * e.scale.y, c.z * e.scale.z};
            half = {std::fabs(h.x * e.scale.x), std::fabs(h.y * e.scale.y), std::fabs(h.z * e.scale.z)};
        }
        const f32 t = rayBox(m_cameraPos, dir, e, center, half);
        if (t >= 0.0f && t < bestT) {
            bestT = t;
            best = e.id;
        }
    }
    m_selected = best;
}

// ---------------------------------------------------------------- 편집 동작

Entity* Editor::selected() { return m_selected ? m_scene.find(m_selected) : nullptr; }

void Editor::addEntity(EntityKind kind) {
    pushUndo(capture());
    const Vec3 forward = cameraForward();
    Vec3 pos = m_cameraPos + forward * 12.0f;
    if (forward.y < -0.05f) {
        const f32 t = -m_cameraPos.y / forward.y;
        if (t > 0.0f && t < 80.0f) pos = m_cameraPos + forward * t;
    }
    const char* base = kind == EntityKind::Solid ? "상자" : kind == EntityKind::Decoration ? "장식" : "스폰 지점";
    Entity& e = m_scene.add(kind, base);
    e.name += " " + std::to_string(e.id);
    switch (kind) {
    case EntityKind::Solid:
        e.scale = {2, 2, 2};
        e.position = {pos.x, std::max(pos.y, 0.0f) + 1.0f, pos.z};
        e.color = 0xFFD2C8C8;
        break;
    case EntityKind::Decoration:
        e.scale = {2, 2, 2};
        e.position = {pos.x, std::max(pos.y, 0.0f) + 1.0f, pos.z};
        e.color = 0xFFDCAA78;
        break;
    case EntityKind::SpawnPoint:
        e.position = {pos.x, std::max(pos.y, 0.0f) + 0.9f, pos.z};
        e.color = 0xFF5ADCFF;
        break;
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
    addEntity(EntityKind::Solid);
    Entity* e = selected();
    e->model = path;
    e->scale = {1, 1, 1};
    e->color = 0xFFFFFFFF; // 재질 색을 그대로 보이게
    e->position.y += -1.0f - model->boundsMin.y; // 바닥에 닿게 (addEntity가 1만큼 띄워 둔다)
    const std::string file = fileName(path);
    e->name = file.substr(0, file.find_last_of('.'));
    setStatus("모델 추가: " + path);
}

void Editor::duplicateSelected() {
    const Entity* e = selected();
    if (!e) return;
    pushUndo(capture());
    Entity copy = *e;
    copy.id = m_scene.nextId++;
    copy.name += " 복사";
    copy.position.x += 1.0f;
    m_scene.entities.push_back(copy);
    m_selected = copy.id;
    setStatus("복제: " + copy.name);
}

void Editor::deleteSelected() {
    const int index = m_scene.indexOf(m_selected);
    if (index < 0) return;
    pushUndo(capture());
    setStatus("삭제: " + m_scene.entities[static_cast<size_t>(index)].name);
    m_scene.entities.erase(m_scene.entities.begin() + index);
    m_selected = 0;
}

void Editor::focusSelected() {
    const Entity* e = selected();
    if (!e) return;
    const f32 extent = std::max({e->scale.x, e->scale.y, e->scale.z});
    m_cameraPos = e->position - cameraForward() * (std::min(extent, 60.0f) * 1.2f + 6.0f);
}

void Editor::newScene() {
    pushUndo(capture());
    scene::Scene fresh;
    Entity& ground = fresh.add(EntityKind::Solid, "Ground");
    ground.position = {0, -0.5f, 0};
    ground.scale = {120, 1, 120};
    ground.color = 0xFF609676;
    ground.flags = render::kInstanceChecker;
    Entity& spawn = fresh.add(EntityKind::SpawnPoint, "Spawn 1");
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
