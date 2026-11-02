#include "mealkit/editor/Project.h"

#include "mealkit/platform/DynamicLibrary.h"
#include "mealkit/platform/Paths.h"

#include <mkcontract/Files.h>
#include <mkcontract/User.h>

#include <algorithm>

namespace mealkit::editor {

using mkcontract::normalizeDirectory;

std::string ProjectInfo::filePath() const { return directory + "/" + kProjectFileName; }

const std::string& editorBuildName() {
    // The editor exe is in <folder>\build\<name>\bin: Editor (DX12) or VulkanEditor
    static const std::string name = [] {
        const std::string build = fileName(parentDirectory(executableDirectory()));
        return build == "VulkanEditor" ? build : std::string("Editor");
    }();
    return name;
}

std::string ProjectInfo::buildRoot() const {
    // The engine repo's reference samples (samples\<name>) have no build.bat: the engine build compiles them all
    if (fileExists(directory + "/build.bat")) return directory;
    const std::string samples = parentDirectory(directory);
    const std::string engine = parentDirectory(samples);
    if (fileName(samples) == "samples" && fileExists(engine + "/build.bat") && directoryExists(engine + "/engine/src/mealkit"))
        return engine;
    return directory;
}

std::string ProjectInfo::editorBuildDirectory() const { return buildRoot() + "/build/" + editorBuildName(); }

std::string ProjectInfo::gameDllPath() const {
    return editorBuildDirectory() + "/bin/" + settings.name + "_game" + libraryExtension();
}

std::string ProjectInfo::startScenePath() const {
    std::string relative = assetFilePath(settings.scene);
    std::replace(relative.begin(), relative.end(), '/', '\\');
    return directory + "/" + relative;
}

std::string ProjectInfo::exportDirectory() const { return directory + "/export/" + settings.name; }

bool isProjectDirectory(const std::string& directory) {
    return !directory.empty() && fileExists(directory + "/" + kProjectFileName);
}

bool loadProject(const std::string& directory, ProjectInfo& out, std::string* error) {
    out = ProjectInfo{};
    const std::string normalized = normalizeDirectory(directory);
    if (!game::loadProjectFile(normalized + "/" + kProjectFileName, out)) {
        if (error) *error = std::string(kProjectFileName) + " not found in " + normalized;
        return false;
    }
    out.directory = normalized;
    if (out.settings.name.empty()) {
        if (error) *error = "mealkit.project has no name";
        return false;
    }
    game::applyTickRate(out.settings); // time values (seconds) in tuning/scenes/tables are read with this tick rate
    return true;
}

std::string editorUserDirectory() {
    const std::string user = mkcontract::userDirectory(); // the contract's folder (MEALKIT_USER_DIR in tests)
    return user.empty() ? executableDirectory() + "/MealKit" : user;
}

bool sameProjectDirectory(const std::string& a, const std::string& b) { return mkcontract::sameDirectory(a, b); }

// Lists shared with MealKit Manager (MealKit-Contract docs/CONTRACT.md "User folder")
std::vector<std::string> loadRecentProjects() { return mkcontract::recentProjects(); }
void addRecentProject(const std::string& directory) { mkcontract::addRecentProject(directory); }
void removeRecentProject(const std::string& directory) { mkcontract::removeRecentProject(directory); }
std::vector<std::string> loadFavoriteProjects() { return mkcontract::favoriteProjects(); }
void setFavoriteProject(const std::string& directory, bool favorite) { mkcontract::setFavoriteProject(directory, favorite); }
void replaceProjectDirectory(const std::string& from, const std::string& to) { mkcontract::replaceProjectDirectory(from, to); }

} // namespace mealkit::editor
