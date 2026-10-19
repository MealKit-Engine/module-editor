#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/data/Tables.h"

#include <string>
#include <vector>

// 에디터 [튜닝] 패널: 게임 튜닝(assets/game.tuning, 시뮬레이션·콘텐츠 해시)과 화면용 튜닝(assets/view.tuning)을
// 게임 DLL이 선언한 타입으로 고친다. 값의 인스턴스는 DLL의 타입이라, DLL을 내리기 전에 텍스트로 보존했다가 새 타입에 다시 넣는다.
namespace mealkit::editor {

class TuningPanel {
public:
    static constexpr const char* kWindow = "Tuning###Tuning";
    static constexpr const char* kPlayViewTuningFile = "play.view.tuning";

    void setProjectDirectory(const std::string& directory) { m_projectDir = directory; }
    // 게임 튜닝 타입 (DLL을 내리기 전에 nullptr). 처음 받으면 파일에서 읽는다.
    void setTuningType(const reflect::TypeInfo* type) { setDocType(m_tuning, type); }
    // 화면용 튜닝 (GameModule::viewTuningType)
    void setViewTuningType(const reflect::TypeInfo* type) { setDocType(m_viewTuning, type); }

    // 플레이를 시작할 때: 저장 안 된 튜닝 값을 임시 파일로 써서 경로를 돌려준다 (타입이 없으면 빈 문자열 = 기본 파일)
    std::string writePlayTuning() { return writePlayCopy(m_tuning, "play.tuning"); }
    // 참가 창이 읽는 화면용 튜닝 사본 (여러 명 플레이를 시작할 때 쓰고, 값이 바뀌면 다시 쓴다)
    std::string writePlayViewTuning() { return writePlayCopy(m_viewTuning, kPlayViewTuningFile); }
    // 바뀌었으면 한 번 true (플레이 중인 게임에 넘긴다)
    bool takeTuningChanged() { return takeChanged(m_tuning); }
    bool takeViewTuningChanged() { return takeChanged(m_viewTuning); }
    reflect::View tuning() const { return {m_tuning.value.type(), m_tuning.value.data()}; }
    reflect::View viewTuning() const { return {m_viewTuning.value.type(), m_viewTuning.value.data()}; }

    // 게임 튜닝의 행 참조를 지금 표로 풀고, 없는 행 참조를 problems에 더한다 ([표] 패널 목록)
    void resolveRows(const data::TableIndexList& index, std::vector<std::string>* problems);

    bool dirty() const { return m_tuning.dirty || m_viewTuning.dirty; }
    void saveDirty(); // 고친 파일만 저장한다
    // dockWith: 처음 열 때 탭으로 붙일 창. playing·multiplayer는 "바로 적용" 안내에 쓴다.
    void draw(const char* dockWith, bool playing, bool multiplayer, const data::TableIndexList& index);
    // 상태 표시줄에 띄울 문구 (없으면 빈 문자열)
    std::string takeStatus() { return std::move(m_status); }

private:
    // 튜닝 파일 하나
    struct Doc {
        const char* file = ""; // assets 안의 파일 이름
        reflect::Instance value;
        reflect::TextFields text; // DLL을 바꿔 끼울 때 값을 보존한다
        bool loaded = false;
        bool dirty = false;   // 파일과 다르다
        bool changed = false; // 플레이 중인 게임에 아직 넘기지 않았다
    };

    void setDocType(Doc& doc, const reflect::TypeInfo* type);
    std::string docPath(const Doc& doc) const;
    bool load(Doc& doc);
    bool save(Doc& doc);
    std::string writePlayCopy(const Doc& doc, const char* fileName);
    static bool takeChanged(Doc& doc);
    void drawDoc(Doc& doc, bool liveInMultiplayer, bool playing, bool multiplayer, const data::TableIndexList& index);

    std::string m_projectDir;
    Doc m_tuning{"game.tuning"};     // 게임 튜닝 (시뮬레이션, 콘텐츠 해시)
    Doc m_viewTuning{"view.tuning"}; // 화면용 튜닝
    std::string m_status;
};

} // namespace mealkit::editor
