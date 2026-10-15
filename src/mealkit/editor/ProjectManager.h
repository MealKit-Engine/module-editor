#pragma once

#include <string>

namespace mealkit {
class Window;
namespace render {
class Renderer;
}
} // namespace mealkit

namespace mealkit::editor {

// 프로젝트 관리자: 에디터를 프로젝트 없이 실행하면 뜬다 (최근 프로젝트, 열기, 새 프로젝트).
// 고른 프로젝트 폴더를 돌려준다. 창을 닫으면 빈 문자열.
// engineDirectory: 새 프로젝트를 만들 때 쓰는 tools\new_project.ps1이 있는 엔진 폴더.
// maxFrames/capturePath: 자동 테스트용 (그 프레임 수만큼 그리고 스크린샷을 남긴 뒤 빈 문자열을 돌려준다).
std::string runProjectManager(Window& window, render::Renderer& renderer, const std::string& engineDirectory,
                              unsigned maxFrames = 0, const std::string& capturePath = {});

} // namespace mealkit::editor
