#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/editor/Project.h"

#include <string>

// 에디터 [프로젝트 설정] 창 (mealkit.project): 엔진 항목(game::ProjectSettings)과 게임이 더한 항목(GameModule::projectSettingsType)을 고친다.
// 디스크에 저장된 값(ProjectInfo)은 에디터가 갖고, 이 창은 고치는 중인 값만 갖는다. 저장하면 넘겨받은 ProjectInfo를 바꾼다.
namespace mealkit::editor {

class ProjectSettingsPanel {
public:
    // 게임이 더한 프로젝트 설정 타입 (DLL을 내리기 전에 nullptr). 값은 텍스트로 보존한다.
    void setGameType(const reflect::TypeInfo* type);
    // 고치는 값을 디스크의 값으로 되돌린다 (프로젝트를 열 때도)
    void revert(const ProjectInfo& project);
    bool save(ProjectInfo& project); // 실패하면 false (상태 문구는 takeStatus)
    // 시작 씬만 바꿔 바로 저장한다 ([씬] 창). 창에서 고치는 중인 다른 값은 저장하지 않는다
    bool setStartScene(ProjectInfo& project, const std::string& scene);
    // 도구가 관리하는 값(engine, engineVersion)만 다시 받는다 (업그레이드 뒤)
    void setToolValues(const game::ProjectSettings& from) {
        m_edit.engine = from.engine;
        m_edit.engineVersion = from.engineVersion;
    }

    void open() { m_open = true; }
    bool* openFlag() { return &m_open; }
    bool dirty() const { return m_dirty; }
    // currentScene: assets 기준 지금 씬 경로 ([지금 씬을 시작 씬으로], 없으면 빈 문자열)
    void draw(ProjectInfo& project, const std::string& currentScene);
    std::string takeStatus() { return std::move(m_status); }

private:
    game::ProjectSettings m_edit;   // 창에서 고치는 값
    reflect::Instance m_game;       // 게임 DLL의 프로젝트 설정 타입 인스턴스
    reflect::TextFields m_gameText; // 게임 타입이 없을 때(DLL을 바꿔 끼울 때) 값을 보존한다
    bool m_dirty = false;
    bool m_open = false;
    std::string m_status;
};

} // namespace mealkit::editor
