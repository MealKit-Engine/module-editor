#include "mealkit/editor/SharedBuild.h"

#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Process.h"

#include <filesystem>

namespace mealkit::editor {
namespace {

namespace fs = std::filesystem;

std::string headlessPath(const ProjectInfo& project) { return project.buildRoot() + "\\build\\Release\\bin\\" + project.name() + "_headless.exe"; }

// Through a temporary name, so a reader never sees half a file. Copying keeps the write time, which is how versions are compared
bool copyWhole(const std::string& from, const std::string& to, std::string& error) {
    const fs::path target = fs::u8path(to);
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    fs::path temp = target;
    temp += ".tmp";
    fs::copy_file(fs::u8path(from), temp, fs::copy_options::overwrite_existing, ec);
    if (!ec) fs::rename(temp, target, ec);
    if (ec) {
        fs::remove(temp, ec);
        error = from + " -> " + to + " failed";
        return false;
    }
    return true;
}

} // namespace

bool hasCompiler() { return hasCppCompiler(); }

bool publishSharedBuild(const ProjectInfo& project, std::string& error) {
    const std::string& folder = project.settings.sharedBuildFolder;
    if (folder.empty()) {
        error = "sharedBuildFolder is empty in the project settings";
        return false;
    }
    const std::string dll = project.gameDllPath();
    if (!fileExists(dll)) {
        error = "not built yet: " + dll;
        return false;
    }
    if (!copyWhole(dll, folder + "\\" + project.name() + "_game.dll", error)) return false;
    const std::string headless = headlessPath(project);
    return !fileExists(headless) || copyWhole(headless, folder + "\\" + project.name() + "_headless.exe", error);
}

bool fetchSharedBuild(const ProjectInfo& project, std::string* error) {
    const std::string& folder = project.settings.sharedBuildFolder;
    if (folder.empty()) return false;
    bool changed = false;
    std::string failure;
    const auto fetch = [&](const std::string& shared, const std::string& local) {
        const unsigned long long from = fileExists(shared) ? fileWriteTime(shared) : 0;
        if (from == 0 || from <= (fileExists(local) ? fileWriteTime(local) : 0)) return;
        if (copyWhole(shared, local, failure)) changed = true;
    };
    fetch(folder + "\\" + project.name() + "_game.dll", project.gameDllPath());
    fetch(folder + "\\" + project.name() + "_headless.exe", headlessPath(project));
    if (error) *error = failure;
    return changed;
}

} // namespace mealkit::editor
