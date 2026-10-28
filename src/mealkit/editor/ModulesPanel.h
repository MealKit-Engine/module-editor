#pragma once

#include "mealkit/editor/Project.h"
#include "mealkit/platform/Process.h"

#include <string>
#include <vector>

// 에디터 [모듈] 창 (docs/MODULES.md): 프로젝트 modules/의 소스 모듈과 레지스트리(MealKit-Modules) 목록.
// 추가·갱신·제거는 엔진의 tools\module.ps1을 창 없이 부른다(규칙은 그 스크립트 한 곳). 목록은 `module.ps1 search -Tsv` 한 번으로 받는다.
// 모듈을 바꾸면 게임 코드를 다시 빌드해야 하므로 takeChanged()로 에디터에 알린다.
namespace mealkit::editor {

struct InstalledModule {
    std::string name, version, source;
    int modified = 0; // 설치한 뒤 게임이 고친 파일 수
    int pending = 0;  // 갱신이 남긴 .new 파일 수
};

struct RegistryModule {
    std::string name;
    std::string version;       // 이 엔진 MINOR용 가장 새 버전 (없으면 빈 칸)
    std::string latest;        // 가장 새 버전
    std::string latestEngine;  // 그 버전의 엔진 MAJOR.MINOR
    std::string title, summary, tags, author, repo;
};

struct ModuleList {
    std::vector<InstalledModule> installed;
    std::vector<RegistryModule> registry;
    std::string error; // 레지스트리 목록을 받지 못한 이유
};

// `module.ps1 search -Tsv`의 출력 (탭으로 나눈 줄, 모르는 줄은 건너뛴다)
ModuleList parseModuleList(const std::string& output);

class ModulesPanel {
public:
    static constexpr const char* kWindow = "Modules###Modules";

    void open() { m_open = true; }
    bool* openFlag() { return &m_open; }
    // editing: 편집 모드인가 (플레이 중에는 모듈을 바꾸지 않는다)
    void draw(const ProjectInfo& project, const std::string& engineDirectory, bool editing);
    // 모듈을 더하거나 바꾸거나 뺐다 (에디터가 게임 코드를 다시 빌드한다)
    bool takeChanged() {
        const bool changed = m_changed;
        m_changed = false;
        return changed;
    }
    std::string takeStatus() { return std::move(m_status); }

private:
    void run(const std::string& engineDirectory, const ProjectInfo& project, const std::string& label,
             const std::string& arguments, bool changes);
    void poll();

    ChildProcess m_process;
    std::string m_output;     // 지금 도는 명령의 출력
    std::string m_label;      // 지금 도는 명령 (상태 문구)
    bool m_listing = false;   // 지금 도는 명령이 목록 받기인가
    bool m_changes = false;   // 지금 도는 명령이 모듈을 바꾸나
    bool m_needList = true;   // 다음 프레임에 목록을 받는다
    std::string m_projectDir; // 목록을 받은 프로젝트 (바뀌면 다시 받는다)
    ModuleList m_list;
    std::string m_log;        // 마지막 add/update/remove 출력
    bool m_logFailed = false;
    char m_search[96] = {};
    std::string m_confirmRemove;
    bool m_open = false;
    bool m_changed = false;
    std::string m_status;
};

} // namespace mealkit::editor
