#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/editor/Project.h"
#include "mealkit/platform/Process.h"

#include <string>

// 에디터 프로젝트 빌드와 [빌드 로그] 창: 프로젝트의 build.bat(게임 코드, 출시본)이나 엔진의 upgrade_project.ps1을
// 창 없이 실행하고 출력을 모은다. 빌드 전에 저장할 것과 끝난 뒤 할 일(DLL 다시 읽기, 다시 빌드)은 에디터가 정한다.
namespace mealkit::editor {

enum class BuildKind {
    GameCode, // build.bat Editor: 게임 DLL (끝나면 앱이 바꿔 끼운다)
    Export,   // build.bat Export: 출시용 exe + assets → export\<이름>
    Upgrade,  // 엔진의 tools\upgrade_project.ps1: 프로젝트 템플릿 파일을 지금 엔진 버전으로 (끝나면 게임 코드 빌드)
};

class BuildPanel {
public:
    static constexpr const char* kWindow = "Build Log###BuildLog";

    // 실행을 시작한다 (이미 빌드 중이면 false). now: 에디터 시간 (걸린 시간 표시)
    bool start(BuildKind kind, const ProjectInfo& project, const std::string& engineDirectory, f64 now);
    // 출력을 모은다. 빌드가 이번에 끝났으면 true (kind()·succeeded()로 결과를 본다)
    bool poll(const ProjectInfo& project, f64 now);
    bool running() const { return m_process.running(); }
    BuildKind kind() const { return m_kind; }
    bool succeeded() const { return m_succeeded; }

    bool* openFlag() { return &m_open; }
    // 상태 표시줄의 "빌드 중... N초" (빌드 중이 아니면 아무것도 그리지 않고 false)
    bool drawStatus(f64 now) const;
    // [빌드 로그] 창. 버튼으로 고른 빌드를 request에 넣고 true (시작은 에디터가 저장한 뒤에 한다)
    bool drawLog(bool editing, const ProjectInfo& project, f64 now, BuildKind* request);
    std::string takeStatus() { return std::move(m_status); }
    void setCodeEditor(const std::string& editor) { m_codeEditor = editor; } // 로그의 "파일(줄)"을 누르면 연다

private:
    ChildProcess m_process;
    BuildKind m_kind = BuildKind::GameCode;
    bool m_succeeded = false;
    std::string m_log;
    bool m_open = false;
    bool m_scroll = false;
    f64 m_start = 0.0;
    std::string m_status;
    std::string m_codeEditor;
};

} // namespace mealkit::editor
