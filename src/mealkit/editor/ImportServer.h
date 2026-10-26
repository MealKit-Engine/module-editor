#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/core/Types.h"
#include "mealkit/live/Server.h"

#include <string>
#include <vector>

// Penpot 가져오기 (ROADMAP "게임 화면 UI 2부"): 에디터 라이브 연결 서버(mealkit/live/Server.h)의 penpot/ 서비스.
//   GET  /<토큰>/penpot/manifest.json, plugin.js, ui.html, icon.png   Penpot 플러그인 파일 (엔진 tools/penpot-plugin/dist)
//   GET  /<토큰>/penpot/api/project                                   프로젝트 이름, 글꼴, 뷰 모델 필드, .ui에 쓰인 액션 (플러그인 드롭다운)
//   POST /<토큰>/penpot/api/export                                    받은 .ui·PNG·문자열·테마·Penpot 원본·배치 상자를 프로젝트에 쓴다
// 보안: 서버가 127.0.0.1·실행마다 무작위 토큰으로 지킨다(플러그인 설치 주소에 들어 있다).
// Origin은 Penpot(https://*.penpot.app, 직접 띄운 localhost)이나 이 서버여야 하고, 쓰기(POST)는 이 서버의 패널만.
// 쓰는 경로는 assets/ui/ 아래 .ui·.png, art/ui/ 아래 .penpot·boxes/*.boxes·preview/*.png, 프로젝트 기본 언어의 문자열 표뿐이다(".." 거절).
namespace mealkit::editor {

struct ImportProject {
    std::string engineDir;  // 플러그인 파일 (tools/penpot-plugin/dist)
    std::string projectDir; // 받은 파일을 쓸 곳
    std::string name;
    std::string language; // 새 문자열 키를 더할 표 (assets/strings/<language>.txt)
};

class ImportServer {
public:
    // server에 penpot/ 서비스를 더한다 (서버가 먼저 열려 있어야 주소가 있다)
    void attach(live::Server& server, const ImportProject& project);
    void setViewModels(const std::vector<const reflect::TypeInfo*>* types) { m_viewModels = types; }

    std::string pluginUrl() const; // Penpot 플러그인 관리자에 붙여 넣는 주소
    // 가져오기가 끝날 때마다 한 줄 (상태 표시줄)
    std::vector<std::string> takeEvents();

    live::Response handle(const live::Request& request); // 서비스 안의 경로 (소켓 없이 시험할 수 있게 공개)

private:
    live::Response serveFile(const std::string& name) const;
    live::Response project() const;
    live::Response importFiles(const std::string& body);

    live::Server* m_server = nullptr;
    ImportProject m_project;
    const std::vector<const reflect::TypeInfo*>* m_viewModels = nullptr;
    std::vector<std::string> m_events;
};

} // namespace mealkit::editor
