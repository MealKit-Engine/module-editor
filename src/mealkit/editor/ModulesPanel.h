#pragma once

#include "mealkit/editor/Project.h"
#include "mealkit/platform/Process.h"

#include <mkcontract/Modules.h>

#include <string>
#include <vector>

// Editor [Modules] window (docs/MODULES.md): engine and source modules in the project's modules/ and the registry (MealKit-Modules) list.
// Add/update/remove call the engine's tools\module.ps1 without a window (the rules live in that one script). The list comes from one `module.ps1 search -Tsv`,
// read with mkcontract::parseModuleList (the line shape is MealKit-Contract's, shared with MealKit Manager).
// One list with search, a kind filter and a tag filter on the left (resizable), the picked module's details and actions on the right.
// Changing modules requires rebuilding game code, so the editor is notified via takeChanged().
namespace mealkit::editor {

using mkcontract::InstalledModule;
using mkcontract::ModuleList;
using mkcontract::parseModuleList;
using mkcontract::RegistryModule;

// Icon glyph for a module: module.txt "icon" keyword, else the first tag that is a keyword, else the name (engine module names are keywords), else a puzzle piece
const char* moduleGlyph(const std::string& icon, const std::string& tags, const std::string& name);

class ModulesPanel {
public:
    // editing: whether in edit mode (modules aren't changed during play)
    void update() { poll(); } // every frame, so a running command finishes with the window closed
    void draw(const ProjectInfo& project, const std::string& engineDirectory, bool editing, bool* open);
    // A module was added, changed, or removed (the editor rebuilds game code)
    bool takeChanged() {
        const bool changed = m_changed;
        m_changed = false;
        return changed;
    }
    std::string takeStatus() { return std::move(m_status); }

private:
    enum class Filter { All, Installed, Engine, Community, Updates };
    // One row of the list: an installed module, a registry module, or both (same name)
    struct Item {
        std::string name, title, summary, tags, icon, author, repo;
        const InstalledModule* installed = nullptr;
        const RegistryModule* registry = nullptr;
        bool engine = false; // engine module (MealKit's own, "engine" first in its tags)
        bool update = false; // the registry has a newer version than the installed one
    };

    void run(const std::string& engineDirectory, const ProjectInfo& project, const std::string& label,
             const std::string& arguments, bool changes);
    void poll();
    void rebuildItems();
    bool visible(const Item& item) const;
    void drawToolbar(bool busy, bool editing);
    void drawList();
    void drawRow(const Item& item);
    void drawDetails(const ProjectInfo& project, const std::string& engineDirectory, bool canChange);
    bool tagChips(const std::string& tags); // clickable tags: a click sets (or clears) the tag filter. Returns whether one was clicked

    ChildProcess m_process;
    std::string m_output;     // output of the command running now
    std::string m_label;      // command running now (status text)
    bool m_listing = false;   // whether the running command is fetching the list
    bool m_changes = false;   // whether the running command changes modules
    bool m_needList = true;   // fetch the list next frame
    std::string m_projectDir; // project the list was fetched for (refetched if it changes)
    ModuleList m_list;
    std::vector<Item> m_items; // m_list as rows, installed first (pointers into m_list)
    std::string m_log;         // output of the last add/update/remove
    bool m_logFailed = false;
    char m_search[96] = {};
    Filter m_filter = Filter::All;
    std::string m_tag;      // tag filter (blank: any)
    std::string m_selected; // module name shown on the right
    bool m_compact = false; // one line per row
    std::string m_confirmRemove;
    bool m_changed = false;
    std::string m_status;
};

} // namespace mealkit::editor
