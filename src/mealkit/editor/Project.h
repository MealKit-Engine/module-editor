#pragma once

#include <string>
#include <vector>

// 게임 프로젝트 폴더: 최상위에 aster.project(key=value 텍스트)가 있다.
//   name=MyGame                  게임 이름 = 실행 파일·게임 DLL 이름
//   engine=D:\Aster              엔진 폴더 (build.bat, editor.bat이 읽는다. 엔진 저장소 자신은 없음)
//   scene=scenes/arena.scene     에디터가 처음 여는 씬 (assets 기준)
namespace aster::editor {

inline constexpr const char* kProjectFileName = "aster.project";

struct ProjectInfo {
    std::string name;       // 게임 이름
    std::string directory;  // 프로젝트 폴더 (끝에 구분자 없음, 역슬래시)
    std::string startScene = "scenes/arena.scene";

    std::string gameDllPath() const;   // build\Editor\bin\<name>_game.dll
    std::string startScenePath() const; // assets\<startScene>
    std::string exportDirectory() const; // export\<name>
};

// directory\aster.project를 읽는다. 실패하면 false와 이유.
bool loadProject(const std::string& directory, ProjectInfo& out, std::string* error = nullptr);
bool isProjectDirectory(const std::string& directory);

// 최근 프로젝트 (%LOCALAPPDATA%\Aster\recent_projects.txt). 최근 것이 먼저.
std::vector<std::string> loadRecentProjects();
void addRecentProject(const std::string& directory);
void removeRecentProject(const std::string& directory);

} // namespace aster::editor
