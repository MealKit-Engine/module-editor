#pragma once

#include "mealkit/editor/Project.h"
#include "mealkit/platform/Process.h"

#include <mkcontract/Modules.h>

#include <string>
#include <vector>

// Editor [Modules] window (docs/MODULES.md): source modules in the project's modules/ and the registry (MealKit-Modules) list.
// Add/update/remove call the engine's tools\module.ps1 without a window (the rules live in that one script). The list comes from one `module.ps1 search -Tsv`,
// read with mkcontract::parseModuleList (the line shape is MealKit-Contract's, shared with MealKit Manager).
// Changing modules requires rebuilding game code, so the editor is notified via takeChanged().
namespace mealkit::editor {

using mkcontract::InstalledModule;
using mkcontract::ModuleList;
using mkcontract::parseModuleList;
using mkcontract::RegistryModule;

class ModulesPanel {
public:

    // editing: whether in edit mode (modules aren't changed during play)
    void update() { poll(); } // every frame, so a running command finishes with the page closed
    void drawContent(const ProjectInfo& project, const std::string& engineDirectory, bool editing);
    // A module was added, changed, or removed (the editor rebuilds game code)
    bool takeChanged() {
        const bool changed = m_changed;
        m_changed = false;
        return changed;
    }
    std::string takeStatus() { return std::move(m_status); }

private:
    void run(const std::string& engineDirectory, const ProjectInfo& project, const std::string& label,
             const std::string& arguments, bool changes);
    void poll();

    ChildProcess m_process;
    std::string m_output;     // output of the command running now
    std::string m_label;      // command running now (status text)
    bool m_listing = false;   // whether the running command is fetching the list
    bool m_changes = false;   // whether the running command changes modules
    bool m_needList = true;   // fetch the list next frame
    std::string m_projectDir; // project the list was fetched for (refetched if it changes)
    ModuleList m_list;
    std::string m_log;        // output of the last add/update/remove
    bool m_logFailed = false;
    char m_search[96] = {};
    std::string m_confirmRemove;
    bool m_changed = false;
    std::string m_status;
};

} // namespace mealkit::editor
