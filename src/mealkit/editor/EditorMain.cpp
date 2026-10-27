// MealKitEditor: 범용 에디터. 프로젝트 폴더를 열고, 그 프로젝트의 게임 DLL을 불러와 플레이한다.
//
//   MealKitEditor.exe                         프로젝트 관리자 (최근 프로젝트, 열기, 새 프로젝트)
//   MealKitEditor.exe --project <폴더>        프로젝트 열기 (mealkit.project가 있는 폴더)
//   MealKitEditor.exe --project <폴더> my.scene   다른 씬 열기
//   MealKitEditor.exe --player-window --project <폴더> [--player-index n] [--view-tuning <파일>] --join <주소:포트> ...
//                                             참가 창 모드 (에디터 여러 명 플레이가 띄운다, 나머지는 출시 exe와 같은 게임 옵션)
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
#include "mealkit/editor/Tools.h"
#include "mealkit/editor/UiCheck.h"
#include "mealkit/game/GameInstance.h"
#include "mealkit/net/Lockstep.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Process.h"
#include "mealkit/platform/System.h"
#include "mealkit/platform/Window.h"
#include "mealkit/render/Renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <vector>
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
    bool gpuCapture = false; // --gpu-capture: RenderDoc·PIX (보기 메뉴의 GPU 캡처, F11)
    u32 viewMode = 0;        // --view-mode <n>: 보기 모드로 시작 (render::DebugView, 문서·시험 캡처)
    // 자동 테스트/스크린샷용
    u32 frames = 0; // 이 프레임 수만큼 돌고 종료
    std::string capturePath;
    int select = -1; // 시작할 때 선택할 엔티티 번호 (-2 = 씬 줄, --select scene)
    bool play = false;
    bool bot = false;
    bool exportGame = false; // 시작하자마자 [출시 빌드]
    bool buildGame = false;  // 시작하자마자 [게임 코드 빌드]
    u32 players = 0;         // 0 = 에디터 설정, 2~4 = 여러 명 플레이 (자동 테스트)
    bool upgrade = false;    // 시작하자마자 프로젝트 업그레이드
    bool projectSettings = false; // [프로젝트 설정] 창을 열고 시작
    bool tables = false;          // [표] 창을 열고 시작
    bool sounds = false;          // [소리] 창을 열고 시작
    bool bakeProbes = false;      // --bake-probes: 씬의 반사 프로브를 모두 굽는다 (프레임 6개씩, --frames와 같이)
    bool lookdev = false;         // --lookdev: [룩데브] 창을 열고 시작 (고른 물체의 모델, --select와 같이)
    bool feedback = false;        // [연출] 창을 열고 시작
    std::string palette;          // --palette <글자>: 명령 팔레트를 이 글자로 열고 시작
    std::string uiPreview;        // [UI] 창을 열고 이 화면(assets/ui의 파일 이름)을 미리보기
    std::string language; // --lang: 이번 실행만 이 언어 (문서 스크린샷)
    std::string penpotToken; // --penpot-token: 라이브 연결 서버 토큰 (Penpot 가져오기 자동 시험, 비면 무작위)
    std::vector<std::string> toolPaths; // --tool-path: [도구] 메뉴가 더 찾을 폴더 (만드는 중인 도구의 빌드 폴더)
    bool listTools = false;             // --list-tools: 찾은 도구를 출력하고 끝 (창 없음)
    bool builtinManager = false;        // --builtin-manager: 설치한 MealKit Manager 대신 에디터 안의 프로젝트 관리자
    bool uiCheck = false;    // --ui-check: Penpot에서 내보낸 화면의 배치를 Penpot 상자와 비교하고 끝 (창 없음)
    std::string uiRender, uiRenderOut; // --ui-render <ui/x.ui> <png>: 화면 하나만 그려 저장하고 끝 (픽셀 비교)
    std::string goldenShots, goldenOut; // --render-golden <shots.txt> <폴더>: 기준 장면을 UI 없이 그려 저장하고 끝 (그래픽 회귀 테스트)
};

// 그래픽 회귀 테스트의 장면 목록 (engine/tests/golden/shots.txt): "이름 씬 x y z yaw pitch" 줄 (각도는 도, # 주석)
struct GoldenShot {
    std::string name, scene;
    Vec3 position;
    f32 yaw = 0.0f, pitch = 0.0f;
    std::string lighting; // 조명 모델 (pbr, toon, unlit). 비면 프로젝트 설정
};

// 장면의 조명 모델: 줄 끝의 pbr / toon / unlit (없으면 프로젝트 설정 그대로)
bool goldenLighting(const std::string& name, render::LightingModel& out) {
    const char* const names[render::kLightingModelCount] = {"pbr", "toon", "unlit"};
    for (u32 i = 0; i < render::kLightingModelCount; ++i)
        if (name == names[i]) {
            out = static_cast<render::LightingModel>(i);
            return true;
        }
    return false;
}

bool loadGoldenShots(const std::string& path, std::vector<GoldenShot>& out) {
    std::string text;
    if (!readTextFile(path, text)) return false;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (const size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream in(line);
        GoldenShot s;
        if (in >> s.name >> s.scene >> s.position.x >> s.position.y >> s.position.z >> s.yaw >> s.pitch) {
            in >> s.lighting;
            render::LightingModel model;
            if (!s.lighting.empty() && !goldenLighting(s.lighting, model)) return false;
            out.push_back(s);
        }
    }
    return !out.empty();
}

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
        else if (a == "--gpu-capture") o.gpuCapture = true;
        else if (a == "--view-mode" && next(v)) o.viewMode = static_cast<u32>(std::atoi(v)); // render::DebugView 번호 (자동 캡처)
        else if (a == "--frames" && next(v)) o.frames = static_cast<u32>(std::atoi(v));
        else if (a == "--capture" && next(v)) o.capturePath = v;
        else if (a == "--select" && next(v)) o.select = std::string(v) == "scene" ? -2 : std::atoi(v);
        else if (a == "--play") o.play = true;
        else if (a == "--bot") o.bot = true;
        else if (a == "--export") o.exportGame = true;
        else if (a == "--build-game") o.buildGame = true;
        else if (a == "--players" && next(v)) o.players = static_cast<u32>(std::atoi(v));
        else if (a == "--upgrade") o.upgrade = true;
        else if (a == "--project-settings") o.projectSettings = true;
        else if (a == "--tables") o.tables = true;
        else if (a == "--sounds") o.sounds = true;
        else if (a == "--lookdev") o.lookdev = true;
        else if (a == "--palette" && next(v)) o.palette = v;
        else if (a == "--bake-probes") o.bakeProbes = true;
        else if (a == "--feedback") o.feedback = true;
        else if (a == "--ui" && next(v)) o.uiPreview = v;
        else if (a == "--scene" && next(v)) o.scenePath = v;
        else if (a == "--lang" && next(v)) o.language = v;
        else if (a == "--penpot-token" && next(v)) o.penpotToken = v;
        else if (a == "--tool-path" && next(v)) o.toolPaths.push_back(v);
        else if (a == "--list-tools") o.listTools = true;
        else if (a == "--builtin-manager") o.builtinManager = true;
        else if (a == "--ui-check") o.uiCheck = true;
        else if (a == "--ui-render" && i + 2 < argc) o.uiRender = argv[++i], o.uiRenderOut = argv[++i];
        else if (a == "--render-golden" && i + 2 < argc) o.goldenShots = argv[++i], o.goldenOut = argv[++i];
        else if (!a.empty() && a[0] != '-') o.scenePath = a;
        else {
            std::printf(
                "Usage: MealKitEditor [--project <folder>] [scene file] [--bodies n] [--debug]\n"
                "  automation: --frames n --capture png --select i --play [--players n] --bot --export --build-game --upgrade\n"
                "              --project-settings --tables --sounds --lookdev --palette <text> --bake-probes --feedback --ui <file.ui> --ui-check --ui-render <ui/x.ui> <png>\n"
                "  tools: --tool-path <folder> (repeatable) --list-tools\n"
                "  without --project: starts MealKit Manager if installed (%%LOCALAPPDATA%%\\MealKit\\manager), --builtin-manager for the old one\n"
                "  player window: --player-window --project <folder> [--player-index n] [--view-tuning <file>]\n"
                "                 + game options (--join <addr:port> --scene <file> --tuning <file> --bot ...)\n");
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

// 프로젝트 없이 열면 별도 저장소의 허브 앱 MealKit Manager(MealKit-Manager, build.bat install)를 띄운다.
// 이 엔진 폴더를 --engine으로 넘겨 Manager의 엔진 목록에 넣는다. 없으면 false (에디터 안의 프로젝트 관리자)
bool startManager(const std::string& engineDir) {
    const std::string exe = editor::editorUserDirectory() + "\\manager\\MealKitManager.exe";
    if (!fileExists(exe)) return false;
    if (!launchDetached(exe, "--engine \"" + engineDir + "\"", engineDir)) return false;
    MEALKIT_INFO("Started MealKit Manager: %s", exe.c_str());
    return true;
}

} // namespace

int main(int argc, char** argv) {
    attachParentConsole(); // 창 프로그램이다 (뒤에 콘솔 창이 뜨지 않게)
    std::string engineDir = MEALKIT_ENGINE_DIR;
    std::replace(engineDir.begin(), engineDir.end(), '/', '\\');
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--player-window") { // 여러 명 플레이의 참가 창
            editor::initStrings(engineDir, {});
            return editor::runPlayer(argc, argv);
        }
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 1;
    if (opt.frames) log::setDialogs(false);
    editor::initStrings(engineDir, opt.language);
    if (opt.listTools) { // [도구] 메뉴가 찾는 것 (editor/Tools.h)
        const std::vector<editor::ToolInfo> tools = editor::findTools(opt.toolPaths);
        for (const editor::ToolInfo& t : tools) {
            const char* problem = t.problem == editor::ToolInfo::Problem::Protocol     ? " (protocol differs)"
                                  : t.problem == editor::ToolInfo::Problem::MissingExe ? " (program not found)"
                                                                                       : "";
            std::printf("tool: %s %s protocol %u %s%s\n", t.name.c_str(), t.version.c_str(), t.protocol, t.exe.c_str(), problem);
        }
        std::printf("%zu tool(s)\n", tools.size());
        return 0;
    }
    if (opt.uiCheck || !opt.uiRender.empty()) {
        log::setDialogs(false);
        editor::ProjectInfo project;
        std::string error;
        if (!editor::loadProject(opt.projectDir.empty() ? "." : opt.projectDir, project, &error)) {
            std::printf("Cannot open project: %s\n", error.c_str());
            return 1;
        }
        const std::string language = opt.language.empty() ? project.settings.language : opt.language;
        if (!opt.uiRender.empty()) return editor::runUiRender(project.directory, language, opt.uiRender, opt.uiRenderOut);
        return editor::runUiCheck(project.directory, language);
    }

    // 자동 실행(--frames)은 늘 에디터 안의 관리자를 쓴다 (설치 여부에 따라 시험이 달라지지 않게)
    if (opt.projectDir.empty() && !opt.builtinManager && !opt.frames && startManager(engineDir)) return 0;

    Window window;
    if (!window.create({"MealKit Editor", 1600, 900, editor::kEditorCaptionColor})) return 1;
    render::Renderer renderer;
    if (!renderer.init({window.nativeHandle(), window.width(), window.height(), true, opt.debugLayer, opt.gpuCapture}))
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
    {
        asset::ModelBudget budgets[asset::kModelKindCount]; // 프로젝트 설정 창에서 바꾸면 Editor::setProject가 다시 넘긴다
        game::modelBudgets(project.settings, budgets);
        models.setBudgets(budgets);
        models.setProbeLimit(project.settings.probeMaxCount);
    }

    // 소리: 플레이 중인 게임과 [소리] 패널 미리듣기가 같이 쓴다
    audio::Engine audio;
    audio.init({project.settings.audioVoices, false});
    feedback::Player feedback(audio); // 연출 표: 플레이 중인 게임과 [연출] 패널 미리보기가 같이 쓴다

    editor::Editor editor;
    const std::string scenePath = opt.scenePath.empty() ? project.startScenePath() : opt.scenePath;
    if (!editor.init(window, renderer, scenePath, models)) MEALKIT_FATAL("Editor initialization failed");
    editor.setAudio(&audio);
    editor.setFeedback(&feedback);
    editor.setEngineTables(input::engineTables()); // [표] 패널에 나오는 엔진 표: 입력 바인딩 (소리 이벤트는 MealKit Audio)
    editor.setEngineDirectory(engineDirectory());
    editor.setPenpotToken(opt.penpotToken);
    editor.setToolPaths(opt.toolPaths);
    editor.setAutomated(opt.frames > 0 || !opt.goldenShots.empty());
    editor.setProject(project);
    if (opt.select != -1) editor.selectByIndex(opt.select);
    if (!opt.palette.empty()) editor.openPalette(opt.palette);
    if (opt.viewMode < static_cast<u32>(render::DebugView::Count)) editor.setDebugView(static_cast<render::DebugView>(opt.viewMode));
    if (opt.players > 1) editor.setPlayPlayers(opt.players);
    if (opt.upgrade) editor.requestUpgrade();
    if (opt.projectSettings) editor.openProjectSettings();
    if (opt.tables) editor.openTables();
    if (opt.sounds) editor.openSounds();
    if (opt.lookdev) editor.openLookdev();
    if (opt.bakeProbes) editor.bakeProbes();
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

    // --render-golden: 장면마다 몇 프레임 그려(모델·아틀라스가 올라간 뒤) 마지막 프레임을 <이름>.png로. 게임 DLL의 씬 종류가 필요하다
    std::vector<GoldenShot> goldenShots;
    int goldenExit = 0;
    if (!opt.goldenShots.empty()) {
        if (!loadGoldenShots(opt.goldenShots, goldenShots)) MEALKIT_FATAL("Cannot read golden shots: %s", opt.goldenShots.c_str());
        if (!host.loaded()) MEALKIT_FATAL("--render-golden needs the game code (build the editor first)");
        createDirectories(opt.goldenOut);
    }
    constexpr u32 kGoldenFrames = 4;
    size_t goldenIndex = 0;
    u32 goldenFrame = 0;

    while (window.pumpMessages() && !editor.quitRequested()) {
        const f64 now = nowSeconds();
        const f64 dt = std::min(now - lastTime, 0.25);
        lastTime = now;
        if (window.consumeResize()) renderer.resize(window.width(), window.height());

        if (!goldenShots.empty()) { // 그래픽 회귀 테스트: 편집기 UI 없이 정한 카메라로만
            const GoldenShot& shot = goldenShots[goldenIndex];
            if (goldenFrame == 0 && !editor.openSceneForShot(findAsset("assets/" + shot.scene))) {
                std::printf("GOLDEN %s cannot open %s\n", shot.name.c_str(), shot.scene.c_str());
                goldenExit = 1;
                break;
            }
            render::LightingModel lighting = static_cast<render::LightingModel>(project.settings.lightingModel);
            goldenLighting(shot.lighting, lighting);
            renderer.setLightingModel(lighting);
            const std::string png = opt.goldenOut + "/" + shot.name + ".png";
            if (goldenFrame + 1 == kGoldenFrames) renderer.requestCapture(png.c_str());
            render::FrameParams params;
            drawList.clear();
            editor.buildShot(shot.position, shot.yaw, shot.pitch, static_cast<f32>(renderer.width()),
                             static_cast<f32>(renderer.height()), params, drawList);
            if (renderer.beginFrame(params)) {
                drawList.submit(renderer);
                renderer.endFrame();
            }
            if (++goldenFrame == kGoldenFrames) {
                std::printf("GOLDEN %s %ux%u -> %s\n", shot.name.c_str(), renderer.width(), renderer.height(),
                            fileExists(png) ? png.c_str() : "failed");
                if (!fileExists(png)) goldenExit = 1;
                goldenFrame = 0;
                if (++goldenIndex == goldenShots.size()) break;
            }
            continue;
        }

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
        feedback.addParticles(drawList); // 플레이 중 연출과 [연출] 패널 미리보기의 파티클
        frameParams.time = static_cast<f32>(std::fmod(now, 4096.0)); // 바람 흔들림 (편집 중에도 움직인다)

        // 자동 테스트: --frames는 빌드를 기다리는 동안 세지 않는다 (빌드 결과를 화면으로 확인할 수 있게)
        const bool last = opt.frames && frame + 1 >= opt.frames;
        if (last && !opt.capturePath.empty()) renderer.requestCapture(opt.capturePath.c_str());

        const bool active = !window.minimized() && renderer.beginFrame(frameParams);
        if (active) drawList.submit(renderer);
        if (active) editor.renderLookdev(renderer, dt); // 오프스크린: 본 화면 3D 다음, UI 전에
        if (active && !playing) editor.renderProbeBake(renderer);
        if (active && !playing) editor.renderGradeCapture(renderer);
        if (active && playing && play.active()) play.renderUi(renderer);
        if (active) editor.renderUiPreview(renderer, dt);
        editor.renderUi(active);
        if (active) renderer.endFrame();

        if (!editor.building() && !editor.bakingProbes()) ++frame;
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
    return goldenExit; // --render-golden이 아니면 0
}
