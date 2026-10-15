#pragma once

#include "mealkit/asset/ModelCache.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Project.h"
#include "mealkit/platform/Process.h"
#include "mealkit/render/DrawList.h"
#include "mealkit/render/Renderer.h"
#include "mealkit/scene/Scene.h"

#include <string>
#include <vector>

namespace mealkit {
class Window;
}

// 범용 에디터(MealKitEditor)의 씬 편집 화면.
// 씬 편집(씬 트리, 인스펙터, 기즈모, 되돌리기, 저장), 플레이 모드 전환, 프로젝트 빌드(게임 코드, 출시본)를 맡는다.
// 게임 로직은 들어 있지 않다: 플레이를 누르면 앱이 게임 DLL의 GameInstance로 scene()을 실행한다.
namespace mealkit::editor {

enum class Mode { Edit, Play };

enum class BuildKind {
    GameCode, // build.bat Editor: 게임 DLL (끝나면 앱이 바꿔 끼운다)
    Export,   // build.bat Export: 출시용 exe + assets → export\<이름>
};

class Editor {
public:
    bool init(Window& window, render::Renderer& renderer, const std::string& scenePath, asset::ModelCache& models);
    void shutdown();
    // 열린 프로젝트: [빌드] 메뉴, 창 제목.
    void setProject(const ProjectInfo& project);
    const ProjectInfo& project() const { return m_project; }
    // 프로젝트의 build.bat을 백그라운드로 실행한다 (이미 빌드 중이면 무시).
    void startBuild(BuildKind kind);
    bool building() const { return m_build.running(); }
    // 게임 코드 빌드가 성공으로 끝났으면 한 번 true (앱이 DLL을 다시 불러온다)
    bool takeGameCodeBuilt();
    bool takeGameCodeBuildFailed(); // 게임 코드 빌드가 실패했으면 한 번 true
    // 게임 DLL 상태: 플레이 버튼 활성화와 상태 표시줄 문구
    void setGameCodeStatus(bool loaded, const std::string& text);
    // 게임 DLL이 선언한 오브젝트 종류 (DLL을 내리기 전에 nullptr로 되돌린다)
    void setGameTypes(const scene::TypeList* types) { m_gameTypes = types; }
    // 게임 튜닝 타입 (DLL을 내리기 전에 nullptr). 편집 중인 값은 텍스트로 보존했다가 새 타입에 다시 넣는다.
    void setTuningType(const reflect::TypeInfo* type);
    // 플레이를 시작할 때: 저장 안 된 튜닝 값을 임시 파일로 써서 경로를 돌려준다 (타입이 없으면 빈 문자열 = 기본 파일)
    std::string writePlayTuning();
    // 튜닝이 바뀌었으면 한 번 true (혼자 플레이 중이면 앱이 게임에 바로 적용한다)
    bool takeTuningChanged();
    const void* tuningData() const { return m_tuning.data(); }
    // 플레이 중인 시뮬레이션 상태 ([상태] 패널). 플레이가 끝나면 빈 View.
    void setStateView(reflect::View view) { m_stateView = view; }

    // 매 프레임 pumpMessages 다음에 호출: UI를 만들고 편집 입력을 처리한다.
    void update(f64 dt);
    // 편집 모드의 3D 장면(씬 물체 + 선택 강조)과 카메라를 채운다.
    void buildSceneView(render::FrameParams& frame, render::DrawList& drawList) const;
    // 3D를 그린 뒤 endFrame 전에 호출. 렌더러 프레임이 열리지 않았으면 frameActive = false.
    void renderUi(bool frameActive);

    Mode mode() const { return m_mode; }
    void setMode(Mode mode);
    bool takePlayRequest();
    bool takeStopRequest();
    void requestPlay() { m_playRequest = true; } // 게임 DLL이 아직 없으면 불러올 때까지 기다린다 (자동 테스트용)
    void setPlayStatus(const std::string& text) { m_playStatus = text; }

    const scene::Scene& scene() const { return m_scene; }
    render::Viewport viewport() const { return m_viewport; }
    bool gameKeyboardAllowed() const; // 플레이 중 UI가 키보드를 쓰고 있지 않은가
    bool gameMouseAllowed() const;    // 플레이 중 마우스가 뷰포트 위에 있는가
    bool quitRequested() const { return m_quit; }
    void saveBackupIfDirty();

    void selectByIndex(int index); // 자동 테스트/스크린샷용

private:
    struct Snapshot {
        std::vector<scene::Entity> entities;
        scene::Environment environment;
        u32 nextId = 1;
        u32 selected = 0;
    };

    void drawMenuBar();
    void drawStatusBar();
    void drawHierarchy(bool editing);
    void drawInspector(bool editing);
    void drawEnvironment(bool editing);
    void drawTuning();
    void drawState();
    std::string tuningPath() const;
    bool loadTuningFile();
    bool saveTuning();
    void drawGizmo();
    void drawBuildLog();
    void setupDefaultLayout(u32 dockId);

    void updateCamera(f64 dt);
    void handleShortcuts();
    void handlePicking();

    scene::Entity* selected();
    void addEntity(const std::string& type);
    const reflect::TypeInfo* entityType(const scene::Entity& e) const; // 모르는 종류(게임 코드 없음)면 nullptr
    bool isMarker(const scene::Entity& e) const;
    std::vector<const reflect::TypeInfo*> allTypes() const; // 기본 종류 + 게임 종류(라벨 순)
    void drawTypeMenuItems();
    void drawObjectFields(scene::Entity& e);
    void addModelEntity();
    std::string chooseModel(); // 파일 대화상자 → assets 기준 경로 (취소하면 빈 문자열)
    void duplicateSelected();
    void deleteSelected();
    void focusSelected();
    void newScene();
    void openSceneDialog();
    bool saveScene();
    bool saveSceneAs();
    bool loadScene(const std::string& path);

    Snapshot capture() const;
    void restore(const Snapshot& snapshot);
    void pushUndo(Snapshot snapshot);
    void undo();
    void redo();
    void markChanged() { m_changed = true; }
    void setStatus(const std::string& text);
    Vec3 cameraForward() const;
    void updateTitle();
    void pollBuild();

    Window* m_window = nullptr;
    render::Renderer* m_renderer = nullptr;
    asset::ModelCache* m_models = nullptr;
    ImGuiLayer m_imgui;

    scene::Scene m_scene;
    std::string m_scenePath;
    bool m_dirty = false;
    u32 m_selected = 0; // 엔티티 id (0 = 없음)

    Mode m_mode = Mode::Edit;
    bool m_playRequest = false;
    bool m_stopRequest = false;
    bool m_quit = false;
    std::string m_playStatus;
    std::string m_status;
    f64 m_statusTime = -100.0;
    f64 m_time = 0.0;
    f64 m_fps = 0.0;

    Vec3 m_cameraPos{0.0f, 24.0f, -48.0f};
    f32 m_cameraYaw = 0.0f;
    f32 m_cameraPitch = -0.42f;
    f32 m_cameraSpeed = 15.0f;
    bool m_looking = false;
    Mat4 m_view;
    Mat4 m_proj;
    render::Viewport m_viewport;

    int m_gizmoOp = 0; // 0 이동, 1 회전, 2 크기
    bool m_gizmoLocal = false;

    std::vector<Snapshot> m_undo;
    std::vector<Snapshot> m_redo;
    Snapshot m_pending;
    bool m_editActive = false;
    bool m_changed = false;

    char m_nameBuffer[128] = {};
    u32 m_nameBufferId = 0;
    std::string m_title;
    bool m_layoutReset = false;

    const scene::TypeList* m_gameTypes = nullptr;
    reflect::Instance m_tuning;    // 게임 DLL의 튜닝 타입 인스턴스
    reflect::TextFields m_tuningText; // DLL을 바꿔 끼울 때 값을 보존한다
    bool m_tuningLoaded = false;
    bool m_tuningDirty = false;
    bool m_tuningChanged = false;
    reflect::View m_stateView;
    const char* m_focusPanel = nullptr; // 다음 프레임에 앞으로 가져올 패널 (모드가 바뀔 때)

    ProjectInfo m_project;
    ChildProcess m_build;
    BuildKind m_buildKind = BuildKind::GameCode;
    bool m_gameCodeBuilt = false;
    bool m_gameCodeFailed = false;
    bool m_gameLoaded = false;
    std::string m_gameStatus;
    std::string m_buildLog;
    bool m_buildLogOpen = false;
    bool m_buildLogScroll = false;
    f64 m_buildStart = 0.0;
};

} // namespace mealkit::editor
