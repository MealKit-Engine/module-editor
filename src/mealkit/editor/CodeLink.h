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

// Where a reflected field is declared (inspector "Find in Code"): the MEALKIT_STRUCT/OBJECT/VIEWMODEL(typeName) line in .h files
// under folders, then the first MEALKIT_ line after it with fieldName as a later argument ("MEALKIT_FIELD(f32, speed, ...").
// The type line when the field isn't found before the next declared type. false if the type isn't found either.
bool findFieldDeclaration(const std::vector<std::string>& folders, const std::string& typeName, const std::string& fieldName, FileLine* out);

} // namespace mealkit::editor
