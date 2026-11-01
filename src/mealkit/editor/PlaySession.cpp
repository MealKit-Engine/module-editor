#include "mealkit/editor/PlaySession.h"

#include "mealkit/debug/DebugDrawRender.h"

#include "mealkit/asset/ModelCache.h"
#include "mealkit/audio/Audio.h"
#include "mealkit/feedback/Feedback.h"
#include "mealkit/core/Clock.h"
#include "mealkit/core/Log.h"
#include "mealkit/editor/Editor.h"
#include "mealkit/editor/GameModuleHost.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Project.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/game/BugReport.h"
#include "mealkit/game/DesyncDump.h"
#include "mealkit/game/FrameStats.h"
#include "mealkit/game/GameData.h"
#include "mealkit/game/GameMain.h"
#include "mealkit/game/GameOptions.h"
#include "mealkit/game/InputRecord.h"
#include "mealkit/input/Actions.h"
#include "mealkit/net/Lockstep.h"
#include "mealkit/net/Socket.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Window.h"
#include "mealkit/render/DrawList.h"
#include "mealkit/render/Renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>

namespace mealkit::editor {
namespace {

constexpr u32 kInputDelayMs = 100; // 127.0.0.1 join windows: fixed (no auto adjustment)

std::string playDirectory(const std::string& projectDir) { return projectDir + "\\build\\" + editorBuildName() + "\\play"; }

} // namespace

u64 playContentHash(const scene::Scene& scene, asset::ModelCache& models, const game::GameModule& module,
                    const std::string& tuningPath, const game::ProjectFile& project, u64* parts) {
    game::MatchDataOptions o;
    o.scene = &scene;
    o.tuningPath = tuningPath;
    o.project = &project;
    game::MatchData data;
    game::loadMatchData(game::gameTypes(module), o, models, data); // a scene is given, so it doesn't fail
    if (parts) std::copy(std::begin(data.contentParts), std::end(data.contentParts), parts);
    return data.contentHash;
}

PlaySession::~PlaySession() { stop(); }

bool PlaySession::start(const PlayConfig& config, Editor& editor, GameModuleHost& host, asset::ModelCache& models,
                        audio::Engine& audio, feedback::Player& feedback, f64 now, std::string& error) {
    stop();
    m_audio = &audio;
    m_feedback = &feedback;
    m_frontReady = false; // the renderer is received in update
    m_config = config;
    m_config.players = std::clamp(config.players, 1u, net::kMaxPlayers);
    m_host = &host;
    m_failure.clear();
    m_startTime = now;
    m_left = m_firstMatch = m_loadingShown = false;
    m_options = {};
    m_options.bot = config.bot;
    m_options.shadow = config.shadow;
    m_options.record = config.players <= 1; // rewind only in solo play (can't pause with multiple players)
    m_options.tuningPath = editor.writePlayTuning();
    m_options.desyncDumpDir = config.projectDir + "/build/" + game::kDesyncFolder; // when a desync happens in multiplayer play
    editor.saveTablesForPlay(); // the game reads table files when a match starts

    m_game = host.create();
    if (!m_game) {
        error = tr("Could not create the game instance");
        stop();
        return false;
    }
    m_match = std::make_unique<game::MatchService>();
    game::MatchServiceConfig matchConfig;
    matchConfig.gameName = editor.project().name();
    matchConfig.defaultPort = kEditorPlayPort;
    matchConfig.defaultMaxPlayers = m_config.players;
    matchConfig.inputDelay = game::msToTicks(kInputDelayMs);
    matchConfig.loopbackOnly = true; // only join windows on the same PC (no firewall prompt)
    matchConfig.announceRooms = false;
    matchConfig.build = host.module()->build;
    m_userFolder.clear();
    m_replayStopped = false;
    if (replay()) {
        game::InputRecording recording;
        std::string loadError, recordingPath = m_config.replayPath, bugInfo;
        // Bug report bundle (game/BugReport.h): extract the input recording from the zip, replay up to the tick where the recording ends, and pause
        m_bugReport = recordingPath.size() > 4 && recordingPath.compare(recordingPath.size() - 4, 4, ".zip") == 0;
        if (m_bugReport &&
            !game::extractBugReport(m_config.replayPath, playDirectory(config.projectDir), recordingPath, bugInfo, loadError))
            recordingPath.clear();
        if (recordingPath.empty() || !game::loadInputRecording(recordingPath, recording, loadError) || recording.inputs.empty()) {
            error = trf("Could not read the replay: %s",
                        loadError.empty() ? tr("it has no ticks") : loadError.c_str());
            stop();
            return false;
        }
        const u64 hash = playContentHash(editor.scene(), models, *host.module(), m_options.tuningPath, editor.project());
        if (hash != recording.contentHash) // still watch, but pause at the tick where it diverges (update)
            editor.showMessage(tr("This replay was recorded with different game data (scene, models, tuning or project "
                                  "settings). It may diverge."));
        m_match->init(matchConfig);
        m_match->playReplay(recording);
        m_firstMatch = true;
        if (m_bugReport) {
            const size_t note = bugInfo.find("\nnote = ");
            const std::string text = note == std::string::npos
                                         ? std::string()
                                         : bugInfo.substr(note + 8, bugInfo.find('\n', note + 1) - note - 8);
            // Crash reports (crash/<time>.zip) have the same layout; the recording ends at the crash
            const bool crash = bugInfo.rfind("# MealKit crash report", 0) == 0;
            MEALKIT_INFO("%s %s:\n%s", crash ? "Crash report" : "Bug report", m_config.replayPath.c_str(), bugInfo.c_str());
            if (crash) editor.showMessage(tr("Crash report: playing up to the tick where the game crashed."));
            else if (!text.empty()) editor.showMessage(trf("Bug report: %s", text.c_str()));
        }
        MEALKIT_INFO("Editor play: replaying %s", m_config.replayPath.c_str());
    } else if (!multiplayer()) {
        // Solo play also keeps a replay per match (user folder replays/, named by date/time on stop)
        m_keepReplays = editor.project().settings.keepReplays;
        if (m_keepReplays > 0 && !config.bot) {
            m_userFolder = game::userFolder(editor.project().settings);
            game::keepReplay(m_userFolder, m_keepReplays); // recording left over from a crash last time
            m_options.recordInputsPath = game::replayRecordingPath(m_userFolder);
            matchConfig.contentHash = playContentHash(editor.scene(), models, *host.module(), m_options.tuningPath,
                                                      editor.project());
        }
        m_match->init(matchConfig);
        m_match->playOffline();
        m_game->lobby(m_match->session(), m_options, game::userFolder(editor.project().settings)); // loadout
        m_match->start(config.bodies);
        m_firstMatch = true;
    } else {
        // Scene copy read by join windows (including unsaved edits)
        const std::string dir = playDirectory(config.projectDir);
        createDirectories(dir);
        const std::string scenePath = dir + "\\play.scene";
        if (!scene::save(editor.scene(), scenePath)) {
            error = trf("Could not write the temporary scene: %s", scenePath.c_str());
            return false;
        }
        matchConfig.contentHash =
            playContentHash(editor.scene(), models, *host.module(), m_options.tuningPath, editor.project(), matchConfig.contentParts);
        m_match->init(matchConfig);
        const std::string viewTuningPath = editor.writePlayViewTuning(); // join windows reread this copy whenever it changes
        editor.takeViewTuningChanged();
        if (!m_match->host(kEditorPlayPort, m_config.players)) {
            error = trf("Could not open port %u (is another play session using it?)", static_cast<unsigned>(kEditorPlayPort));
            m_match.reset();
            return false;
        }
        const std::string exe = executableDirectory() + "\\MealKitEditor.exe";
        for (u32 i = 2; i <= m_config.players; ++i) {
            std::string args = "--player-window --project \"" + config.projectDir + "\" --player-index " +
                               std::to_string(i) + " --join 127.0.0.1:" + std::to_string(kEditorPlayPort) +
                               " --scene \"" + scenePath + "\"";
            if (!m_options.tuningPath.empty()) args += " --tuning \"" + m_options.tuningPath + "\"";
            if (!viewTuningPath.empty()) args += " --view-tuning \"" + viewTuningPath + "\"";
            if (config.clientBots) args += " --bot";
            const std::string log = dir + "\\player" + std::to_string(i) + ".log";
            if (!m_clients.launch(exe, args, config.projectDir, log)) {
                error = tr("Could not start the player windows");
                stop();
                return false;
            }
        }
        MEALKIT_INFO("Editor play: hosting %u players on port %u", m_config.players, kEditorPlayPort);
    }
    editor.takeTuningChanged(); // already passed as a temp file at start
    return true;
}

void PlaySession::stop() {
    if (m_audio) m_audio->stopAll(); // sounds made by the game (including background music loops)
    if (m_feedback) m_feedback->clear();
    m_clients.terminateAll();
    if (m_host && m_game) m_host->destroy(m_game); // the simulation closes the input recording file
    m_game = nullptr;
    m_match.reset();
    if (!m_userFolder.empty()) game::keepReplay(m_userFolder, m_keepReplays);
    m_userFolder.clear();
}

void PlaySession::update(f64 now, f64 dt, Editor& editor, Window& window, render::Renderer& renderer,
                         asset::ModelCache& models) {
    if (!m_game || !m_match || !m_failure.empty() || m_left) return;
    if (!m_frontReady) {
        game::FrontendConfig c; // only the project's bindings (no player-changed key file), display options aren't applied
        c.renderer = &renderer;
        c.audio = m_audio;
        c.feedback = m_feedback;
        c.projectLanguage = editor.project().settings.language;
        c.settingsFile = game::userFolder(editor.project().settings) + "/" + settings::kSettingsFile;
        m_front.init(c); // view tuning is passed by the editor (values edited in the panel)
        m_frontReady = true;
    }
    net::LockstepSession& session = m_match->session();
    m_match->update(now);
    if (m_match->phase() == game::MatchPhase::Menu) { // the game pressed [Leave] (replay after the results screen)
        m_left = true;
        return;
    }
    // When the replay diverges from the recording, pause at that tick (inspect with rewind/[State] panel)
    if (session.isReplay() && session.desynced() && !m_replayStopped && m_game->debug()) {
        m_game->debug()->setPaused(true);
        m_replayStopped = true;
        editor.showMessage(trf("The replay diverged from the recording at tick %u (different build or data).",
                               session.desyncTick()));
    }
    // Bug reports pause at the tick where the recording ended (when F9 was pressed)
    if (m_bugReport && !m_replayStopped && m_game->started() && m_game->debug() &&
        m_game->debug()->timeline().live >= session.replayTicks()) {
        m_game->debug()->setPaused(true);
        m_replayStopped = true;
    }
    if (!m_game->started() && session.state() == net::SessionState::Lobby)
        m_game->lobby(session, m_options, game::userFolder(editor.project().settings));

    // Host: the first match starts when everyone is in (later matches are started by the game's lobby screen)
    if (!m_firstMatch && session.isHost() && session.state() == net::SessionState::Lobby &&
        session.lobbyPlayerCount() >= m_config.players) {
        m_match->start(m_config.bodies);
        m_firstMatch = true;
    }
    if (session.state() == net::SessionState::Ended) {
        m_failure = trf("The session ended: %s", session.endReason());
        return;
    }
    if (!m_firstMatch) {
        if (now - m_startTime > kJoinTimeoutSeconds) {
            m_failure = trf("The player windows did not connect within %d seconds (see build\\Editor\\play\\player*.log)",
                            static_cast<int>(kJoinTimeoutSeconds));
            return;
        }
        char text[160];
        snprintf(text, sizeof(text), tr("Waiting for player windows (%u/%u) | Esc to stop"), session.lobbyPlayerCount(),
                 m_config.players);
        editor.setPlayStatus(text);
        return;
    }

    // View tuning: pass the editor values as is, and with multiple players rewrite the copy join windows read (at most once every 0.2 s)
    m_viewTuningTimer += dt;
    if (multiplayer() && m_viewTuningTimer >= 0.2 && editor.takeViewTuningChanged()) {
        editor.writePlayViewTuning();
        m_viewTuningTimer = 0.0;
    }
    game::GameContext ctx = m_front.context(window, renderer, models, *m_match);
    ctx.viewport = editor.viewport();
    ctx.keyboardAllowed = editor.gameKeyboardAllowed();
    ctx.mouseAllowed = editor.gameMouseAllowed();
    ctx.viewTuningData = editor.viewTuning();
    ctx.userFolder = game::userFolder(editor.project().settings);
    ctx.scene = &editor.scene();
    // Match start (after drawing the loading screen once) and end (back to the lobby)
    if (!m_game->started() && session.state() == net::SessionState::Running) {
        if (m_loadingShown) {
            m_game->start(ctx, editor.scene(), m_options);
            MEALKIT_INFO("Play mode: %u player(s), match data %u", session.playerCount(), session.matchData());
        }
        m_loadingShown = !m_loadingShown;
    } else if (m_game->started() && session.state() != net::SessionState::Running) {
        m_game->stop(ctx);
        m_loadingShown = false;
    }
    // Gamepads only when the game view has the keyboard (so the character doesn't move while editing other panels)
    m_front.update(now, dt, window.input(), editor.gameKeyboardAllowed(), ctx);
    // Solo play: tuning changes apply immediately. With multiple players they'd differ per peer and desync, so they aren't applied.
    if (editor.takeTuningChanged() && !multiplayer()) m_game->setTuning(editor.tuningData());
    m_game->update(ctx, dt);
    m_game->debugUi(ctx); // inside the editor's ImGui frame (the game DLL uses the same ImGui from mealkit.dll)
    editor.setStateView(m_game->stateView());
    editor.setSimDebug(m_game->debug(), !multiplayer());

    if (m_perf.update(dt, {nullptr, &session, m_audio, m_game})) {
        const game::FrameSample& perf = m_perf.sample();
        editor.setPlaySample(perf);
        char text[256];
        const std::string players = multiplayer() ? " " + trf("%u players", m_config.players) : std::string();
        const std::string sync =
            !multiplayer() ? std::string() : std::string(" | ") + (session.desynced() ? tr("Desync!") : tr("In sync"));
        snprintf(text, sizeof(text), tr("Playing%s | tick %u | sim %.2f ms/tick | moving bodies %u%s | Esc to stop"),
                 players.c_str(), perf.tick, perf.simMsPerTick, m_game->activeBodies(), sync.c_str());
        editor.setPlayStatus(text);
    }
}

void PlaySession::buildFrame(f64 now, Editor& editor, Window& window, render::Renderer& renderer,
                             asset::ModelCache& models, render::FrameParams& frame, render::DrawList& out) {
    if (!m_game || !m_match || !m_frontReady) return;
    game::GameContext ctx = m_front.context(window, renderer, models, *m_match);
    ctx.viewport = editor.viewport();
    ctx.viewTuningData = editor.viewTuning();
    ctx.scene = &editor.scene();
    m_game->buildFrame(ctx, now, frame, out);
    debug::flushDebugDraw(out, models.boxMesh(), frame, now, true); // game code's debug::line/box/text
    m_front.afterBuildFrame(frame);
}

void PlaySession::renderUi(render::Renderer& renderer) {
    if (m_game && m_match && m_frontReady) m_front.renderUi(renderer);
}

// ---------------------------------------------------------------- Join windows

// Passes only the editor-side differences to the same command line/loop as the join-mode shipping exe (game::parseGameArgs, game::runGame)
int runPlayer(int argc, char** argv) {
    log::setDialogs(false);
    std::string projectDir, viewTuningPath;
    u32 index = 2; // number used in the window title
    std::vector<char*> gameArgs{argv[0]};
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--player-window") continue;
        if (a == "--project" && i + 1 < argc) projectDir = argv[++i];
        else if (a == "--player-index" && i + 1 < argc) index = static_cast<u32>(std::atoi(argv[++i]));
        else if (a == "--view-tuning" && i + 1 < argc) viewTuningPath = argv[++i]; // copy rewritten whenever the editor edits
        else gameArgs.push_back(argv[i]);
    }
    game::MatchOptions match;
    match.port = kEditorPlayPort;
    match.inputDelayMs = kInputDelayMs;
    game::WindowOptions window;
    window.width = 960;
    window.height = 540;
    window.mute = true; // several open on the same PC (game code calls sounds the same way)
    if (!game::parseGameArgs(static_cast<int>(gameArgs.size()), gameArgs.data(), match, &window)) return 1;

    ProjectInfo project;
    std::string error;
    if (!loadProject(projectDir, project, &error)) {
        MEALKIT_ERROR("Cannot open project: %s", error.c_str());
        return 1;
    }
    setProjectRoot(project.directory);
    GameModuleHost host;
    if (!host.load(project.gameDllPath(), error)) {
        MEALKIT_ERROR("%s", error.c_str());
        return 1;
    }

    game::GameMainSpec spec;
    spec.module = host.module();
    spec.build = host.module()->build;
    game::GameRunOptions run;
    run.project = &project;
    run.title = "MealKit - " + project.name() + " - " + trf("Player %u", static_cast<unsigned>(index));
    run.captionColor = kEditorCaptionColor;
    run.viewTuningPath = viewTuningPath;
    run.playerFiles = false; // settings/rebound keys are defaults and not saved (so multiple windows on the same PC don't share one file)
    run.onlineService = false;
    run.exitWhenSessionEnds = true;
    const int exitCode = game::runGame(std::move(match), window, spec, run);
    host.unload();
    return exitCode;
}

} // namespace mealkit::editor
