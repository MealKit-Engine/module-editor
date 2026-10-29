#include "mealkit/editor/Tools.h"

#include "mealkit/core/Log.h"
#include "mealkit/core/TextFormat.h"
#include "mealkit/editor/Project.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Process.h"

#include <algorithm>
#include <cstdlib>

namespace mealkit::editor {
namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// "key value" lines (# comments and @format lines are skipped)
template <class F>
void forEachLine(const std::string& text, F&& f) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3); // BOM added by Notepad
        if (line.empty() || line[0] == '#') continue;
        const size_t space = line.find(' ');
        f(line, line.substr(0, space), space == std::string::npos ? std::string() : trim(line.substr(space + 1)));
    }
}

// toolPath lines in editor settings
std::vector<std::string> settingsToolPaths() {
    std::vector<std::string> out;
    std::string text;
    if (!readTextFile(editorUserDirectory() + "\\editor_settings.txt", text)) return out;
    forEachLine(text, [&](const std::string&, const std::string& key, const std::string& value) {
        if (key == "toolPath" && !value.empty()) out.push_back(value);
    });
    return out;
}

void addTool(const std::string& folder, std::vector<ToolInfo>& out) {
    const std::string manifest = folder + "\\" + kToolManifest;
    if (!fileExists(manifest)) return;
    ToolInfo t;
    std::string error;
    if (!readToolManifest(manifest, t, &error)) {
        MEALKIT_WARN("Tool manifest %s: %s", manifest.c_str(), error.c_str());
        return;
    }
    for (const ToolInfo& o : out)
        if (o.name == t.name) return; // the first one found wins
    out.push_back(std::move(t));
}

} // namespace

bool readToolManifest(const std::string& path, ToolInfo& out, std::string* error) {
    out = {};
    std::string content;
    if (!readTextFile(path, content)) {
        if (error) *error = "cannot read the file";
        return false;
    }
    out.folder = parentDirectory(path);
    std::string exe;
    forEachLine(content, [&](const std::string& line, const std::string& key, const std::string& value) {
        u32 version = 1; // only version 1 for now, so no migrate
        if (text::readFormatLine(line, text::kTool, path, version)) return;
        if (key == "name") out.name = value;
        else if (key == "exe") exe = value;
        else if (key == "version") out.version = value;
        else if (key == "protocol") out.protocol = static_cast<u32>(std::strtoul(value.c_str(), nullptr, 10));
        else if (key == "opens") {
            size_t a = 0;
            while (a < value.size()) {
                size_t b = value.find(' ', a);
                if (b == std::string::npos) b = value.size();
                if (b > a) out.opens.push_back(value.substr(a, b - a));
                a = b + 1;
            }
        } else MEALKIT_WARN("%s: unknown key '%s' ignored", path.c_str(), key.c_str());
    });
    if (out.name.empty() || exe.empty() || out.protocol == 0) {
        if (error) *error = "needs name, exe and protocol";
        return false;
    }
    const bool absolute = exe.size() > 1 && (exe[1] == ':' || exe[0] == '\\' || exe[0] == '/');
    out.exe = absolute ? exe : out.folder + "\\" + exe;
    if (out.protocol != live::kProtocol) out.problem = ToolInfo::Problem::Protocol;
    else if (!fileExists(out.exe)) out.problem = ToolInfo::Problem::MissingExe;
    return true;
}

std::string defaultToolsFolder() { return editorUserDirectory() + "\\tools"; }

std::vector<ToolInfo> findTools(const std::vector<std::string>& extraFolders) {
    std::vector<ToolInfo> out;
    std::vector<std::string> roots = extraFolders;
    roots.push_back(defaultToolsFolder());
    for (const std::string& p : settingsToolPaths()) roots.push_back(p);
    for (const std::string& root : roots) {
        addTool(root, out); // the folder itself is a tool
        for (const std::string& d : listDirectories(root)) addTool(root + "\\" + d, out);
    }
    std::sort(out.begin(), out.end(), [](const ToolInfo& a, const ToolInfo& b) { return a.name < b.name; });
    return out;
}

bool launchTool(const ToolInfo& tool, const std::string& projectDirectory) {
    if (tool.problem != ToolInfo::Problem::None) return false;
    MEALKIT_INFO("Opening tool %s %s: %s", tool.name.c_str(), tool.version.c_str(), tool.exe.c_str());
    return launchDetached(tool.exe, "--project \"" + projectDirectory + "\"", tool.folder);
}

} // namespace mealkit::editor
