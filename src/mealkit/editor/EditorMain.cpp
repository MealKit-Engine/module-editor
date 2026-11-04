// MealKitEditor: the general-purpose editor. Opens a project folder, loads that project's game DLL, and plays it.
//
//   MealKitEditor.exe                         project manager (recent projects, open, new project)
//   MealKitEditor.exe --project <folder>      open a project (folder with mealkit.project)
//   MealKitEditor.exe --project <folder> my.scene   open a different scene
//   MealKitEditor.exe --player-window --project <folder> [--player-index n] [--view-tuning <file>] --join <address:port> ...
//                                             join window mode (opened by editor multiplayer play; the rest are the same game options as the shipping exe)
//
// Game code: <project>\build\Editor\bin\<name>_game.dll. When rebuilt, it's swapped in automatically in edit mode (hot reload).
// Editor controls: right drag + WASD/QE camera, click select, W/E/R move/rotate/scale, Ctrl+S save, Ctrl+Z undo, F5 play,
//              Ctrl+R game code build, Ctrl+B shipping build

#include "mealkit/asset/ModelCache.h"
#include "mealkit/audio/Audio.h"
#include "mealkit/input/Actions.h"
#include "mealkit/feedback/Feedback.h"
#include "mealkit/core/Clock.h"
#include "mealkit/core/Log.h"
#include "mealkit/core/Profile.h"
#include "mealkit/editor/Editor.h"
#include "mealkit/editor/FieldEditor.h"
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
    u32 bodies = 0; // --bodies: match data (decided by the game)
    bool debugLayer = false;
    bool gpuCapture = false; // --gpu-capture: RenderDoc/PIX (GPU capture in the View menu, F11)
    u32 viewMode = 0;        // --view-mode <n>: start in a view mode (render::DebugView, docs/test captures)
    // For automated tests/screenshots
    u32 frames = 0; // run this many frames and exit
    std::string capturePath;
    int select = -1; // entity index to select at start (-2 = scene row, --select scene)
    std::vector<int> selectMore; // --select 3,4,5: later indices are selected too (multi-select)
    bool play = false;
    std::string replayPath; // --replay <file>: watch the replay right at start
    bool bot = false;
    bool exportGame = false; // [Shipping Build] right at start
    bool buildGame = false;  // [Game Code Build] right at start
    u32 players = 0;         // 0 = editor setting, 2~4 = multiplayer play (automated tests)
    bool upgrade = false;    // project upgrade right at start
    bool projectSettings = false; // start with the [Project Settings] window open
    bool tables = false;          // start with the [Tables] window open
    bool tuning = false;
    bool sounds = false;          // start with the [Sound] window open
    bool bakeProbes = false;      // --bake-probes: bake all reflection probes in the scene (6 frames each, with --frames)
    bool lookThrough = false;     // --look-through: the scene view looks through the selected virtual camera (with --select)
    std::string cutscene;         // --cutscene <name>: plays a cutscenes.table row in the scene view
    bool lookdev = false;         // --lookdev: start with the [Lookdev] window open (selected object's model, with --select)
    bool feedback = false;        // start with the [Feedback] window open
    std::string palette;          // --palette <text>: start with the command palette open with this text
    std::vector<std::string> commands; // --command "<English name>": run a command from the command table right after start (can be given multiple times)
    std::string uiPreview;        // open the [UI] window and preview this screen (file name in assets/ui)
    std::string uiSelect;         // with --ui: edit mode on, select this node (automated capture)
    int uiState = 0;              // with --ui-select: state shown on it (1 hover, 2 pressed, 3 focus, 4 disabled, 5 checked)
    std::string uiDrag;           // with --ui-select: "dx,dy" logical units to drag it after the first frame (logs the result)
    std::string language; // --lang: this language for this run only (docs screenshots)
    std::string penpotToken; // --penpot-token: live connection server token (Penpot import automated tests, random if empty)
    std::vector<std::string> toolPaths; // --tool-path: extra folders for the [Tools] menu to search (build folder of a tool in development)
    bool listTools = false;             // --list-tools: print found tools and exit (no window)
    u32 windowWidth = 1600;             // --window WxH: window size (checking layouts at other sizes)
    u32 windowHeight = 900;
    bool builtinManager = false;        // --builtin-manager: the in-editor project manager instead of the installed MealKit Manager
    bool uiCheck = false;    // --ui-check: compare the layout of screens exported from Penpot with the Penpot boxes and exit (no window)
    std::string uiRender, uiRenderOut; // --ui-render <ui/x.ui> <png>: draw just one screen, save, and exit (pixel comparison)
    std::string goldenShots, goldenOut; // --render-golden <shots.txt> <folder>: draw reference scenes without UI, save, and exit (graphics regression test)
};

// Scene list for the graphics regression test (engine/tests/golden/shots.txt): "name scene x y z yaw pitch" lines (angles in degrees, # comments)
struct GoldenShot {
    std::string name, scene;
    Vec3 position;
    f32 yaw = 0.0f, pitch = 0.0f;
    std::string lighting; // lighting model (pbr, toon, unlit). Project setting if empty
};

// Scene lighting model: pbr / toon / unlit at the end of the line (project setting as is if absent)
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
        else if (a == "--view-mode" && next(v)) o.viewMode = static_cast<u32>(std::atoi(v)); // render::DebugView number (automated capture)
        else if (a == "--frames" && next(v)) o.frames = static_cast<u32>(std::atoi(v));
        else if (a == "--capture" && next(v)) o.capturePath = v;
        else if (a == "--select" && next(v)) {
            o.select = std::string(v) == "scene" ? -2 : std::atoi(v);
            for (const char* c = strchr(v, ','); c; c = strchr(c + 1, ',')) o.selectMore.push_back(std::atoi(c + 1));
        }
        else if (a == "--play") o.play = true;
        else if (a == "--replay" && next(v)) o.replayPath = v;
        else if (a == "--bot") o.bot = true;
        else if (a == "--export") o.exportGame = true;
        else if (a == "--build-game") o.buildGame = true;
        else if (a == "--players" && next(v)) o.players = static_cast<u32>(std::atoi(v));
        else if (a == "--upgrade") o.upgrade = true;
        else if (a == "--project-settings") o.projectSettings = true;
        else if (a == "--tables") o.tables = true;
        else if (a == "--tuning") o.tuning = true;
        else if (a == "--sounds") o.sounds = true;
        else if (a == "--lookdev") o.lookdev = true;
        else if (a == "--look-through") o.lookThrough = true;
        else if (a == "--cutscene" && next(v)) o.cutscene = v;
        else if (a == "--palette" && next(v)) o.palette = v;
        else if (a == "--command" && next(v)) o.commands.push_back(v);
        else if (a == "--bake-probes") o.bakeProbes = true;
        else if (a == "--feedback") o.feedback = true;
        else if (a == "--ui" && next(v)) o.uiPreview = v;
        else if (a == "--ui-select" && next(v)) o.uiSelect = v;
        else if (a == "--ui-drag" && next(v)) o.uiDrag = v;
        else if (a == "--ui-state" && next(v)) o.uiState = std::atoi(v);
        else if (a == "--scene" && next(v)) o.scenePath = v;
        else if (a == "--lang" && next(v)) o.language = v;
        else if (a == "--penpot-token" && next(v)) o.penpotToken = v;
        else if (a == "--tool-path" && next(v)) o.toolPaths.push_back(v);
        else if (a == "--list-tools") o.listTools = true;
        else if (a == "--window" && next(v)) {
            int w = 0, h = 0;
            if (sscanf_s(v, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                o.windowWidth = static_cast<u32>(w);
                o.windowHeight = static_cast<u32>(h);
            }
        }
        else if (a == "--builtin-manager") o.builtinManager = true;
        else if (a == "--ui-check") o.uiCheck = true;
        else if (a == "--ui-render" && i + 2 < argc) o.uiRender = argv[++i], o.uiRenderOut = argv[++i];
        else if (a == "--render-golden" && i + 2 < argc) o.goldenShots = argv[++i], o.goldenOut = argv[++i];
        else if (!a.empty() && a[0] != '-') o.scenePath = a;
        else {
            std::printf(
                "Usage: MealKitEditor [--project <folder>] [scene file] [--bodies n] [--debug]\n"
                "  automation: --frames n --capture png --select i --play [--players n] --replay rec --bot --export --build-game --upgrade\n"
                "              --project-settings --tables --sounds --lookdev --look-through --cutscene <name> --palette <text> --command <name> --bake-probes --feedback --ui <file.ui> --ui-check --ui-render <ui/x.ui> <png>\n"
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

// Engine repo this editor was built from (CMake path → Windows notation)
std::string engineDirectory() {
    std::string dir = MEALKIT_ENGINE_DIR;
    std::replace(dir.begin(), dir.end(), '/', '\\');
    return dir;
}

// Projects chosen in the project manager open in a new process (the editor state starts clean)
bool relaunchWithProject(const std::string& directory) {
    std::string exe = executableDirectory() + "\\MealKitEditor.exe";
    return launchDetached(exe, "--project \"" + directory + "\"", directory);
}

// Opening without a project launches the hub app MealKit Manager from its separate repo (MealKit-Manager, build.bat install).
// Passes this engine folder with --engine to add it to Manager's engine list. false if not present (in-editor project manager)
bool startManager(const std::string& engineDir) {
    const std::string exe = editor::editorUserDirectory() + "\\manager\\MealKitManager.exe";
    if (!fileExists(exe)) return false;
    if (!launchDetached(exe, "--engine \"" + engineDir + "\"", engineDir)) return false;
    MEALKIT_INFO("Started MealKit Manager: %s", exe.c_str());
    return true;
}

} // namespace

int main(int argc, char** argv) {
    attachParentConsole(); // it's a GUI program (so no console window appears behind it)
    std::string engineDir = MEALKIT_ENGINE_DIR;
    std::replace(engineDir.begin(), engineDir.end(), '/', '\\');
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--player-window") { // join window of multiplayer play
            editor::initStrings(engineDir, {});
            return editor::runPlayer(argc, argv);
        }
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 1;
    if (opt.frames) log::setDialogs(false);
    editor::initStrings(engineDir, opt.language);
    if (opt.listTools) { // what the [Tools] menu finds (editor/Tools.h)
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

    // Automated runs (--frames) always use the in-editor manager (so tests don't vary by whether it's installed)
    if (opt.projectDir.empty() && !opt.builtinManager && !opt.frames && startManager(engineDir)) return 0;

    Window window;
    if (!window.create({"MealKit Editor", opt.windowWidth, opt.windowHeight, editor::kEditorCaptionColor})) return 1;
    render::Renderer renderer;
    if (!renderer.init({window.nativeHandle(), window.width(), window.height(), true, opt.debugLayer, opt.gpuCapture,
                        userDataDirectory() + "/MealKit Editor/cache"}))
        MEALKIT_FATAL("Renderer initialization failed.\n%s", renderer.initError());

    // ---- Pick a project
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
        asset::ModelBudget budgets[asset::kModelKindCount]; // when changed in the project settings window, Editor::setProject passes them again
        game::modelBudgets(project.settings, budgets);
        models.setBudgets(budgets);
        models.setProbeLimit(project.settings.probeMaxCount);
    }

    // Sound: shared by the running game and the [Sound] panel preview
    audio::Engine audio;
    audio.init({project.settings.audioVoices, false});
    feedback::Player feedback(audio); // feedback table: shared by the running game and the [Feedback] panel preview

    editor::Editor editor;
    const std::string scenePath = opt.scenePath.empty() ? project.startScenePath() : opt.scenePath;
    if (!editor.init(window, renderer, scenePath, models)) MEALKIT_FATAL("Editor initialization failed");
    editor.setAudio(&audio);
    editor::setSoundEventSource([&audio] { return audio.eventNames(); }); // pick=sound fields
    editor.setFeedback(&feedback);
    editor.setEngineTables(input::engineTables()); // engine tables shown in the [Tables] panel: input bindings (sound events are in MealKit Audio)
    editor.setEngineDirectory(engineDirectory());
    editor.setPenpotToken(opt.penpotToken);
    editor.setToolPaths(opt.toolPaths);
    editor.setAutomated(opt.frames > 0 || !opt.goldenShots.empty());
    editor.setProject(project);
    if (opt.select != -1) editor.selectByIndex(opt.select);
    for (int i : opt.selectMore) editor.selectByIndex(i, true);
    if (!opt.palette.empty()) editor.openPalette(opt.palette);
    for (const std::string& c : opt.commands) editor.queueCommand(c);
    if (opt.viewMode < static_cast<u32>(render::DebugView::Count)) editor.setDebugView(static_cast<render::DebugView>(opt.viewMode));
    if (opt.players > 1) editor.setPlayPlayers(opt.players);
    if (opt.upgrade) editor.requestUpgrade();
    if (opt.projectSettings) editor.openProjectSettings();
    if (opt.tables) editor.openTables();
    if (opt.tuning) editor.openTuning();
    if (opt.sounds) editor.openSounds();
    if (opt.lookdev) editor.openLookdev();
    if (opt.lookThrough) editor.lookThroughSelected(); // with --select <virtual camera id>
    if (!opt.cutscene.empty()) editor.previewCutscene(opt.cutscene);
    if (opt.bakeProbes) editor.bakeProbes();
    if (opt.feedback) editor.openFeedback();
    if (!opt.uiPreview.empty()) editor.openUiPanel(opt.uiPreview);
    if (!opt.uiSelect.empty() && !editor.selectUiNode(opt.uiSelect)) MEALKIT_ERROR("--ui-select: no node '%s'", opt.uiSelect.c_str());
    if (opt.uiState > 0 && opt.uiState < 6) editor.setUiPreviewState(opt.uiState);
    if (!opt.uiDrag.empty()) {
        char* end = nullptr;
        const f32 dx = std::strtof(opt.uiDrag.c_str(), &end);
        const f32 dy = *end == ',' ? std::strtof(end + 1, &end) : 0.0f;
        if (!*end && opt.uiDrag.find(',') != std::string::npos) editor.dragUiNode(opt.uiSelect, dx, dy);
        else MEALKIT_ERROR("--ui-drag: expected dx,dy");
    }
    if (opt.play) editor.requestPlay();
    if (!opt.replayPath.empty()) editor.requestReplay(opt.replayPath);
    if (opt.exportGame) editor.startBuild(editor::BuildKind::Export);

    // ---- Game DLL: if present and newer than the engine, load it right away; otherwise build first
    editor::GameModuleHost host;
    const std::string dllPath = project.gameDllPath();
    auto loadGame = [&]() {
        std::string loadError;
        editor.setGameTypes(nullptr); // the old DLL's type descriptions become invalid when it's unloaded
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
    if (opt.buildGame || !fileExists(dllPath) || editor::GameModuleHost::olderThanEngine(dllPath, engineDirectory()) ||
        editor::GameModuleHost::olderThanSources(dllPath, project.directory)) {
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

    // --render-golden: draw a few frames per scene (after models/atlases are uploaded) and save the last frame as <name>.png. Needs the game DLL's scene kinds
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

        if (!goldenShots.empty()) { // graphics regression test: fixed camera only, no editor UI
            const GoldenShot& shot = goldenShots[goldenIndex];
            if (goldenFrame == 0 && !editor.openSceneForShot(findAssetFile(shot.scene))) {
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

        // ---- Hot reload: edit mode only (the running game instance is using the previous DLL's code)
        reloadTimer += dt;
        if (editor.mode() == editor::Mode::Edit) {
            bool reload = editor.takeGameCodeBuilt();
            if (!reload && reloadTimer >= 0.5 && !editor.building()) {
                reload = host.sourceChangedAndStable(); // when built outside, e.g. VS Code
                reloadTimer = 0.0;
            }
            if (reload) loadGame();
            // If the build failed and no code is loaded, load even an older built DLL so play is possible
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
            config.replayPath = editor.takeReplayPath();
            if (!config.replayPath.empty()) config.players = 1;
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
            if (!play.failure().empty()) { // join wait timeout, session ended
                MEALKIT_ERROR("Play stopped: %s", play.failure().c_str());
                editor.showMessage(play.failure());
                stopGame();
                editor.setMode(editor::Mode::Edit);
            } else if (play.leftByGame()) { // [Leave] in the game menu
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
            feedback.screenTint(frameParams.tintColor, frameParams.tintAmount); // flash from the preview while editing
        }
        feedback.addParticles(drawList); // particles from feedback during play and the [Feedback] panel preview
        frameParams.time = static_cast<f32>(std::fmod(now, 4096.0)); // wind sway (moves while editing too)

        // Automated tests: --frames doesn't count while waiting for a build (so the build result can be checked on screen)
        const bool last = opt.frames && frame + 1 >= opt.frames;
        if (last && !opt.capturePath.empty()) renderer.requestCapture(opt.capturePath.c_str());

        const bool active = !window.minimized() && renderer.beginFrame(frameParams);
        if (active) drawList.submit(renderer);
        if (active) editor.renderLookdev(renderer, dt); // offscreen: after the main screen 3D, before UI
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
    return goldenExit; // 0 unless --render-golden
}
