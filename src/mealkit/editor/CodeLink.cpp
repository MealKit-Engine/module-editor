#include "mealkit/editor/CodeLink.h"

#include "mealkit/core/Log.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Process.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace mealkit::editor {
namespace {

bool sourceExtension(const std::string& path) {
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* e : {".cpp", ".h", ".hpp", ".c", ".hlsl", ".hlsli", ".inl", ".cc"})
        if (ext == e) return true;
    return false;
}

// Searches backward from pos for the start of a path (stops at spaces/quotes/'['; a drive letter "D:" is inside the path)
size_t pathStart(const std::string& text, size_t end) {
    size_t i = end;
    while (i > 0) {
        const char c = text[i - 1];
        if (c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '[' || c == '>') break;
        --i;
    }
    return i;
}

// Calls visit(path, text) for each .h file under folders (skipping build and dot folders) until it returns true
template <class Visit>
bool forEachHeader(const std::vector<std::string>& folders, Visit&& visit) {
    for (const std::string& folder : folders) {
        if (!directoryExists(folder)) continue;
        for (const std::string& name : listFiles(folder, ".h")) {
            const std::string path = folder + MEALKIT_SEP + name;
            std::string text;
            if (readTextFile(path, text) && visit(path, text)) return true;
        }
        std::vector<std::string> sub;
        for (const std::string& d : listDirectories(folder))
            if (d != "build" && d[0] != '.') sub.push_back(folder + MEALKIT_SEP + d);
        if (forEachHeader(sub, visit)) return true;
    }
    return false;
}

int lineAt(const std::string& text, size_t at) {
    int line = 1;
    for (size_t i = 0; i < at; ++i) line += text[i] == '\n';
    return line;
}

bool identifierChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Start of the next "MEALKIT_STRUCT(" / "MEALKIT_OBJECT(" / "MEALKIT_VIEWMODEL(" at or after from (npos if none)
size_t nextTypeDeclaration(const std::string& text, size_t from) {
    size_t best = std::string::npos;
    for (const char* macro : {"MEALKIT_STRUCT(", "MEALKIT_OBJECT(", "MEALKIT_VIEWMODEL("}) best = std::min(best, text.find(macro, from));
    return best;
}

// Whether a MEALKIT_ macro line names field as an argument after the first: ", field," or ", field)"
bool declaresField(const std::string& line, const std::string& field) {
    const size_t open = line.find('(', line.find("MEALKIT_"));
    if (line.find("MEALKIT_") == std::string::npos || open == std::string::npos) return false;
    for (size_t at = line.find(field, open); at != std::string::npos; at = line.find(field, at + 1)) {
        size_t before = at, after = at + field.size();
        if (after < line.size() && identifierChar(line[after])) continue;
        while (before > open && (line[before - 1] == ' ' || line[before - 1] == '\t')) --before;
        while (after < line.size() && (line[after] == ' ' || line[after] == '\t')) ++after;
        if (before > open && line[before - 1] == ',' && after < line.size() && (line[after] == ',' || line[after] == ')')) return true;
    }
    return false;
}

} // namespace

bool parseFileLine(const std::string& text, FileLine* out) {
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if ((c != '(' && c != ':') || i + 1 >= text.size() || !std::isdigit(static_cast<unsigned char>(text[i + 1])))
            continue;
        const size_t start = pathStart(text, i);
        const std::string file = text.substr(start, i - start);
        if (file.empty() || !sourceExtension(file)) continue;
        out->file = file;
        out->line = std::atoi(text.c_str() + i + 1);
        return out->line > 0;
    }
    return false;
}

bool openInCodeEditor(const std::string& editor, const std::string& file, int line) {
    const std::string exe = editor.empty() ? std::string("code") : editor;
    const std::string target = "-g \"" + file + ":" + std::to_string(line) + "\"";
    const bool direct = exe.size() > 4 && exe.compare(exe.size() - 4, 4, ".exe") == 0;
#if defined(_WIN32)
    // code is code.cmd, so it's called via cmd
    const bool ok = direct ? launchDetached(exe, target, "")
                           : launchDetached("cmd.exe", "/c \"\"" + exe + "\" " + target + "\"", "");
#else
    (void)direct; // code is a shell script on PATH
    const bool ok = launchDetached(exe, target, "");
#endif
    if (!ok) MEALKIT_WARN("Could not start the code editor '%s' (Preferences > Code editor)", exe.c_str());
    return ok;
}

bool findInHeaders(const std::vector<std::string>& folders, const std::string& needle, FileLine* out) {
    return forEachHeader(folders, [&](const std::string& path, const std::string& text) {
        const size_t at = text.find(needle);
        if (at == std::string::npos) return false;
        *out = {path, lineAt(text, at)};
        return true;
    });
}

bool findFieldDeclaration(const std::vector<std::string>& folders, const std::string& typeName, const std::string& fieldName, FileLine* out) {
    return forEachHeader(folders, [&](const std::string& path, const std::string& text) {
        for (size_t type = nextTypeDeclaration(text, 0); type != std::string::npos; type = nextTypeDeclaration(text, type + 1)) {
            const size_t open = text.find('(', type), close = text.find(')', type);
            if (close == std::string::npos) return false;
            std::string name = text.substr(open + 1, close - open - 1);
            while (!name.empty() && name.front() == ' ') name.erase(name.begin());
            while (!name.empty() && name.back() == ' ') name.pop_back();
            const size_t lineStart = text.rfind('\n', type) == std::string::npos ? 0 : text.rfind('\n', type) + 1;
            const std::string before = text.substr(lineStart, type - lineStart);
            if (name != typeName || before.find("//") != std::string::npos || before.find("define") != std::string::npos) continue; // comment and macro examples
            *out = {path, lineAt(text, type)};
            const size_t end = std::min(nextTypeDeclaration(text, close), text.size());
            for (size_t line = text.find('\n', close); line != std::string::npos && line < end; line = text.find('\n', line + 1)) {
                const size_t next = text.find('\n', line + 1);
                if (declaresField(text.substr(line + 1, (next == std::string::npos ? text.size() : next) - line - 1), fieldName)) {
                    out->line = lineAt(text, line + 1);
                    break;
                }
            }
            return true;
        }
        return false;
    });
}

} // namespace mealkit::editor
