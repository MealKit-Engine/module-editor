// MealKitEditor: 범용 에디터. 프로젝트 폴더를 열고, 그 프로젝트의 게임 DLL을 불러와 플레이한다.
//
//   MealKitEditor.exe                         프로젝트 관리자 (최근 프로젝트, 열기, 새 프로젝트)
//   MealKitEditor.exe --project <폴더>        프로젝트 열기 (mealkit.project가 있는 폴더)
//   MealKitEditor.exe --project <폴더> my.scene   다른 씬 열기
//   MealKitEditor.exe --project <폴더> --player <주소:포트> --scene <파일> [--tuning <파일>] [--view-tuning <파일>] [--bot]
//                                             참가 창 모드 (에디터 여러 명 플레이가 띄운다)
//
// 게임 코드: <프로젝트>\build\Editor\bin\<이름>_game.dll. 다시 빌드되면 편집 모드에서 자동으로 바꿔 끼운다(핫 리로드).
// 에디터 조작: 오른쪽 드래그 + WASD/QE 카메라, 클릭 선택, W/E/R 이동·회전·크기, Ctrl+S 저장, Ctrl+Z 되돌리기, F5 플레이,
//              Ctrl+R 게임 코드 빌드, Ctrl+B 출시 빌드

#include "mealkit/asset/ModelCache.h"
#include "mealkit/audio/Audio.h"
#include "mealkit/input/Actions.h"
#include "mealkit/feedback/Feedback.h"
#include "mealkit/core/Clock.h"
#include "mealkit/core/Log.h"
#include "mealkit/core/Profile.h"
#include "mealkit/editor/Editor.h"
#include "mealkit/editor/GameModuleHost.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/PlaySession.h"
#include "mealkit/editor/Project.h"
#include "mealkit/editor/ProjectManager.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/game/GameInstance.h"
#include "mealkit/net/Lockstep.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Process.h"
#include "mealkit/platform/Window.h"
#include "mealkit/render/Renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>

using namespace mealkit;

namespace {

struct Options {
    std::string projectDir;
    std::string scenePath;
    u32 bodies = 0; // --bodies: 매치 데이터 (게임이 정한다)
    bool debugLayer = false;
    // 자동 테스트/스크린샷용
    u32 frames = 0; // 이 프레임 수만큼 돌고 종료
    std::string capturePath;
    int select = -1; // 시작할 때 선택할 엔티티 번호
    bool play = false;
    bool bot = false;
    bool exportGame = false; // 시작하자마자 [출시 빌드]
    bool buildGame = false;  // 시작하자마자 [게임 코드 빌드]
    u32 players = 0;         // 0 = 에디터 설정, 2~4 = 여러 명 플레이 (자동 테스트)
    bool upgrade = false;    // 시작하자마자 프로젝트 업그레이드
    bool projectSettings = false; // [프로젝트 설정] 창을 열고 시작
    bool tables = false;          // [표] 창을 열고 시작
    bool sounds = false;          // [소리] 창을 열고 시작
    bool feedback = false;        // [연출] 창을 열고 시작
    std::string uiPreview;        // [UI] 창을 열고 이 화면(assets/ui의 파일 이름)을 미리보기
    // 참가 창 모드
    std::string playerAddress;
    std::string tuningPath;
    std::string viewTuningPath;
    u32 playerIndex = 2;
    std::string language; // --lang: 이번 실행만 이 언어 (문서 스크린샷)
};

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char*& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        const char* v = nullptr;
        if (a == "--project" && next(v)) o.projectDir = v;
        else if (a == "--bodies" && next(v)) o.bodies = static_cast<u32>(std::atoi(v));
        else if (a == "--debug") o.debugLayer = true;
        else if (a == "--frames" && next(v)) o.frames = static_cast<u32>(std::atoi(v));
        else if (a == "--capture" && next(v)) o.capturePath = v;
        else if (a == "--select" && next(v)) o.select = std::atoi(v);
        else if (a == "--play") o.play = true;
        else if (a == "--bot") o.bot = true;
        else if (a == "--export") o.exportGame = true;
        else if (a == "--build-game") o.buildGame = true;
        else if (a == "--players" && next(v)) o.players = static_cast<u32>(std::atoi(v));
        else if (a == "--upgrade") o.upgrade = true;
        else if (a == "--project-settings") o.projectSettings = true;
        else if (a == "--tables") o.tables = true;
        else if (a == "--sounds") o.sounds = true;
        else if (a == "--feedback") o.feedback = true;
        else if (a == "--ui" && next(v)) o.uiPreview = v;
        else if (a == "--player" && next(v)) o.playerAddress = v;
        else if (a == "--player-index" && next(v)) o.playerIndex = static_cast<u32>(std::atoi(v));
        else if (a == "--scene" && next(v)) o.scenePath = v;
        else if (a == "--tuning" && next(v)) o.tuningPath = v;
        else if (a == "--view-tuning" && next(v)) o.viewTuningPath = v;
        else if (a == "--lang" && next(v)) o.language = v;
        else if (!a.empty() && a[0] != '-') o.scenePath = a;
        else {
            std::printf(
                "Usage: MealKitEditor [--project <folder>] [scene file] [--bodies n] [--debug]\n"
                "  automation: --frames n --capture png --select i --play [--players n] --bot --export --build-game --upgrade\n"
                "              --project-settings --tables --sounds --feedback --ui <file.ui>\n"
                "  player window: --project <folder> --player <addr:port> --scene <file> [--tuning <file>]\n"
                "                 [--view-tuning <file>] [--bot]\n");
            return false;
        }
    }
#if defined(MEALKIT_DEBUG)
    o.debugLayer = true;
#endif
    return true;
}

// 이 에디터를 빌드한 엔진 저장소 (CMake 경로 → 윈도우 표기)
std::string engineDirectory() {
    std::string dir = MEALKIT_ENGINE_DIR;
    std::replace(dir.begin(), dir.end(), '/', '\\');
    return dir;
}

// 프로젝트 관리자에서 고른 프로젝트는 새 프로세스로 연다 (에디터 상태를 깨끗하게 시작)
bool relaunchWithProject(const std::string& directory) {
    std::string exe = executableDirectory() + "\\MealKitEditor.exe";
    return launchDetached(exe, "--project \"" + directory + "\"", directory);
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 1;
    if (opt.frames) log::setDialogs(false);
    std::string engineDir = MEALKIT_ENGINE_DIR;
    std::replace(engineDir.begin(), engineDir.end(), '/', '\\');
    editor::initStrings(engineDir, opt.language);
    if (!opt.playerAddress.empty()) { // 여러 명 플레이의 참가 창
        editor::PlayerOptions player;
        player.projectDir = opt.projectDir;
        player.address = opt.playerAddress;
        player.scenePath = opt.scenePath;
        player.tuningPath = opt.tuningPath;
        player.viewTuningPath = opt.viewTuningPath;
        player.index = opt.playerIndex;
        player.bot = opt.bot;
        player.debugLayer = opt.debugLayer;
        return editor::runPlayer(player);
    }

    Window window;
    if (!window.create({"MealKit Editor", 1600, 900, editor::kEditorCaptionColor})) return 1;
    render::Renderer renderer;
    if (!renderer.init({window.nativeHandle(), window.width(), window.height(), true, opt.debugLayer}))
        MEALKIT_FATAL("Renderer initialization failed.\n%s", renderer.initError());

    // ---- 프로젝트 고르기
    if (opt.projectDir.empty()) {
        const std::string chosen =
            editor::runProjectManager(window, renderer, MEALKIT_ENGINE_DIR, opt.frames, opt.capturePath);
        if (!chosen.empty() && !relaunchWithProject(chosen))
            MEALKIT_ERROR("Could not start the editor for %s", chosen.c_str());
        renderer.shutdown();
        return 0;
    }
    editor::ProjectInfo project;
    std::string error;
    if (!editor::loadProject(opt.projectDir, project, &error)) MEALKIT_FATAL("Cannot open project: %s", error.c_str());
    setProjectRoot(project.directory);
    editor::addRecentProject(project.directory);
    MEALKIT_INFO("Project '%s': %s", project.name().c_str(), project.directory.c_str());

    asset::ModelCache models;
    models.setRenderer(&renderer);

    // 소리: 플레이 중인 게임과 [소리] 패널 미리듣기가 같이 쓴다
    audio::Engine audio;
    audio.init({project.settings.audioVoices, false});
    feedback::Player feedback(audio); // 연출 표: 플레이 중인 게임과 [연출] 패널 미리보기가 같이 쓴다

    editor::Editor editor;
    const std::string scenePath = opt.scenePath.empty() ? project.startScenePath() : opt.scenePath;
    if (!editor.init(window, renderer, scenePath, models)) MEALKIT_FATAL("Editor initialization failed");
    editor.setAudio(&audio);
    editor.setFeedback(&feedback);
    reflect::TableList engineTables = audio::engineTables(); // [표] 패널에 나오는 엔진 표: 소리 이벤트, 입력 바인딩
    engineTables.insert(engineTables.end(), input::engineTables().begin(), input::engineTables().end());
    editor.setEngineTables(engineTables);
    editor.setEngineDirectory(engineDirectory());
    editor.setProject(project);
    if (opt.select >= 0) editor.selectByIndex(opt.select);
    if (opt.players > 1) editor.setPlayPlayers(opt.players);
    if (opt.upgrade) editor.requestUpgrade();
    if (opt.projectSettings) editor.openProjectSettings();
    if (opt.tables) editor.openTables();
    if (opt.sounds) editor.openSounds();
    if (opt.feedback) editor.openFeedback();
    if (!opt.uiPreview.empty()) editor.openUiPanel(opt.uiPreview);
    if (opt.play) editor.requestPlay();
    if (opt.exportGame) editor.startBuild(editor::BuildKind::Export);

    // ---- 게임 DLL: 있고 엔진보다 새것이면 바로 불러오고, 아니면 빌드부터 한다
    editor::GameModuleHost host;
    const std::string dllPath = project.gameDllPath();
    auto loadGame = [&]() {
        std::string loadError;
        editor.setGameTypes(nullptr); // 예전 DLL의 타입 설명은 내리면 무효
        editor.setTuningType(nullptr);
        editor.setViewTuningType(nullptr);
        editor.setProjectSettingsType(nullptr);
        editor.setTables(nullptr);
        editor.setEventType(nullptr);
        editor.setViewModels(nullptr);
        const bool loaded = host.load(dllPath, loadError);
        if (host.loaded()) {
            editor.setGameTypes(host.module()->objectTypes());
            editor.setTuningType(host.module()->tuningType());
            editor.setViewTuningType(host.module()->viewTuningType ? host.module()->viewTuningType() : nullptr);
            static const reflect::TableList kNoTables;
            editor.setTables(host.module()->tables ? host.module()->tables() : &kNoTables);
            editor.setEventType(host.module()->eventType ? host.module()->eventType() : nullptr);
            editor.setViewModels(host.module()->viewModels ? host.module()->viewModels() : nullptr);
            editor.setProjectSettingsType(host.module()->projectSettingsType ? host.module()->projectSettingsType()
                                                                             : nullptr);
        }
        if (loaded) {
            editor.setGameCodeStatus(true, mealkit::editor::trf("Game code #%u", static_cast<unsigned>(host.generation())));
        } else {
            MEALKIT_ERROR("%s", loadError.c_str());
            editor.setGameCodeStatus(host.loaded(), host.loaded()
                                                        ? std::string(mealkit::editor::tr("Could not load the new game code (using the previous code)"))
                                                        : mealkit::editor::trf("Could not load the game code: %s", loadError.c_str()));
        }
    };
    if (opt.buildGame || !fileExists(dllPath) || editor::GameModuleHost::olderThanEngine(dllPath, engineDirectory())) {
        editor.setGameCodeStatus(false, mealkit::editor::tr("Building game code - you can play when it finishes"));
        editor.startBuild(editor::BuildKind::GameCode);
    } else {
        loadGame();
    }

    editor::PlaySession play;
    auto stopGame = [&]() {
        editor.setStateView({});
        editor.setSimDebug(nullptr, false);
        play.stop();
    };

    render::DrawList drawList;
    f64 lastTime = nowSeconds();
    f64 reloadTimer = 0.0;
    u32 frame = 0;

    while (window.pumpMessages() && !editor.quitRequested()) {
        const f64 now = nowSeconds();
        const f64 dt = std::min(now - lastTime, 0.25);
        lastTime = now;
        if (window.consumeResize()) renderer.resize(window.width(), window.height());

        editor.update(dt);

        // ---- 핫 리로드: 편집 모드에서만 (플레이 중인 게임 인스턴스는 이전 DLL 코드를 쓰고 있다)
        reloadTimer += dt;
        if (editor.mode() == editor::Mode::Edit) {
            bool reload = editor.takeGameCodeBuilt();
            if (!reload && reloadTimer >= 0.5 && !editor.building()) {
                reload = host.sourceChangedAndStable(); // VS Code 등 밖에서 빌드한 경우
                reloadTimer = 0.0;
            }
            if (reload) loadGame();
            // 빌드가 실패했는데 불러온 코드가 없으면, 예전에 빌드된 DLL이라도 불러와 플레이할 수 있게 한다
            if (editor.takeGameCodeBuildFailed() && !host.loaded() && fileExists(dllPath) &&
                !editor::GameModuleHost::olderThanEngine(dllPath, engineDirectory())) {
                loadGame();
                if (host.loaded())
                    editor.setGameCodeStatus(true, mealkit::editor::trf("Game code #%u (build failed - using the previously built code)",
                                                           static_cast<unsigned>(host.generation())));
            }
        }

        if (editor.mode() == editor::Mode::Edit && host.loaded() && editor.takePlayRequest()) {
            editor::PlayConfig config;
            config.players = editor.playPlayers();
            config.clientBots = editor.playClientBots();
            config.bot = opt.bot;
            config.shadow = editor.shadowCheck();
            config.bodies = opt.bodies;
            config.projectDir = project.directory;
            std::string playError;
            if (play.start(config, editor, host, models, audio, feedback, now, playError)) {
                editor.setMode(editor::Mode::Play);
            } else {
                MEALKIT_ERROR("Play failed: %s", playError.c_str());
                editor.showMessage(mealkit::editor::trf("Could not start playing: %s", playError.c_str()));
            }
        }
        if (editor.takeStopRequest() && editor.mode() == editor::Mode::Play) {
            stopGame();
            editor.setMode(editor::Mode::Edit);
        }

        const bool playing = editor.mode() == editor::Mode::Play && play.active();
        if (playing) {
            play.update(now, dt, editor, window, renderer, models);
            if (!play.failure().empty()) { // 참가 대기 시간 초과, 세션 종료
                MEALKIT_ERROR("Play stopped: %s", play.failure().c_str());
                editor.showMessage(play.failure());
                stopGame();
                editor.setMode(editor::Mode::Edit);
            } else if (play.leftByGame()) { // 게임 메뉴의 [나가기]
                MEALKIT_INFO("Play stopped by the game (left the match)");
                stopGame();
                editor.setMode(editor::Mode::Edit);
            }
        }

        audio.update(dt);
        feedback.update(dt);

        render::FrameParams frameParams;
        drawList.clear();
        if (playing && play.active()) {
            play.buildFrame(now, editor, window, renderer, models, frameParams, drawList);
        } else {
            editor.buildSceneView(frameParams, drawList);
            feedback.screenTint(frameParams.tintColor, frameParams.tintAmount); // 편집 중 미리보기의 번쩍임
        }

        // 자동 테스트: --frames는 빌드를 기다리는 동안 세지 않는다 (빌드 결과를 화면으로 확인할 수 있게)
        const bool last = opt.frames && frame + 1 >= opt.frames;
        if (last && !opt.capturePath.empty()) renderer.requestCapture(opt.capturePath.c_str());

        const bool active = !window.minimized() && renderer.beginFrame(frameParams);
        if (active) drawList.submit(renderer);
        if (active && playing && play.active()) play.renderUi(renderer);
        if (active) editor.renderUiPreview(renderer, dt);
        editor.renderUi(active);
        if (active) renderer.endFrame();

        if (!editor.building()) ++frame;
        MEALKIT_FRAME_MARK();
        if (last) break;
    }

    editor.saveBackupIfDirty();
    stopGame();
    editor.setAudio(nullptr);
    editor.setFeedback(nullptr);
    audio.shutdown();
    editor.setGameTypes(nullptr);
    editor.setTuningType(nullptr);
    editor.setViewTuningType(nullptr);
    editor.setProjectSettingsType(nullptr);
    editor.setTables(nullptr);
    editor.setEventType(nullptr);
    editor.setViewModels(nullptr);
    host.unload();
    editor.shutdown();
    renderer.shutdown();
    return 0;
}
