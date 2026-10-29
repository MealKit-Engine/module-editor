#pragma once

#include <string>
#include <vector>

// Finds "file:line" in log lines and opens it in the code editor (build log, [Log] window, inspector "Open code").
namespace mealkit::editor {

struct FileLine {
    std::string file; // as written in the log (absolute path or relative to the working folder)
    int line = 0;
};

// MSVC "D:\x\a.cpp(27): error", clang/generic "src/a.cpp:27:5" shapes. Source extensions only (.cpp .h .hpp .c .hlsl .hlsli .inl)
bool parseFileLine(const std::string& text, FileLine* out);

// Opens in the code editor from preferences. VS Code style "<editor> -g file:line" (called via cmd if not an .exe)
bool openInCodeEditor(const std::string& editor, const std::string& file, int line);

// First line containing needle in .h files under folders (including subfolders)
bool findInHeaders(const std::vector<std::string>& folders, const std::string& needle, FileLine* out);

} // namespace mealkit::editor
