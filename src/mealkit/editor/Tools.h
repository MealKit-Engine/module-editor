#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/live/Server.h"

#include <string>
#include <vector>

// Editor [Tools] menu (contract between tools, docs/CODING_MODEL.md "Tool shortcuts"): finds MealKit tools from separate repos and opens them with the current project.
// Each tool folder has a manifest mealkit-tool.txt:
//   # MealKit tool manifest
//   @format tool 1
//   name Audio
//   exe mealkit-audio.exe       (relative to the manifest folder)
//   version 0.1.0
//   protocol 1                  (live connection contract version live::kProtocol)
//   opens .table .wav           (optional: file extensions this tool opens)
// Search locations: %LOCALAPPDATA%\MealKit\tools\<tool>\, "toolPath <folder>" lines in editor settings (editor_settings.txt)
// (that folder has a manifest, or each folder under it does). Open command: "<exe>" --project "<project folder>"
namespace mealkit::editor {

inline constexpr const char* kToolManifest = "mealkit-tool.txt";

struct ToolInfo {
    std::string name, version;
    std::string exe;    // absolute path
    std::string folder; // folder containing the manifest
    u32 protocol = 0;
    std::vector<std::string> opens; // extensions like ".table"
    enum class Problem : u8 { None, Protocol, MissingExe } problem = Problem::None; // can't be opened unless None
};

// Reads one manifest. false and a reason if required keys (name, exe, protocol) are missing
bool readToolManifest(const std::string& path, ToolInfo& out, std::string* error = nullptr);
// Searches extraFolders (command line --tool-path), the default folder (%LOCALAPPDATA%\MealKit\tools), then toolPath folders from editor settings
// (sorted by name; for the same name, the first found)
std::vector<ToolInfo> findTools(const std::vector<std::string>& extraFolders = {});
std::string defaultToolsFolder();
// Opens the tool with the project folder
bool launchTool(const ToolInfo& tool, const std::string& projectDirectory);

} // namespace mealkit::editor
