#pragma once

#include "mealkit/game/ProjectSettings.h"

#include <string>
#include <vector>

// 게임 프로젝트 폴더: 최상위에 mealkit.project(game::ProjectSettings, mealkit/game/ProjectSettings.h)가 있다.
namespace mealkit::editor {

inline constexpr const char* kProjectFileName = game::kProjectFileName;

struct ProjectInfo : game::ProjectFile {
    std::string directory; // 프로젝트 폴더 (끝에 구분자 없음, 역슬래시)

    const std::string& name() const { return settings.name; }
    std::string filePath() const;        // <directory>\mealkit.project
    std::string editorBuildDirectory() const; // build\<editorBuildName()>
    std::string gameDllPath() const;     // build\<editorBuildName()>\bin\<name>_game.dll
    std::string startScenePath() const;  // assets\<scene>
    std::string exportDirectory() const; // export\<name>
};

// 이 에디터의 빌드 이름 = build.bat 인자: "Editor"(DX12) 또는 "VulkanEditor" (exe가 있는 build\<이름>\bin에서)
const std::string& editorBuildName();

// directory\mealkit.project를 읽는다. 실패하면 false와 이유.
bool loadProject(const std::string& directory, ProjectInfo& out, std::string* error = nullptr);
bool isProjectDirectory(const std::string& directory);

// 사용자별 에디터 파일 폴더 (%LOCALAPPDATA%\MealKit): 프로젝트 목록, 즐겨찾기, 에디터 설정
std::string editorUserDirectory();
bool sameProjectDirectory(const std::string& a, const std::string& b); // 대소문자·슬래시·끝 구분자 무시

// 프로젝트 관리자 목록 (recent_projects.txt). 최근에 연 것이 먼저.
std::vector<std::string> loadRecentProjects();
void addRecentProject(const std::string& directory);
void removeRecentProject(const std::string& directory); // 즐겨찾기에서도 뺀다
// 즐겨찾기 (favorite_projects.txt). 목록 맨 위에 보인다
std::vector<std::string> loadFavoriteProjects();
void setFavoriteProject(const std::string& directory, bool favorite);
// 프로젝트 폴더를 옮긴 뒤 목록과 즐겨찾기의 경로를 바꾼다
void replaceProjectDirectory(const std::string& from, const std::string& to);

} // namespace mealkit::editor
