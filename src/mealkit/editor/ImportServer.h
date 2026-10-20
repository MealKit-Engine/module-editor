#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/core/Types.h"
#include "mealkit/net/Socket.h"

#include <string>
#include <vector>

// Penpot 가져오기 서버 (ROADMAP "게임 화면 UI 2부"): 에디터가 켜져 있는 동안 127.0.0.1에 작은 HTTP 서버를 연다.
//   GET  /<토큰>/manifest.json, plugin.js, ui.html, icon.png   Penpot 플러그인 파일 (엔진 tools/penpot-plugin/dist)
//   GET  /<토큰>/api/project                                   프로젝트 이름, 글꼴, 뷰 모델 필드, .ui에 쓰인 액션 (플러그인 드롭다운)
//   POST /<토큰>/api/export                                    받은 .ui·PNG·문자열·테마·Penpot 원본·배치 상자를 프로젝트에 쓴다
// 보안: 127.0.0.1에만 열고, 에디터를 켤 때마다 무작위 토큰을 경로에 넣는다(플러그인 설치 주소에 들어 있다).
// Origin이 있으면 Penpot(https://*.penpot.app, 직접 띄운 localhost)이나 이 서버여야 하고, 쓰기(POST)는 이 서버의 패널만.
// 쓰는 경로는 assets/ui/ 아래 .ui·.png, art/ui/ 아래 .penpot·boxes/*.boxes·preview/*.png, 프로젝트 기본 언어의 문자열 표뿐이다(".." 거절).
namespace mealkit::editor {

inline constexpr u16 kImportPort = 27220; // 쓰고 있으면 다음 9개 포트

struct ImportProject {
    std::string engineDir;  // 플러그인 파일 (tools/penpot-plugin/dist)
    std::string projectDir; // 받은 파일을 쓸 곳
    std::string name;
    std::string language; // 새 문자열 키를 더할 표 (assets/strings/<language>.txt)
};

class ImportServer {
public:
    ImportServer() = default;
    ~ImportServer();
    ImportServer(const ImportServer&) = delete;
    ImportServer& operator=(const ImportServer&) = delete;

    // token이 비어 있으면 무작위 (자동 시험은 --penpot-token으로 정한다)
    bool start(const ImportProject& project, const std::string& token = {});
    void stop();
    bool running() const { return m_listener.isOpen(); }
    void setViewModels(const std::vector<const reflect::TypeInfo*>* types) { m_viewModels = types; }
    void update(); // 매 프레임: 연결 받기, 요청 읽기, 응답 보내기 (논블로킹)

    std::string pluginUrl() const; // Penpot 플러그인 관리자에 붙여 넣는 주소
    // 가져오기가 끝날 때마다 한 줄 (상태 표시줄)
    std::vector<std::string> takeEvents();

    // 요청 처리 (소켓 없이 시험할 수 있게 공개)
    struct Request {
        std::string method, path, origin;
        std::string body;
        bool privateNetworkPreflight = false;
    };
    struct Response {
        int status = 200;
        std::string contentType = "application/json";
        std::string body;
        std::string allowOrigin; // 비어 있지 않으면 CORS 헤더
        bool privateNetwork = false;
    };
    Response handle(const Request& request);

private:
    struct Connection {
        net::TcpStream stream;
        std::string in, out;
        size_t sent = 0;
        bool responded = false;
        u32 idleFrames = 0; // 오래 조용한 연결은 닫는다
    };
    bool originAllowed(const std::string& origin) const;
    Response serveFile(const std::string& name) const;
    Response project() const;
    Response importFiles(const std::string& body);

    ImportProject m_project;
    std::string m_token;
    net::TcpListener m_listener;
    std::vector<Connection> m_connections;
    const std::vector<const reflect::TypeInfo*>* m_viewModels = nullptr;
    std::vector<std::string> m_events;
};

} // namespace mealkit::editor
