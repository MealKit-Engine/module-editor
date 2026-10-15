#include "mealkit/editor/Project.h"

#include "mealkit/platform/Paths.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <windows.h>

namespace mealkit::editor {
namespace fs = std::filesystem;

namespace {

fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string toUtf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// 같은 폴더를 다른 표기(대소문자, 슬래시, 끝 구분자)로 적어도 같게 본다
std::string normalizeDirectory(const std::string& directory) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fromUtf8(directory), ec);
    if (ec) p = fromUtf8(directory);
    std::string s = toUtf8(p.make_preferred());
    while (s.size() > 3 && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    return s;
}

bool sameDirectory(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

fs::path recentFile() {
    wchar_t buffer[MAX_PATH];
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    const fs::path base = (len > 0 && len < MAX_PATH) ? fs::path(buffer) : fromUtf8(executableDirectory());
    return base / "MealKit" / "recent_projects.txt";
}

void saveRecent(const std::vector<std::string>& list) {
    std::error_code ec;
    const fs::path file = recentFile();
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    for (const std::string& d : list) out << d << '\n';
}

} // namespace

std::string ProjectInfo::gameDllPath() const { return directory + "\\build\\Editor\\bin\\" + name + "_game.dll"; }

std::string ProjectInfo::startScenePath() const {
    std::string relative = startScene;
    std::replace(relative.begin(), relative.end(), '/', '\\');
    return directory + "\\assets\\" + relative;
}

std::string ProjectInfo::exportDirectory() const { return directory + "\\export\\" + name; }

bool isProjectDirectory(const std::string& directory) {
    return !directory.empty() && fileExists(directory + "\\" + kProjectFileName);
}

bool loadProject(const std::string& directory, ProjectInfo& out, std::string* error) {
    out = ProjectInfo{};
    out.directory = normalizeDirectory(directory);
    std::ifstream in(fromUtf8(out.directory) / kProjectFileName, std::ios::binary);
    if (!in) {
        if (error) *error = std::string(kProjectFileName) + " not found in " + out.directory;
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "name") out.name = value;
        else if (key == "scene") out.startScene = value;
        else if (key == "engine") out.engineDirectory = value;
        else if (key == "engineVersion") out.engineVersion = value;
    }
    if (out.name.empty()) {
        if (error) *error = "mealkit.project has no name=";
        return false;
    }
    return true;
}

std::vector<std::string> loadRecentProjects() {
    std::vector<std::string> list;
    std::ifstream in(recentFile(), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (!line.empty()) list.push_back(line);
    }
    return list;
}

void addRecentProject(const std::string& directory) {
    const std::string dir = normalizeDirectory(directory);
    std::vector<std::string> list = loadRecentProjects();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const std::string& d) { return sameDirectory(d, dir); }),
               list.end());
    list.insert(list.begin(), dir);
    if (list.size() > 20) list.resize(20);
    saveRecent(list);
}

void removeRecentProject(const std::string& directory) {
    const std::string dir = normalizeDirectory(directory);
    std::vector<std::string> list = loadRecentProjects();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const std::string& d) { return sameDirectory(d, dir); }),
               list.end());
    saveRecent(list);
}

} // namespace mealkit::editor
