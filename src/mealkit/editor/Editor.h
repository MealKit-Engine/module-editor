#pragma once

#include "mealkit/asset/ModelCache.h"
#include "mealkit/editor/BuildPanel.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Project.h"
#include "mealkit/editor/FeedbackPanel.h"
#include "mealkit/editor/ProjectSettingsPanel.h"
#include "mealkit/editor/TuningPanel.h"
#include "mealkit/editor/ImportServer.h"
#include "mealkit/editor/LookdevPanel.h"
#include "mealkit/editor/Tools.h"
#include "mealkit/editor/UiPanel.h"
#include "mealkit/editor/SoundPanel.h"
#include "mealkit/editor/TablesPanel.h"
#include "mealkit/game/FrameStats.h"
#include "mealkit/game/RenderStyle.h"
#include "mealkit/game/SimDebug.h"
#include "mealkit/platform/Image.h"
#include "mealkit/render/DrawList.h"
#include "mealkit/render/Renderer.h"
#include "mealkit/scene/Scene.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace mealkit {
class Window;
}

// 범용 에디터(MealKitEditor)의 씬 편집 화면.
// 씬 편집(씬 트리, 인스펙터, 기즈모, 되돌리기, 저장), 플레이 모드 전환, 프로젝트 빌드(게임 코드, 출시본)를 맡는다.
// 게임 로직은 들어 있지 않다: 플레이를 누르면 앱이 게임 DLL의 GameInstance로 scene()을 실행한다.
namespace mealkit::editor {

enum class Mode { Edit, Play };

class Editor {
public:
    bool init(Window& window, render::Renderer& renderer, const std::string& scenePath, asset::ModelCache& models);
    void shutdown();
    // 열린 프로젝트: [빌드] 메뉴, 창 제목.
    void setProject(const ProjectInfo& project);
    void setEngineDirectory(const std::string& directory) { m_engineDirectory = directory; }
    // Penpot 가져오기 서버의 토큰 (setProject 전에, 자동 시험용. 비면 무작위)
    void setPenpotToken(const std::string& token) { m_penpotToken = token; }
    void setToolPaths(const std::vector<std::string>& folders) { m_toolPaths = folders; } // --tool-path
    // [업그레이드]를 누른 것과 같다 (자동 테스트용)
    void requestUpgrade() {
        m_upgradeRequested = true;
        m_upgradeOffer = false;
    }
    const ProjectInfo& project() const { return m_project; }
    // 프로젝트의 build.bat을 백그라운드로 실행한다 (이미 빌드 중이면 무시).
    void startBuild(BuildKind kind);
    bool building() const { return m_buildPanel.running(); }
    // 게임 코드 빌드가 성공으로 끝났으면 한 번 true (앱이 DLL을 다시 불러온다)
    bool takeGameCodeBuilt();
    bool takeGameCodeBuildFailed(); // 게임 코드 빌드가 실패했으면 한 번 true
    // 게임 DLL 상태: 플레이 버튼 활성화와 상태 표시줄 문구
    void setGameCodeStatus(bool loaded, const std::string& text);
    // 게임 DLL이 선언한 오브젝트 종류 (DLL을 내리기 전에 nullptr로 되돌린다)
    void setGameTypes(const scene::TypeList* types) { m_gameTypes = types; }
    // 게임 튜닝 타입 (DLL을 내리기 전에 nullptr). 편집 중인 값은 텍스트로 보존했다가 새 타입에 다시 넣는다.
    void setTuningType(const reflect::TypeInfo* type) { m_tuningPanel.setTuningType(type); }
    // 플레이를 시작할 때: 저장 안 된 튜닝 값을 임시 파일로 써서 경로를 돌려준다 (타입이 없으면 빈 문자열 = 기본 파일)
    std::string writePlayTuning() { return m_tuningPanel.writePlayTuning(); }
    // 튜닝이 바뀌었으면 한 번 true (혼자 플레이 중이면 앱이 게임에 바로 적용한다)
    bool takeTuningChanged() { return m_tuningPanel.takeTuningChanged(); }
    const void* tuningData() const { return m_tuningPanel.tuning().data; }
    // 화면용 튜닝 (GameModule::viewTuningType, assets/view.tuning): 고치면 플레이 중인 게임에 바로 보인다 (여러 명이어도).
    void setViewTuningType(const reflect::TypeInfo* type) { m_tuningPanel.setViewTuningType(type); }
    reflect::View viewTuning() const { return m_tuningPanel.viewTuning(); }
    // 참가 창이 읽는 화면용 튜닝 사본 (여러 명 플레이를 시작할 때 쓰고, 값이 바뀌면 다시 쓴다)
    std::string writePlayViewTuning() { return m_tuningPanel.writePlayViewTuning(); }
    bool takeViewTuningChanged() { return m_tuningPanel.takeViewTuningChanged(); }
    static constexpr const char* kPlayViewTuningFile = TuningPanel::kPlayViewTuningFile;
    // 게임이 더한 프로젝트 설정 타입 (GameModule::projectSettingsType, DLL을 내리기 전에 nullptr). 값은 텍스트로 보존한다.
    void setProjectSettingsType(const reflect::TypeInfo* type) { m_projectPanel.setGameType(type); }
    // [프로젝트 설정] 창을 연다 (자동 테스트용)
    void openProjectSettings() { m_projectPanel.open(); }
    // 게임이 등록한 정의 데이터 표 ([표] 패널, DLL을 내리기 전에 nullptr. 값은 텍스트로 보존한다)
    void setTables(const reflect::TableList* tables) { m_tablesPanel.setTables(tables); }
    // 플레이를 시작할 때: 고친 표를 저장한다 (게임은 매치를 시작할 때 파일에서 읽는다)
    void saveTablesForPlay();
    void openTables() { m_tablesOpen = true; } // 자동 테스트용
    void setEngineTables(const reflect::TableList& tables) { m_tablesPanel.setEngineTables(tables); }
    // 소리 ([소리] 패널 미리듣기·목소리 목록). 에디터가 만들어 넘긴다 (nullptr = 없음)
    void setAudio(audio::Engine* audio) { m_audio = audio; }
    void openSounds() { m_soundsOpen = true; } // 자동 테스트용
    // 연출 ([연출] 패널 미리보기). nullptr = 없음
    void setFeedback(feedback::Player* player) { m_feedback = player; }
    void setEventType(const reflect::TypeInfo* type) { m_feedbackPanel.setEventType(type); }
    void openFeedback() { m_feedbackOpen = true; } // 자동 테스트용
    // 게임 화면 UI 미리보기 ([UI] 패널): 게임의 뷰 모델 목록, 편집 모드에서 씬 뷰 위에 그리기
    void setViewModels(const std::vector<const reflect::TypeInfo*>* types) {
        m_uiPanel.setViewModels(types);
        m_import.setViewModels(types);
    }
    void openUiPanel(const std::string& file = {}) {
        m_uiPanelOpen = true;
        if (!file.empty()) m_uiPanel.select(file);
    }
    void renderUiPreview(render::Renderer& renderer, f64 dt) {
        if (m_uiPanelOpen && m_mode == Mode::Edit) m_uiPanel.render(renderer, m_viewport, dt);
    }
    // [룩데브] 창의 오프스크린 그림: 본 화면 3D 다음, UI 전에 (창이 열려 있을 때만)
    void renderLookdev(render::Renderer& renderer, f64 dt);
    void openLookdev() { m_lookdevOpen = true; } // --lookdev (자동 캡처)
    // 반사 프로브 굽기: 본 화면 3D 다음, UI 전에. 한 프레임에 큐브 면 하나를 오프스크린으로 그리고 endFrame에서 읽는다
    void renderProbeBake(render::Renderer& renderer);
    void bakeProbes(u32 onlyId = 0); // 0 = 씬의 모든 반사 프로브 (--bake-probes)
    // 색 보정용 캡처 ([환경] 패널 단추): 씬 뷰 카메라로 LUT 없이 그린 화면을 art/grading/<씬>.png로, 중립 LUT를 그 옆에.
    // 외부 도구(DaVinci Resolve·Photoshop 등)에서 보정해 .cube로 내보내 assets/lut/ 아래에 둔다. 본 화면 3D 다음, UI 전에
    void renderGradeCapture(render::Renderer& renderer);
    bool bakingProbes() const { return !m_probeQueue.empty(); }
    // 플레이 중인 시뮬레이션 상태 ([상태] 패널). 플레이가 끝나면 빈 View.
    void setStateView(reflect::View view) { m_stateView = view; }

    // 매 프레임 pumpMessages 다음에 호출: UI를 만들고 편집 입력을 처리한다.
    void update(f64 dt);
    // 편집 모드의 3D 장면(씬 물체 + 선택 강조)과 카메라를 채운다.
    void buildSceneView(render::FrameParams& frame, render::DrawList& drawList) const;
    void setDebugView(render::DebugView view) { m_debugView = view; } // --view-mode (자동 캡처)
    // 그래픽 회귀 테스트 (--render-golden): 씬을 열고, 정한 카메라(도)로 창 전체에 UI 없이 그린다
    bool openSceneForShot(const std::string& path) { return loadScene(path); }
    void buildShot(Vec3 position, f32 yawDegrees, f32 pitchDegrees, f32 width, f32 height, render::FrameParams& frame,
                   render::DrawList& drawList);
    // 3D를 그린 뒤 endFrame 전에 호출. 렌더러 프레임이 열리지 않았으면 frameActive = false.
    void renderUi(bool frameActive);

    Mode mode() const { return m_mode; }
    void setMode(Mode mode);
    bool takePlayRequest();
    bool takeStopRequest();
    void requestPlay() { m_playRequest = true; } // 게임 DLL이 아직 없으면 불러올 때까지 기다린다 (자동 테스트용)
    // 플레이 인원: 1 = 혼자, 2~4 = 에디터가 방장이 되고 참가 창을 더 띄운다
    u32 playPlayers() const { return m_playPlayers; }
    void setPlayPlayers(u32 players) { m_playPlayers = players < 1 ? 1 : players > 4 ? 4 : players; }
    bool playClientBots() const { return m_playClientBots; }
    bool shadowCheck() const { return m_shadowCheck; }
    // 플레이 중인 시뮬레이션의 디버그 제어 (일시정지·되감기·그림자 시뮬레이션). 플레이가 끝나면 nullptr
    void setSimDebug(game::SimDebug* debug, bool canPause);
    // 화면 가운데 알림 창 (확인을 누르면 닫힌다)
    void showMessage(const std::string& text) { m_message = text; }
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
    void drawEntityNode(size_t index, const std::unordered_map<u32, std::vector<size_t>>& children);
    void reparent(u32 entityId, u32 parentId); // 월드 위치를 지키며 부모를 바꾼다 (0 = 최상위)
    void drawInspector(bool editing);
    void drawEnvironment(bool editing);
    void updateRowProblems(f64 dt); // 튜닝·씬 물체의 없는 행 참조 → [표] 패널
    void saveAll();         // Ctrl+S: 씬, 튜닝, 프로젝트 설정, 표
    void takePanelStatus(); // 패널들이 남긴 상태 문구를 상태 표시줄로
    void drawState();
    void drawPopups();
    void drawPlayControls();
    void reportSimProblems();
    void updateAutosave(f64 dt);
    void updateHotReload(f64 dt); // 바뀐 모델·씬 파일 다시 읽기 (편집 모드)
    void focusPanel(const char* window);
    void focusPanelNow(); // 그 창의 Begin 직전에 부른다
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
    bool isArea(const scene::Entity& e) const;
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
    void captureGpuFrame(); // 보기 메뉴·F11 (rhi/GpuCapture.h)
    Vec3 cameraForward() const;
    void updateTitle();
    void pollBuild();
    void startLayoutCheck();
    void pollLayoutCheck();

    Window* m_window = nullptr;
    render::Renderer* m_renderer = nullptr;
    asset::ModelCache* m_models = nullptr;
    ImGuiLayer m_imgui;

    scene::Scene m_scene;
    std::string m_scenePath;
    bool m_dirty = false;
    u32 m_selected = 0; // 엔티티 id (0 = 없음)

    Mode m_mode = Mode::Edit;
    render::DebugView m_debugView = render::DebugView::None; // [보기] > 보기 모드 (편집 화면만)
    bool m_playRequest = false;
    bool m_stopRequest = false;
    bool m_quit = false;
    std::string m_playStatus;
    std::string m_status;
    f64 m_statusTime = -100.0;
    f64 m_time = 0.0;
    game::FrameStats m_perf; // 상태 표시줄 fps (mealkit/game/FrameStats.h)

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

    struct TreeAction {
        enum Kind { None, Reparent, Duplicate, Delete } kind = None;
        u32 entity = 0;
        u32 parent = 0;
    };
    TreeAction m_treeAction; // 씬 트리에서 고른 구조 변경 (트리를 다 그린 뒤 처리)

    char m_nameBuffer[128] = {};
    u32 m_nameBufferId = 0;
    std::string m_title;
    bool m_layoutReset = false;

    const scene::TypeList* m_gameTypes = nullptr;
    TuningPanel m_tuningPanel;
    reflect::View m_stateView;
    const char* m_focusPanel = nullptr; // 앞으로 가져올 패널 (모드가 바뀔 때)
    int m_focusFrames = 0;
    u32 m_playPlayers = 1;
    bool m_shadowCheck = true;
    game::SimDebug* m_simDebug = nullptr;
    bool m_canPause = false;
    bool m_shadowReported = false;
    bool m_replayReported = false;
    bool m_playClientBots = true;
    std::string m_message;     // 알림 창 내용
    std::string m_recoverPath; // 복구할지 물어볼 자동 저장 파일
    f64 m_autosaveTimer = 0.0;
    f64 m_hotReloadTimer = 0.0;
    game::RenderStyleState m_renderStyle;
    std::vector<u32> m_probeQueue;     // 구울 프로브 물체 id (앞에서부터)
    u32 m_probeFace = 0;               // 지금 그리는 면 (0..5)
    bool m_probeCapturePending = false; // 지난 프레임에 그린 면을 endFrame이 m_probeCapture로 읽었다
    rhi::RenderTarget m_probeTarget;
    u32 m_probeTargetSize = 0;
    Image m_probeCapture;
    std::vector<f32> m_probeFaces; // 면 6 × 크기² × rgb 선형
    bool m_gradeRequested = false, m_gradeCapturePending = false; // 보정용 캡처: 단추를 눌렀다 / 지난 프레임에 그렸다
    rhi::RenderTarget m_gradeTarget;
    u32 m_gradeTargetSize[2] = {};
    Image m_gradeCapture;
    mutable std::vector<render::ReflectionProbe> m_probeList; // buildSceneView가 프레임마다 채운다
    mutable std::vector<render::PointLight> m_lightList;      // 씬 점광원 (buildSceneView·프로브 굽기)
    f64 m_animTime = 0.0; // 스킨 모델 미리보기 시계 (편집 모드에서 흐른다, 그래픽 회귀 테스트는 0)
    void addSceneObjects(render::DrawList& out, Vec3 eye, bool gameView) const; // gameView: 게임에 보이는 것만, 강조 없이
    void fillEnvironment(render::FrameParams& frame) const;
    u32 probeMaxCount() const;
    LookdevPanel m_lookdev; // [룩데브] 창: 고른 물체의 모델을 고정 조명·받침대에서
    bool m_lookdevOpen = false; // 프로젝트 설정의 조명 모델·툰 램프 (핫 리로드가 바뀌면 다시 넘긴다)
    unsigned long long m_sceneTime = 0; // 열거나 저장할 때 씬 파일 시각 (밖에서 바뀌었는지)
    bool m_sceneChangedWarned = false;
    bool m_autosavePending = false; // 마지막 자동 저장 뒤에 바뀐 것이 있다

    ProjectInfo m_project;              // 디스크에 저장된 프로젝트 설정
    ProjectSettingsPanel m_projectPanel; // [프로젝트 설정] 창
    std::string m_engineDirectory;
    bool m_upgradeOffer = false;     // 프로젝트 템플릿이 엔진 버전과 다르다 (한 번 묻는다)
    bool m_upgradeRequested = false; // 빌드가 끝나면 업그레이드를 시작한다
    BuildPanel m_buildPanel;
    bool m_gameCodeBuilt = false;
    bool m_gameCodeFailed = false;
    bool m_gameLoaded = false;
    std::string m_gameStatus;
    ChildProcess m_layoutCheck; // tools\check_layout.ps1 (프로젝트를 열 때)
    TablesPanel m_tablesPanel;
    bool m_tablesOpen = false;
    audio::Engine* m_audio = nullptr;
    SoundPanel m_soundPanel;
    bool m_soundsOpen = false;
    feedback::Player* m_feedback = nullptr;
    FeedbackPanel m_feedbackPanel;
    bool m_feedbackOpen = false;
    UiPanel m_uiPanel;
    bool m_uiPanelOpen = false;
    ImportServer m_import; // Penpot 플러그인 가져오기 (라이브 연결 서버의 penpot/ 서비스)
    live::Server m_live;   // 라이브 연결 (도구 사이 약속, 프로젝트를 열면 시작. m_import보다 먼저 닫힌다)
    std::vector<ToolInfo> m_tools; // [도구] 메뉴 (프로젝트를 열 때와 [다시 찾기]에서 찾는다)
    std::vector<std::string> m_toolPaths;
    std::string m_penpotToken;
    bool m_viewTableDirty = false;
    f64 m_viewTableTimer = 0.0;
    f64 m_rowCheckTimer = 0.0;
    std::string m_layoutOutput;
};

} // namespace mealkit::editor
