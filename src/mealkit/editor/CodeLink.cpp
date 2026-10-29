#include "mealkit/editor/CodeLink.h"

#include "mealkit/core/Log.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Process.h"

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
    // code is code.cmd, so it's called via cmd
    const bool ok = direct ? launchDetached(exe, target, "")
                           : launchDetached("cmd.exe", "/c \"\"" + exe + "\" " + target + "\"", "");
    if (!ok) MEALKIT_WARN("Could not start the code editor '%s' (Preferences > Code editor)", exe.c_str());
    return ok;
}

bool findInHeaders(const std::vector<std::string>& folders, const std::string& needle, FileLine* out) {
    for (const std::string& folder : folders) {
        if (!directoryExists(folder)) continue;
        for (const std::string& name : listFiles(folder, ".h")) {
            const std::string path = folder + "\\" + name;
            std::string text;
            if (!readTextFile(path, text)) continue;
            const size_t at = text.find(needle);
            if (at == std::string::npos) continue;
            out->file = path;
            out->line = 1;
            for (size_t i = 0; i < at; ++i) out->line += text[i] == '\n';
            return true;
        }
        std::vector<std::string> sub;
        for (const std::string& d : listDirectories(folder))
            if (d != "build" && d[0] != '.') sub.push_back(folder + "\\" + d);
        if (findInHeaders(sub, needle, out)) return true;
    }
    return false;
}

} // namespace mealkit::editor
