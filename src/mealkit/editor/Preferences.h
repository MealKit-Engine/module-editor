#pragma once

#include "mealkit/core/Types.h"

#include <string>

// 에디터 환경설정: 사용자별(%LOCALAPPDATA%\MealKit\editor_settings.txt, "키 값" 줄). 프로젝트와 무관하다.
// 같은 파일의 다른 줄(language, toolPath)은 그 기능이 읽고 쓰며, 여기서는 건드리지 않는다.
namespace mealkit::editor {

struct Preferences {
    f32 cameraSpeed = 15.0f;          // 에디터를 켤 때 카메라 비행 속도 (휠로 바꾼 값은 저장하지 않는다)
    f32 uiScale = 1.0f;               // 글자 크기 배율 (DPI 배율 위에 곱한다)
    u32 autosaveSeconds = 90;         // 저장 안 된 변경을 <씬>.autosave에 쓰는 간격 (0 = 끄기)
    std::string externalEditor = "code"; // 코드 열기에 쓸 편집기 (VS Code 방식 "-g 파일:줄")
    bool projectLayout = true;        // 창 배치를 프로젝트마다 build\editor\layout.ini에 따로 둔다
};

Preferences loadPreferences();
bool savePreferences(const Preferences& prefs);

class PreferencesPanel {
public:
    static constexpr const char* kWindow = "Preferences###Preferences";
    bool* openFlag() { return &m_open; }
    // 값이 바뀌면 true (저장까지 했다)
    bool draw(Preferences& prefs);

private:
    bool m_open = false;
    char m_editor[260] = {}; // 고치는 중인 편집기 경로
};

} // namespace mealkit::editor
