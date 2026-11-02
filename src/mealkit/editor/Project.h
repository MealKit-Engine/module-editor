#pragma once

#include "mealkit/game/ProjectSettings.h"

#include <string>
#include <vector>

// Game project folder: has mealkit.project (game::ProjectSettings, mealkit/game/ProjectSettings.h) at the top level.
namespace mealkit::editor {

inline constexpr const char* kProjectFileName = game::kProjectFileName;

struct ProjectInfo : game::ProjectFile {
    std::string directory; // project folder (no trailing separator, backslashes)

    const std::string& name() const { return settings.name; }
    std::string filePath() const;        // <directory>\mealkit.project
    std::string buildRoot() const;       // folder with build.bat and build\: the project, or the engine repo for its samples\<name>
    std::string editorBuildDirectory() const; // <buildRoot()>\build\<editorBuildName()>
    std::string gameDllPath() const;     // build/<editorBuildName()>/bin/<name>_game.dll (.so, .dylib)
    std::string startScenePath() const;  // assets\<scene>
    std::string exportDirectory() const; // export\<name>
};

// This editor's build name = build.bat argument: "Editor" (DX12) or "VulkanEditor" (from build\<name>\bin where the exe is)
const std::string& editorBuildName();

// Reads directory\mealkit.project. On failure, false and a reason.
bool loadProject(const std::string& directory, ProjectInfo& out, std::string* error = nullptr);
bool isProjectDirectory(const std::string& directory);

// Per-user editor file folder (%LOCALAPPDATA%\MealKit): project list, favorites, editor settings
std::string editorUserDirectory();
bool sameProjectDirectory(const std::string& a, const std::string& b); // ignores case, slashes, trailing separator

// Project manager list (recent_projects.txt). Most recently opened first.
std::vector<std::string> loadRecentProjects();
void addRecentProject(const std::string& directory);
void removeRecentProject(const std::string& directory); // also removed from favorites
// Favorites (favorite_projects.txt). Shown at the top of the list
std::vector<std::string> loadFavoriteProjects();
void setFavoriteProject(const std::string& directory, bool favorite);
// After moving a project folder, changes the path in the list and favorites
void replaceProjectDirectory(const std::string& from, const std::string& to);

} // namespace mealkit::editor
