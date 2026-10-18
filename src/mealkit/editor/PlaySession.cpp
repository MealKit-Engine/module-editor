#include "mealkit/editor/PlaySession.h"

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
#include "mealkit/game/GameData.h"
#include "mealkit/input/Actions.h"
#include "mealkit/net/Lockstep.h"
#include "mealkit/net/Socket.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Window.h"
#include "mealkit/render/DrawList.h"
#include "mealkit/render/Renderer.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace mealkit::editor {
namespace {

constexpr u32 kInputDelay = 3;

std::string playDirectory(const std::string& projectDir) { return projectDir + "\\build\\Editor\\play"; }

} // namespace

u64 playContentHash(const scene::Scene& scene, asset::ModelCache& models, const game::GameModule& module,
                    const std::string& tuningPath, const game::ProjectFile& project) {
    const reflect::TypeInfo* tuningType = module.tuningType ? module.tuningType() : nullptr;
    reflect::Instance tuning = tuningType ? reflect::Instance(*tuningType) : reflect::Instance();
    if (tuningType) game::loadTuning(*tuningType, tuning.data(), tuningPath);
    const reflect::TypeInfo* gameProjectType = module.projectSettingsType ? module.projectSettingsType() : nullptr;
    reflect::Instance gameProject = gameProjectType ? reflect::Instance(*gameProjectType) : reflect::Instance();
    if (gameProjectType) game::applyGameSettings(project, *gameProjectType, gameProject.data());
    return game::contentHash(scene, models, module.objectTypes ? module.objectTypes() : nullptr, tuningType,
                             tuning.data(), {&reflect::typeOf<game::ProjectSettings>(), &project.settings},
                             {gameProjectType, gameProject.data()}, module.tables ? module.tables() : nullptr);
}

PlaySession::~PlaySession() { stop(); }

bool PlaySession::start(const PlayConfig& config, Editor& editor, GameModuleHost& host, asset::ModelCache& models,
                        audio::Engine& audio, feedback::Player& feedback, f64 now, std::string& error) {
    stop();
    m_audio = &audio;
    m_feedback = &feedback;
    m_actions.load(); // 에디터 플레이는 프로젝트 바인딩만 쓴다 (플레이어가 바꾼 키 파일 없음)
    m_strings.load(editor.project().settings.language, editor.project().settings.language);
    m_config = config;
    m_config.players = std::clamp(config.players, 1u, net::kMaxPlayers);
    m_host = &host;
    m_failure.clear();
    m_startTime = now;
    m_seed = static_cast<u64>(std::time(nullptr));
    m_options = {};
    m_options.bot = config.bot;
    m_options.shadow = config.shadow;
    m_options.record = config.players <= 1; // 되감기는 혼자 플레이에서만 (여러 명이면 멈출 수 없다)
    m_options.tuningPath = editor.writePlayTuning();
    editor.saveTablesForPlay(); // 게임은 매치를 시작할 때 표 파일을 읽는다

    m_session = std::make_unique<net::LockstepSession>();
    if (!multiplayer()) {
        m_session->startOffline(m_seed, config.bodies);
    } else {
        // 참가 창이 읽을 씬 사본 (저장 안 된 편집 포함)
        const std::string dir = playDirectory(config.projectDir);
        createDirectories(dir);
        const std::string scenePath = dir + "\\play.scene";
        if (!scene::save(editor.scene(), scenePath)) {
            error = trf("Could not write the temporary scene: %s", scenePath.c_str());
            return false;
        }
        m_session->setContentHash(playContentHash(editor.scene(), models, *host.module(), m_options.tuningPath, editor.project()));
        const std::string viewTuningPath = editor.writePlayViewTuning(); // 참가 창은 이 사본이 바뀔 때마다 다시 읽는다
        editor.takeViewTuningChanged();
        if (!m_session->host(kEditorPlayPort, m_config.players, kInputDelay, true)) {
            error = trf("Could not open port %u (is another play session using it?)", static_cast<unsigned>(kEditorPlayPort));
            m_session.reset();
            return false;
        }
        const std::string exe = executableDirectory() + "\\MealKitEditor.exe";
        for (u32 i = 2; i <= m_config.players; ++i) {
            std::string args = "--project \"" + config.projectDir +
                               "\" --player 127.0.0.1:" + std::to_string(kEditorPlayPort) + " --player-index " +
                               std::to_string(i) + " --scene \"" + scenePath + "\"";
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
    m_game = host.create();
    if (!m_game) {
        error = tr("Could not create the game instance");
        stop();
        return false;
    }
    editor.takeTuningChanged(); // 시작할 때 이미 임시 파일로 넘겼다
    return true;
}

void PlaySession::stop() {
    if (m_audio) m_audio->stopAll(); // 게임이 낸 소리 (배경음 루프 포함)
    if (m_feedback) m_feedback->clear();
    m_clients.terminateAll();
    if (m_host && m_game) m_host->destroy(m_game);
    m_game = nullptr;
    m_session.reset();
}

void PlaySession::update(f64 now, f64 dt, Editor& editor, Window& window, render::Renderer& renderer,
                         asset::ModelCache& models) {
    if (!m_game || !m_session || !m_failure.empty()) return;
    net::LockstepSession& session = *m_session;
    session.update(now);

    // 방장: 모두 들어오면 매치 시작
    if (session.isHost() && session.state() == net::SessionState::Lobby &&
        session.lobbyPlayerCount() >= m_config.players)
        session.startMatch(m_seed, m_config.bodies);
    if (session.state() == net::SessionState::Ended) {
        m_failure = trf("The session ended: %s", session.endReason());
        return;
    }
    if (!m_game->started() && session.state() != net::SessionState::Running) {
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

    // 화면용 튜닝: 에디터 값을 그대로 넘기고, 여러 명이면 참가 창이 읽는 사본을 다시 쓴다 (0.2초에 한 번까지)
    m_viewTuningTimer += dt;
    if (multiplayer() && m_viewTuningTimer >= 0.2 && editor.takeViewTuningChanged()) {
        editor.writePlayViewTuning();
        m_viewTuningTimer = 0.0;
    }
    game::GameContext ctx{window,
                          renderer,
                          models,
                          session,
                          *m_audio,
                          *m_feedback,
                          m_actions,
                          m_strings,
                          editor.viewport(),
                          editor.gameKeyboardAllowed(),
                          editor.gameMouseAllowed(),
                          editor.viewTuning()};
    m_strings.update(dt);
    m_pads.update(dt); // 패드는 게임 뷰가 키보드를 받을 때만 (다른 패널을 고치는 동안 캐릭터가 움직이지 않게)
    m_actions.update(window.input(), ctx.keyboardAllowed ? m_pads.first() : input::PadState{}, ctx.keyboardAllowed,
                     ctx.mouseAllowed, dt);
    if (!m_game->started()) {
        m_game->start(ctx, editor.scene(), m_options);
        MEALKIT_INFO("Play mode: %u player(s), match data %u", session.playerCount(), session.matchData());
    }
    // 혼자 플레이: 튜닝을 바꾸면 바로 적용. 여러 명이면 피어마다 달라져 디싱크가 나므로 적용하지 않는다.
    if (editor.takeTuningChanged() && !multiplayer()) m_game->setTuning(editor.tuningData());
    m_game->update(ctx, dt);
    m_game->debugUi(ctx); // 에디터 ImGui 프레임 안 (게임 DLL도 mealkit.dll의 같은 ImGui를 쓴다)
    editor.setStateView(m_game->stateView());
    editor.setSimDebug(m_game->debug(), !multiplayer());

    m_statusTimer += dt;
    if (m_statusTimer >= 0.25) {
        char text[256];
        const std::string players = multiplayer() ? " " + trf("%u players", m_config.players) : std::string();
        const std::string sync =
            !multiplayer() ? std::string() : std::string(" | ") + (session.desynced() ? tr("Desync!") : tr("In sync"));
        snprintf(text, sizeof(text), tr("Playing%s | tick %u | sim %.2f ms/tick | moving bodies %u%s | Esc to stop"),
                 players.c_str(), m_game->nextTick(), m_game->takeSimMsPerTick(), m_game->activeBodies(), sync.c_str());
        editor.setPlayStatus(text);
        m_statusTimer = 0.0;
    }
}

void PlaySession::buildFrame(f64 now, Editor& editor, Window& window, render::Renderer& renderer,
                             asset::ModelCache& models, render::FrameParams& frame, render::DrawList& out) {
    if (!m_game || !m_session) return;
    game::GameContext ctx{window, renderer, models, *m_session, *m_audio, *m_feedback, m_actions, m_strings, editor.viewport()};
    ctx.viewTuningData = editor.viewTuning();
    m_game->buildFrame(ctx, now, frame, out);
    m_audio->setListener(frame.cameraPos, frame.cameraForward);
    m_feedback->screenTint(frame.tintColor, frame.tintAmount);
}

// ---------------------------------------------------------------- 참가 창

int runPlayer(const PlayerOptions& o) {
    log::setDialogs(false);
    ProjectInfo project;
    std::string error;
    if (!loadProject(o.projectDir, project, &error)) {
        MEALKIT_ERROR("Cannot open project: %s", error.c_str());
        return 1;
    }
    setProjectRoot(project.directory);

    const std::string title = "MealKit - " + project.name() + " - " + trf("Player %u", static_cast<unsigned>(o.index));
    Window window;
    if (!window.create({title.c_str(), 960, 540, kEditorCaptionColor})) return 1;
    render::Renderer renderer;
    if (!renderer.init({window.nativeHandle(), window.width(), window.height(), true, o.debugLayer})) return 1;
    asset::ModelCache models;
    models.setRenderer(&renderer);

    scene::Scene scene;
    if (!scene::load(scene, o.scenePath, &error)) {
        MEALKIT_ERROR("Cannot load scene %s: %s", o.scenePath.c_str(), error.c_str());
        return 1;
    }
    GameModuleHost host;
    if (!host.load(project.gameDllPath(), error)) {
        MEALKIT_ERROR("%s", error.c_str());
        return 1;
    }

    net::LockstepSession session;
    session.setContentHash(playContentHash(scene, models, *host.module(), o.tuningPath, project));
    net::Address address;
    if (!net::startup() || !net::resolveAddress(o.address.c_str(), kEditorPlayPort, address)) {
        MEALKIT_ERROR("Cannot resolve %s", o.address.c_str());
        return 1;
    }
    net::cleanup();
    if (!session.join(address)) return 1;

    game::LiveTuning viewTuning; // 에디터가 고칠 때마다 다시 쓰는 사본 (없으면 assets/view.tuning)
    if (host.module()->viewTuningType) viewTuning.open(*host.module()->viewTuningType(), o.viewTuningPath);

    // 참가 창은 같은 PC에서 여러 개가 뜨므로 소리를 끈 채로 시작한다 (게임 코드는 똑같이 소리를 부른다)
    audio::Engine audio;
    audio.init({project.settings.audioVoices, true});
    feedback::Player feedback(audio);
    input::Actions actions;
    actions.load();
    Gamepads pads;
    loc::Strings strings;
    strings.load(project.settings.language, project.settings.language);

    game::GameInstance* game = host.create();
    game::PlayOptions options;
    options.bot = o.bot;
    options.tuningPath = o.tuningPath;
    game::GameContext ctx{window, renderer, models, session, audio, feedback, actions, strings};
    render::DrawList drawList;
    f64 lastTime = nowSeconds();
    int exitCode = 0;
    while (window.pumpMessages()) {
        const f64 now = nowSeconds();
        const f64 dt = std::min(now - lastTime, 0.25);
        lastTime = now;
        if (window.input().keyPressed(key::Escape)) break;
        if (window.consumeResize()) renderer.resize(window.width(), window.height());
        session.update(now);
        if (session.state() == net::SessionState::Ended) {
            MEALKIT_INFO("Session ended: %s", session.endReason());
            exitCode = session.desynced() ? 2 : 0;
            break;
        }
        ctx.viewport = {0.0f, 0.0f, static_cast<f32>(window.width()), static_cast<f32>(window.height())};
        ctx.keyboardAllowed = ctx.mouseAllowed = window.focused();
        viewTuning.poll(now);
        ctx.viewTuningData = viewTuning.view();
        if (!game->started() && session.state() == net::SessionState::Running) game->start(ctx, scene, options);
        audio.update(dt);
        feedback.update(dt);
        pads.update(dt);
        strings.update(dt);
        actions.update(window.input(), window.focused() ? pads.first() : input::PadState{}, ctx.keyboardAllowed,
                       ctx.mouseAllowed, dt);
        game->update(ctx, dt);
        if (!window.minimized()) {
            render::FrameParams frame;
            drawList.clear();
            game->buildFrame(ctx, now, frame, drawList);
            audio.setListener(frame.cameraPos, frame.cameraForward);
            feedback.screenTint(frame.tintColor, frame.tintAmount);
            if (renderer.beginFrame(frame)) {
                drawList.submit(renderer);
                renderer.endFrame();
            }
        }
    }
    host.destroy(game);
    session.shutdown();
    host.unload();
    renderer.shutdown();
    return exitCode;
}

} // namespace mealkit::editor
