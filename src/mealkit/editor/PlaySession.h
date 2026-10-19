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
// parts: 구성별 해시 (net::ContentPart 순서, 거부 이유), 필요 없으면 nullptr
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

    // 매 프레임: 네트워크, 매치 시작, 게임 진행, 튜닝 적용, 상태 표시줄
    void update(f64 now, f64 dt, Editor& editor, Window& window, render::Renderer& renderer, asset::ModelCache& models);
    void buildFrame(f64 now, Editor& editor, Window& window, render::Renderer& renderer, asset::ModelCache& models,
                    render::FrameParams& frame, render::DrawList& out);
    // 3D 다음, 에디터 UI 전: 게임 화면 UI
    void renderUi(render::Renderer& renderer);

    game::GameInstance* game() const { return m_game; }
    // 더 진행할 수 없으면(참가 대기 시간 초과, 세션 종료) 이유를 돌려준다
    const std::string& failure() const { return m_failure; }
    // 게임의 화면 흐름이 매치를 나갔다 (메뉴의 [나가기] = 에디터는 플레이를 멈춘다)
    bool leftByGame() const { return m_left; }

private:
    GameModuleHost* m_host = nullptr;
    audio::Engine* m_audio = nullptr;
    feedback::Player* m_feedback = nullptr;
    // 화면 쪽 서비스 (mealkit/game/Frontend.h): 게임 뷰 입력(프로젝트 바인딩만), 문자열 표, 게임 UI(씬 뷰 영역),
    // 플레이어 설정(에디터 창의 모드는 바꾸지 않는다). 렌더러를 받는 첫 update에서 init (플레이마다 다시 읽는다)
    game::Frontend m_front;
    bool m_frontReady = false;
    std::unique_ptr<game::MatchService> m_match; // 세션 (혼자: 네트워크 없는 로비, 여러 명: 127.0.0.1 방장)
    game::GameInstance* m_game = nullptr;
    bool m_firstMatch = false;   // 첫 매치를 시작했다 (그다음은 게임의 로비 화면이 시작한다)
    bool m_loadingShown = false; // 매치 시작 전 로딩 화면을 한 번 그렸다
    bool m_left = false;
    ProcessGroup m_clients;
    PlayConfig m_config;
    game::PlayOptions m_options;
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
