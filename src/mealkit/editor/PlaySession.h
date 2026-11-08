#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/game/FrameStats.h"
#include "mealkit/game/Frontend.h"
#include "mealkit/game/GameInstance.h"
#include "mealkit/game/ProjectSettings.h"
#include "mealkit/platform/Process.h"
#include "mealkit/scene/Scene.h"

#include <memory>
#include <string>

namespace mealkit {
class Window;
namespace audio {
class Engine;
}
namespace feedback {
class Player;
}
namespace asset {
class ModelCache;
}
namespace net {
class LockstepSession;
}
namespace render {
class DrawList;
class Renderer;
} // namespace render
} // namespace mealkit

// One round of editor play: lockstep session (solo or host), join windows, game instance.
// With multiple players the editor becomes host and opens more copies of the same editor executable in join window mode (--player) (like Unity Multiplayer Play Mode).
// Join windows receive the current scene and tuning (including unsaved values) as temp files and connect with the same game DLL.
namespace mealkit::editor {

class Editor;
class GameModuleHost;

inline constexpr u16 kEditorPlayPort = 27215;
inline constexpr f64 kJoinTimeoutSeconds = 30.0;

struct PlayConfig {
    u32 players = 1;        // 1 = solo (no network)
    bool clientBots = true; // join windows are driven by bot input
    bool bot = false;       // bot input for the editor window too (automated tests)
    bool shadow = true;     // shadow simulation (determinism check every tick)
    u32 bodies = 0;         // match data (decided by the game. sandbox: 0 = number of boxes in the scene, otherwise that number)
    std::string projectDir;
    std::string replayPath; // if not empty, plays this replay (solo, input from the recording. game/InputRecord.h)
};

// Content hash computed with the same values by host and join windows (scene + models + tuning files + saved project settings)
// parts: per-component hashes (session::ContentPart order, rejection reasons), nullptr if not needed
u64 playContentHash(const scene::Scene& scene, asset::ModelCache& models, const game::GameModule& module,
                    const std::string& tuningPath, const game::ProjectFile& project, u64* parts = nullptr);

class PlaySession {
public:
    PlaySession() = default;
    ~PlaySession();
    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;

    bool start(const PlayConfig& config, Editor& editor, GameModuleHost& host, asset::ModelCache& models,
               audio::Engine& audio, feedback::Player& feedback, f64 now, std::string& error);
    void stop();
    bool active() const { return m_game != nullptr; }
    bool multiplayer() const { return m_config.players > 1; }
    bool replay() const { return !m_config.replayPath.empty(); }

    // Every frame: network, match start, game progress, tuning application, status bar
    void update(f64 now, f64 dt, Editor& editor, Window& window, render::Renderer& renderer, asset::ModelCache& models);
    void buildFrame(f64 now, Editor& editor, Window& window, render::Renderer& renderer, asset::ModelCache& models,
                    render::FrameParams& frame, render::DrawList& out);
    // After 3D, before editor UI: game screen UI
    void renderUi(render::Renderer& renderer);

    game::GameInstance* game() const { return m_game; }
    // If it can't proceed (join wait timeout, session ended), returns the reason
    const std::string& failure() const { return m_failure; }
    // The game's screen flow left the match (menu [Leave] = the editor stops play)
    bool leftByGame() const { return m_left; }

private:
    GameModuleHost* m_host = nullptr;
    audio::Engine* m_audio = nullptr;
    feedback::Player* m_feedback = nullptr;
    // View-side services (mealkit/game/Frontend.h): game view input (project bindings only), string tables, game UI (scene view area),
    // player settings (the editor window's mode isn't changed). init on the first update that receives the renderer (reread every play)
    game::Frontend m_front;
    bool m_frontReady = false;
    std::unique_ptr<game::MatchService> m_match; // session (solo: lobby without network, multiple: 127.0.0.1 host)
    game::GameInstance* m_game = nullptr;
    bool m_firstMatch = false;   // the first match was started (later ones are started by the game's lobby screen)
    bool m_loadingShown = false; // the loading screen was drawn once before match start
    bool m_left = false;
    ProcessGroup m_clients;
    PlayConfig m_config;
    game::PlayOptions m_options;
    f64 m_startTime = 0.0;
    game::FrameStats m_perf; // tick/simulation time for the play status line (takeSimMsPerTick is read only here)
    f64 m_viewTuningTimer = 0.0;
    std::string m_failure;
    std::string m_userFolder;       // where solo play recordings are kept (replays; not kept if empty)
    u32 m_keepReplays = 0;
    bool m_replayStopped = false;   // the replay diverged from the recording and paused (bug reports pause at the end tick)
    bool m_bugReport = false;       // playing a bug report zip
};

// Join window mode: MealKitEditor.exe --player-window --project <folder> [--player-index n] [--view-tuning <file>] + game options.
// Apart from the three editor options, the rest is read with the same command line as the shipping exe (game::parseGameArgs) and passed to game::runGame
int runPlayer(int argc, char** argv);

} // namespace mealkit::editor
