#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/live/Server.h"

#include <mkcontract/Tool.h>

#include <string>
#include <vector>

// Editor [Tools] menu (contract between tools, docs/CODING_MODEL.md "Tool shortcuts"): finds MealKit tools from separate repos and opens them with the current project.
// Manifests (mealkit-tool.txt), search locations and the open command are MealKit-Contract's (mkcontract/Tool.h, its docs/CONTRACT.md "Tool manifest"):
//   --tool-path folders, %LOCALAPPDATA%\MealKit\tools\<tool>\, "toolPath <folder>" lines in editor settings. Open: "<exe>" --project "<project folder>"
namespace mealkit::editor {

struct ToolInfo : mkcontract::ToolManifest {
    enum class Problem : u8 { None, Protocol, MissingExe } problem = Problem::None; // can't be opened unless None
};

// Searches extraFolders (command line --tool-path), the default folder, then toolPath folders from editor settings
// (sorted by name; for the same name, the first found). Unreadable manifests and their warnings go to the log
std::vector<ToolInfo> findTools(const std::vector<std::string>& extraFolders = {});
std::string defaultToolsFolder();
// Opens the tool with the project folder
bool launchTool(const ToolInfo& tool, const std::string& projectDirectory);

} // namespace mealkit::editor
