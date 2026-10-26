#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/live/Server.h"

#include <string>
#include <vector>

// 에디터 [도구] 메뉴 (도구 사이 약속, docs/CODING_MODEL.md "도구 바로가기"): 별도 저장소의 MealKit 도구를 찾아 현재 프로젝트로 연다.
// 도구 폴더마다 매니페스트 mealkit-tool.txt:
//   # MealKit tool manifest
//   @format tool 1
//   name Audio
//   exe mealkit-audio.exe       (매니페스트 폴더 기준)
//   version 0.1.0
//   protocol 1                  (라이브 연결 약속 버전 live::kProtocol)
//   opens .table .wav           (선택: 이 도구로 여는 파일 확장자)
// 찾는 곳: %LOCALAPPDATA%\MealKit\tools\<도구>\, 에디터 설정(editor_settings.txt)의 "toolPath <폴더>" 줄
// (그 폴더에 매니페스트가 있거나, 그 아래 폴더마다 있다). 여는 명령: "<exe>" --project "<프로젝트 폴더>"
namespace mealkit::editor {

inline constexpr const char* kToolManifest = "mealkit-tool.txt";

struct ToolInfo {
    std::string name, version;
    std::string exe;    // 절대 경로
    std::string folder; // 매니페스트가 있는 폴더
    u32 protocol = 0;
    std::vector<std::string> opens; // ".table" 같은 확장자
    enum class Problem : u8 { None, Protocol, MissingExe } problem = Problem::None; // None이 아니면 열 수 없다
};

// 매니페스트 하나를 읽는다. 필수 키(name, exe, protocol)가 없으면 false와 이유
bool readToolManifest(const std::string& path, ToolInfo& out, std::string* error = nullptr);
// extraFolders(명령줄 --tool-path), 기본 폴더(%LOCALAPPDATA%\MealKit\tools), 에디터 설정의 toolPath 폴더 순으로 찾는다
// (이름 순, 같은 이름은 먼저 찾은 것)
std::vector<ToolInfo> findTools(const std::vector<std::string>& extraFolders = {});
std::string defaultToolsFolder();
// 도구를 프로젝트 폴더로 연다
bool launchTool(const ToolInfo& tool, const std::string& projectDirectory);

} // namespace mealkit::editor
