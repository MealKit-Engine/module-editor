#include "mealkit/editor/Project.h"

#include "mealkit/platform/Paths.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>

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

constexpr const char* kRecentFile = "recent_projects.txt";
constexpr const char* kFavoriteFile = "favorite_projects.txt";
constexpr size_t kMaxRecent = 100;

// 사용자 폴더의 목록 파일 (한 줄에 폴더 하나)
std::vector<std::string> loadList(const char* name) {
    std::vector<std::string> list;
    std::ifstream in(fromUtf8(editorUserDirectory()) / name, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (list.empty() && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3); // 메모장·PowerShell이 붙인 BOM
        line = trim(line);
        if (!line.empty()) list.push_back(line);
    }
    return list;
}

void saveList(const char* name, const std::vector<std::string>& list) {
    std::error_code ec;
    const fs::path dir = fromUtf8(editorUserDirectory());
    fs::create_directories(dir, ec);
    std::ofstream out(dir / name, std::ios::binary | std::ios::trunc);
    for (const std::string& d : list) out << d << '\n';
}

void eraseDirectory(std::vector<std::string>& list, const std::string& dir) {
    list.erase(std::remove_if(list.begin(), list.end(), [&](const std::string& d) { return sameDirectory(d, dir); }),
               list.end());
}

} // namespace

std::string ProjectInfo::filePath() const { return directory + "\\" + kProjectFileName; }

const std::string& editorBuildName() {
    // 에디터 exe는 <폴더>\build\<이름>\bin에 있다: Editor(DX12) 또는 VulkanEditor
    static const std::string name = [] {
        const std::string build = fileName(parentDirectory(executableDirectory()));
        return build == "VulkanEditor" ? build : std::string("Editor");
    }();
    return name;
}

std::string ProjectInfo::editorBuildDirectory() const { return directory + "\\build\\" + editorBuildName(); }

std::string ProjectInfo::gameDllPath() const {
    return editorBuildDirectory() + "\\bin\\" + settings.name + "_game.dll";
}

std::string ProjectInfo::startScenePath() const {
    std::string relative = settings.scene;
    std::replace(relative.begin(), relative.end(), '/', '\\');
    return directory + "\\assets\\" + relative;
}

std::string ProjectInfo::exportDirectory() const { return directory + "\\export\\" + settings.name; }

bool isProjectDirectory(const std::string& directory) {
    return !directory.empty() && fileExists(directory + "\\" + kProjectFileName);
}

bool loadProject(const std::string& directory, ProjectInfo& out, std::string* error) {
    out = ProjectInfo{};
    const std::string normalized = normalizeDirectory(directory);
    if (!game::loadProjectFile(normalized + "\\" + kProjectFileName, out)) {
        if (error) *error = std::string(kProjectFileName) + " not found in " + normalized;
        return false;
    }
    out.directory = normalized;
    if (out.settings.name.empty()) {
        if (error) *error = "mealkit.project has no name";
        return false;
    }
    game::applyTickRate(out.settings); // 튜닝·씬·표의 시간 값(초)을 이 틱레이트로 읽는다
    return true;
}

std::string editorUserDirectory() {
    return toUtf8(fromUtf8(userDataDirectory()) / "MealKit");
}

bool sameProjectDirectory(const std::string& a, const std::string& b) {
    return sameDirectory(normalizeDirectory(a), normalizeDirectory(b));
}

std::vector<std::string> loadRecentProjects() { return loadList(kRecentFile); }

void addRecentProject(const std::string& directory) {
    const std::string dir = normalizeDirectory(directory);
    std::vector<std::string> list = loadRecentProjects();
    eraseDirectory(list, dir);
    list.insert(list.begin(), dir);
    if (list.size() > kMaxRecent) list.resize(kMaxRecent);
    saveList(kRecentFile, list);
}

void removeRecentProject(const std::string& directory) {
    const std::string dir = normalizeDirectory(directory);
    std::vector<std::string> list = loadRecentProjects();
    eraseDirectory(list, dir);
    saveList(kRecentFile, list);
    setFavoriteProject(dir, false);
}

std::vector<std::string> loadFavoriteProjects() { return loadList(kFavoriteFile); }

void setFavoriteProject(const std::string& directory, bool favorite) {
    const std::string dir = normalizeDirectory(directory);
    std::vector<std::string> list = loadFavoriteProjects();
    eraseDirectory(list, dir);
    if (favorite) list.push_back(dir);
    saveList(kFavoriteFile, list);
}

void replaceProjectDirectory(const std::string& from, const std::string& to) {
    const std::string a = normalizeDirectory(from), b = normalizeDirectory(to);
    for (const char* name : {kRecentFile, kFavoriteFile}) {
        std::vector<std::string> list = loadList(name);
        for (std::string& d : list)
            if (sameDirectory(d, a)) d = b;
        saveList(name, list);
    }
}

} // namespace mealkit::editor
