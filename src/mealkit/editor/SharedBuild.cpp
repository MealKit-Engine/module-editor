#include "mealkit/editor/SharedBuild.h"

#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Process.h"

#include <filesystem>
#include <fstream>
#include <sstream>

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

// Which engine build this editor runs: a game DLL only works with the mealkit.dll it was built against, even when the API version is the same
std::string engineStamp() {
    std::ifstream in(fs::u8path(executableDirectory() + "\mealkit.dll"), std::ios::binary);
    unsigned long long hash = 14695981039346656037ull;
    char buffer[65536];
    while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0) {
        for (std::streamsize i = 0; i < in.gcount(); ++i) hash = (hash ^ static_cast<unsigned char>(buffer[i])) * 1099511628211ull;
    }
    return std::to_string(hash);
}

std::string readStamp(const std::string& path) {
    std::ifstream in(fs::u8path(path));
    std::string stamp;
    in >> stamp;
    return stamp;
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
    {
        std::ofstream stamp(fs::u8path(folder + "\\" + project.name() + ".engine"));
        stamp << engineStamp() << '\n';
        if (!stamp) {
            error = "cannot write the engine stamp in " + folder;
            return false;
        }
    }
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
    const std::string stamp = readStamp(folder + "\\" + project.name() + ".engine");
    if (!stamp.empty() && stamp != engineStamp()) { // loading it could crash: keep the local code and say what to do
        if (error) *error = "the shared game code was built for another engine build: get the same engine build, or ask for a new publish";
        return false;
    }
    fetch(folder + "\\" + project.name() + "_game.dll", project.gameDllPath());
    fetch(folder + "\\" + project.name() + "_headless.exe", headlessPath(project));
    if (error) *error = failure;
    return changed;
}

} // namespace mealkit::editor
