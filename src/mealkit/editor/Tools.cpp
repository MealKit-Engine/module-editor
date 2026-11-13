#include "mealkit/editor/Tools.h"

#include "mealkit/core/Log.h"
#include "mealkit/editor/Project.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Process.h"

#include <mkcontract/User.h>

namespace mealkit::editor {

std::string defaultToolsFolder() { return editorUserDirectory() + "\\" + mkcontract::kToolsFolder; }

std::vector<ToolInfo> findTools(const std::vector<std::string>& extraFolders) {
    std::vector<std::string> errors;
    std::vector<ToolInfo> out;
    for (mkcontract::ToolManifest& m : mkcontract::findTools(mkcontract::toolSearchRoots(extraFolders), &errors)) {
        for (const std::string& w : m.warnings) MEALKIT_WARN("Tool manifest %s: %s", m.folder.c_str(), w.c_str());
        ToolInfo t;
        static_cast<mkcontract::ToolManifest&>(t) = std::move(m);
        if (t.protocol != live::kProtocol) t.problem = ToolInfo::Problem::Protocol;
        else if (!fileExists(t.exe)) t.problem = ToolInfo::Problem::MissingExe;
        out.push_back(std::move(t));
    }
    for (const std::string& e : errors) MEALKIT_WARN("Tool manifest %s", e.c_str());
    return out;
}

bool launchTool(const ToolInfo& tool, const std::string& projectDirectory, const std::string& file) {
    if (tool.problem != ToolInfo::Problem::None) return false;
    MEALKIT_INFO("Opening tool %s %s: %s %s", tool.name.c_str(), tool.version.c_str(), tool.exe.c_str(), file.c_str());
    return launchDetached(tool.exe, "--project \"" + projectDirectory + "\"" + (file.empty() ? "" : " --file \"" + file + "\""), tool.folder);
}

std::vector<mkcontract::AvailableTool> notInstalledTools(const std::vector<ToolInfo>& found) {
    std::vector<mkcontract::AvailableTool> out;
    for (mkcontract::AvailableTool& a : mkcontract::readAvailableTools()) {
        bool have = false;
        for (const ToolInfo& t : found) have |= t.name == a.name;
        if (!have) out.push_back(std::move(a));
    }
    return out;
}

bool openManagerTools() {
    const std::string manager = editorUserDirectory() + "\\manager\\MealKitManager.exe";
    if (!fileExists(manager)) return false;
    return launchDetached(manager, "--tools", editorUserDirectory() + "\\manager");
}

} // namespace mealkit::editor
