#pragma once

#include "mealkit/editor/Project.h"

#include <string>

// Designer mode (docs/DECISIONS.md 2026-11-04): a PC without a C++ compiler uses game code a programmer published to the folder in the
// project setting sharedBuildFolder (<name>_game.dll, <name>_headless.exe), instead of building it.
namespace mealkit::editor {

// false on a PC without Visual Studio's C++ tools. MEALKIT_NO_COMPILER=1 acts as such a PC (tests)
bool hasCompiler();
// Copies built files to the shared folder. false with a reason
bool publishSharedBuild(const ProjectInfo& project, std::string& error);
// Copies files from the shared folder when they're newer than the local ones. true if any file changed
bool fetchSharedBuild(const ProjectInfo& project, std::string* error = nullptr);

} // namespace mealkit::editor
