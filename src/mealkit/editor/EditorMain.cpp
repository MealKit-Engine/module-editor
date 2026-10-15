// MealKitEditor: 범용 에디터. 프로젝트 폴더를 열고, 그 프로젝트의 게임 DLL을 불러와 플레이한다.
//
//   MealKitEditor.exe                         프로젝트 관리자 (최근 프로젝트, 열기, 새 프로젝트)
//   MealKitEditor.exe --project <폴더>        프로젝트 열기 (mealkit.project가 있는 폴더)
//   MealKitEditor.exe --project <폴더> my.scene   다른 씬 열기
//
// 게임 코드: <프로젝트>\build\Editor\bin\<이름>_game.dll. 다시 빌드되면 편집 모드에서 자동으로 바꿔 끼운다(핫 리로드).
// 에디터 조작: 오른쪽 드래그 + WASD/QE 카메라, 클릭 선택, W/E/R 이동·회전·크기, Ctrl+S 저장, Ctrl+Z 되돌리기, F5 플레이,
//              Ctrl+R 게임 코드 빌드, Ctrl+B 출시 빌드

#include "mealkit/asset/ModelCache.h"
#include "mealkit/core/Clock.h"
#include "mealkit/core/Log.h"
#include "mealkit/core/Profile.h"
#include "mealkit/editor/Editor.h"
#include "mealkit/editor/GameModuleHost.h"
#include "mealkit/editor/Project.h"
#include "mealkit/editor/ProjectManager.h"
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
    u32 bodies = 2000;
    bool debugLayer = false;
    // 자동 테스트/스크린샷용
    u32 frames = 0;   // 이 프레임 수만큼 돌고 종료
    std::string capturePath;
    int select = -1;  // 시작할 때 선택할 엔티티 번호
    bool play = false;
    bool bot = false;
    bool exportGame = false; // 시작하자마자 [출시 빌드]
    bool buildGame = false;  // 시작하자마자 [게임 코드 빌드]
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
        else if (!a.empty() && a[0] != '-') o.scenePath = a;
        else {
            std::printf("Usage: MealKitEditor [--project <folder>] [scene file] [--bodies n] [--debug]\n"
                        "  automation: --frames n --capture png --select i --play --bot --export --build-game\n");
            return false;
        }
    }
#if defined(MEALKIT_DEBUG)
    o.debugLayer = true;
#endif
    return true;
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

    Window window;
    if (!window.create({"MealKit Editor", 1600, 900})) return 1;
    render::Renderer renderer;
    if (!renderer.init({window.nativeHandle(), window.width(), window.height(), true, opt.debugLayer}))
        MEALKIT_FATAL("Renderer initialization failed. A Direct3D 12 GPU with Shader Model 6.0 is required.");

    // ---- 프로젝트 고르기
    if (opt.projectDir.empty()) {
        const std::string chosen = editor::runProjectManager(window, renderer, MEALKIT_ENGINE_DIR, opt.frames, opt.capturePath);
        if (!chosen.empty() && !relaunchWithProject(chosen)) MEALKIT_ERROR("Could not start the editor for %s", chosen.c_str());
        renderer.shutdown();
        return 0;
    }
    editor::ProjectInfo project;
    std::string error;
    if (!editor::loadProject(opt.projectDir, project, &error)) MEALKIT_FATAL("Cannot open project: %s", error.c_str());
    setProjectRoot(project.directory);
    editor::addRecentProject(project.directory);
    MEALKIT_INFO("Project '%s': %s", project.name.c_str(), project.directory.c_str());

    asset::ModelCache models;
    models.setRenderer(&renderer);

    editor::Editor editor;
    const std::string scenePath = opt.scenePath.empty() ? project.startScenePath() : opt.scenePath;
    if (!editor.init(window, renderer, scenePath, models)) MEALKIT_FATAL("Editor initialization failed");
    editor.setProject(project);
    if (opt.select >= 0) editor.selectByIndex(opt.select);
    if (opt.play) editor.requestPlay();
    if (opt.exportGame) editor.startBuild(editor::BuildKind::Export);

    // ---- 게임 DLL: 있고 엔진보다 새것이면 바로 불러오고, 아니면 빌드부터 한다
    editor::GameModuleHost host;
    const std::string dllPath = project.gameDllPath();
    auto loadGame = [&]() {
        std::string loadError;
        editor.setGameTypes(nullptr); // 예전 DLL의 타입 설명은 내리면 무효
        const bool loaded = host.load(dllPath, loadError);
        if (host.loaded()) editor.setGameTypes(host.module()->objectTypes());
        if (loaded) {
            editor.setGameCodeStatus(true, "게임 코드 #" + std::to_string(host.generation()));
        } else {
            MEALKIT_ERROR("%s", loadError.c_str());
            editor.setGameCodeStatus(host.loaded(), host.loaded() ? "새 게임 코드를 불러오지 못했습니다 (이전 코드 사용 중)"
                                                                  : "게임 코드를 불러오지 못했습니다: " + loadError);
        }
    };
    if (opt.buildGame || !fileExists(dllPath) || editor::GameModuleHost::olderThanEngine(dllPath)) {
        editor.setGameCodeStatus(false, "게임 코드 빌드 중 - 끝나면 플레이할 수 있습니다");
        editor.startBuild(editor::BuildKind::GameCode);
    } else {
        loadGame();
    }

    std::unique_ptr<net::LockstepSession> session;
    game::GameInstance* game = nullptr;
    auto stopGame = [&]() {
        host.destroy(game);
        game = nullptr;
        session.reset();
    };
    game::PlayOptions playOptions;
    playOptions.bot = opt.bot;

    render::DrawList drawList;
    f64 lastTime = nowSeconds();
    f64 statusTimer = 0.0, reloadTimer = 0.0;
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
                !editor::GameModuleHost::olderThanEngine(dllPath)) {
                loadGame();
                if (host.loaded())
                    editor.setGameCodeStatus(true, "게임 코드 #" + std::to_string(host.generation()) +
                                                       " (빌드 실패 - 이전에 빌드된 코드 사용 중)");
            }
        }

        if (editor.mode() == editor::Mode::Edit && host.loaded() && editor.takePlayRequest()) {
            session = std::make_unique<net::LockstepSession>();
            session->startOffline(static_cast<u64>(std::time(nullptr)), opt.bodies);
            game = host.create();
            game::GameContext ctx{window, renderer, models, *session, editor.viewport()};
            game->start(ctx, editor.scene(), playOptions);
            editor.setMode(editor::Mode::Play);
            MEALKIT_INFO("Play mode: %u bodies", opt.bodies);
        }
        if (editor.takeStopRequest() && editor.mode() == editor::Mode::Play) {
            stopGame();
            editor.setMode(editor::Mode::Edit);
        }

        const bool playing = editor.mode() == editor::Mode::Play && session && game;
        if (playing) {
            game::GameContext ctx{window, renderer, models, *session, editor.viewport(), editor.gameKeyboardAllowed(),
                                  editor.gameMouseAllowed()};
            session->update(now);
            game->update(ctx, dt);
            statusTimer += dt;
            if (statusTimer >= 0.25) {
                char text[200];
                snprintf(text, sizeof(text),
                         "플레이 중 | 틱 %u | 시뮬레이션 %.2f ms/틱 | 움직이는 물체 %u | WASD 이동 · Space 점프 · 클릭 밀치기 · Esc 정지",
                         game->nextTick(), game->takeSimMsPerTick(), game->activeBodies());
                editor.setPlayStatus(text);
                statusTimer = 0.0;
            }
        }

        render::FrameParams frameParams;
        drawList.clear();
        if (playing) {
            game::GameContext ctx{window, renderer, models, *session, editor.viewport()};
            game->buildFrame(ctx, now, frameParams, drawList);
        } else {
            editor.buildSceneView(frameParams, drawList);
        }

        // 자동 테스트: --frames는 빌드를 기다리는 동안 세지 않는다 (빌드 결과를 화면으로 확인할 수 있게)
        const bool last = opt.frames && frame + 1 >= opt.frames;
        if (last && !opt.capturePath.empty()) renderer.requestCapture(opt.capturePath.c_str());

        const bool active = !window.minimized() && renderer.beginFrame(frameParams);
        if (active) drawList.submit(renderer);
        editor.renderUi(active);
        if (active) renderer.endFrame();

        if (!editor.building()) ++frame;
        MEALKIT_FRAME_MARK();
        if (last) break;
    }

    editor.saveBackupIfDirty();
    stopGame();
    editor.setGameTypes(nullptr);
    host.unload();
    editor.shutdown();
    renderer.shutdown();
    return 0;
}
