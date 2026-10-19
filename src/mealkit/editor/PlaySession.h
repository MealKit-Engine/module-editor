#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/game/FrameStats.h"
#include "mealkit/game/GameInstance.h"
#include "mealkit/game/ProjectSettings.h"
#include "mealkit/input/Actions.h"
#include "mealkit/loc/Strings.h"
#include "mealkit/settings/Settings.h"
#include "mealkit/ui/Ui.h"
#include "mealkit/platform/Gamepad.h"
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

// 에디터의 플레이 한 판: 락스텝 세션(혼자 또는 방장), 참가 창, 게임 인스턴스.
// 여러 명이면 에디터가 방장이 되고, 같은 에디터 실행 파일을 참가 창 모드(--player)로 더 띄운다(Unity Multiplayer Play Mode에 해당).
// 참가 창은 현재 씬과 튜닝(저장 안 된 값 포함)을 임시 파일로 받아 같은 게임 DLL로 접속한다.
namespace mealkit::editor {

class Editor;
class GameModuleHost;

inline constexpr u16 kEditorPlayPort = 27215;
inline constexpr f64 kJoinTimeoutSeconds = 30.0;

struct PlayConfig {
    u32 players = 1;        // 1 = 혼자 (네트워크 없음)
    bool clientBots = true; // 참가 창을 봇 입력으로 움직인다
    bool bot = false;       // 에디터 창도 봇 입력 (자동 테스트)
    bool shadow = true;     // 그림자 시뮬레이션 (매 틱 결정론 검사)
    u32 bodies = 0;         // 매치 데이터 (게임이 정한다. sandbox: 0 = 씬의 상자 수, 아니면 그 수로)
    std::string projectDir;
};

// 방장과 참가 창이 같은 값으로 만드는 콘텐츠 해시 (씬 + 모델 + 튜닝 파일 + 저장된 프로젝트 설정)
u64 playContentHash(const scene::Scene& scene, asset::ModelCache& models, const game::GameModule& module,
                    const std::string& tuningPath, const game::ProjectFile& project);

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

    // 매 프레임: 네트워크, 매치 시작, 게임 진행, 튜닝 적용, 상태 표시줄
    void update(f64 now, f64 dt, Editor& editor, Window& window, render::Renderer& renderer, asset::ModelCache& models);
    void buildFrame(f64 now, Editor& editor, Window& window, render::Renderer& renderer, asset::ModelCache& models,
                    render::FrameParams& frame, render::DrawList& out);
    // 3D 다음, 에디터 UI 전: 게임 화면 UI
    void renderUi(render::Renderer& renderer);

    game::GameInstance* game() const { return m_game; }
    // 더 진행할 수 없으면(참가 대기 시간 초과, 세션 종료) 이유를 돌려준다
    const std::string& failure() const { return m_failure; }

private:
    GameModuleHost* m_host = nullptr;
    audio::Engine* m_audio = nullptr;
    feedback::Player* m_feedback = nullptr;
    input::Actions m_actions; // 에디터 창의 게임 뷰 입력 (assets/input.bindings, 플레이 중에 고치면 다시 읽는다)
    Gamepads m_pads;
    loc::Strings m_strings; // 게임 문자열 표 (프로젝트 기본 언어, 플레이 중에 고치면 다시 읽는다)
    ui::System m_ui;        // 게임 화면 UI (씬 뷰 영역에 그린다)
    bool m_uiReady = false;
    settings::Service m_settings; // 플레이어 설정 (에디터 창의 모드는 바꾸지 않는다)
    std::unique_ptr<net::LockstepSession> m_session;
    game::GameInstance* m_game = nullptr;
    ProcessGroup m_clients;
    PlayConfig m_config;
    game::PlayOptions m_options;
    u64 m_seed = 0;
    f64 m_startTime = 0.0;
    game::FrameStats m_perf; // 플레이 상태 줄의 틱·시뮬레이션 시간 (takeSimMsPerTick은 여기서만 읽는다)
    f64 m_viewTuningTimer = 0.0;
    std::string m_failure;
};

// 참가 창 모드 (MealKitEditor.exe --project <폴더> --player <주소:포트> --scene <파일> --tuning <파일> [--bot])
struct PlayerOptions {
    std::string projectDir;
    std::string address;
    std::string scenePath;
    std::string tuningPath;
    std::string viewTuningPath; // 에디터가 고칠 때마다 다시 쓰는 화면용 튜닝 사본 (바뀌면 다시 읽는다)
    u32 index = 2; // 창 제목에 쓰는 번호
    bool bot = false;
    bool debugLayer = false;
};
int runPlayer(const PlayerOptions& options);

} // namespace mealkit::editor
