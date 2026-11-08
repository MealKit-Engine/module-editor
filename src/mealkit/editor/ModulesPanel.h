#pragma once

#include "mealkit/editor/Project.h"
#include "mealkit/platform/Process.h"

#include <mkcontract/Modules.h>

#include <string>
#include <vector>

// Editor [Modules] window (docs/MODULES.md): engine and source modules in the project's modules/ and the registry (MealKit-Modules) list.
// Add/update/remove/resolve call the engine's tools\module.ps1 without a window (the rules live in that one script). The list comes from one `module.ps1 search -Tsv`,
// read with mkcontract::parseModuleList (the line shape is MealKit-Contract's, shared with MealKit Manager).
// One list with search, a kind filter and a tag filter on the left (resizable), the picked module's details and actions on the right:
// requires and required-by, .new files left by an update, other versions, and update all.
// Changing modules requires rebuilding game code, so the editor is notified via takeChanged().
namespace mealkit::editor {

using mkcontract::InstalledModule;
using mkcontract::ModuleList;
using mkcontract::parseModuleList;
using mkcontract::RegistryModule;

// Icon glyph for a module (mkcontract::moduleIconKeyword, a puzzle piece when there is none)
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
    enum class Job { List, Change, Versions, Preview };
    // One row of the list: an installed module, a registry module, or both (same name)
    struct Item {
        std::string name, title, summary, tags, icon, author, repo;
        const InstalledModule* installed = nullptr;
        const RegistryModule* registry = nullptr;
        bool engine = false; // engine module (MealKit's own, "engine" first in its tags)
        bool update = false; // a newer version for this engine than the installed one
        bool inUse = false;  // an engine module the game builds with although it isn't in modules/ (the project has no engine modules)
    };
    // What the details pane shows for the picked module, read from files when the pick or the list changes
    struct Details {
        std::string name;
        std::vector<std::string> needs, neededBy, missing; // module.txt requires, installed modules requiring it, requires not in the build
        std::vector<std::string> newFiles; // paths in the module of <file>.new left by an update
    };
    struct Version {
        std::string version, engine;
        bool fits = true;
    };

    void run(const std::string& label, const std::string& arguments, Job job);
    void poll();
    void rebuildItems();
    void readDetails(const std::string& name);
    bool visible(const Item& item) const;
    int updateCount() const;
    void drawToolbar(bool busy, bool editing);
    void drawList();
    void drawRow(const Item& item);
    void drawDetails(bool canChange);
    void drawPopups();
    void add(const Item& item, const std::string& spec, bool withRequires);
    bool tagChips(const std::string& tags); // clickable tags: a click sets (or clears) the tag filter. Returns whether one was clicked
    bool anyEngineInstalled() const;

    ChildProcess m_process;
    std::string m_output; // output of the command running now
    std::string m_label;  // command running now (status text)
    Job m_job = Job::List;
    bool m_needList = true;   // fetch the list next frame
    std::string m_projectDir; // project the list was fetched for (refetched if it changes)
    std::string m_engineDir;
    ModuleList m_list;
    std::vector<Item> m_items; // m_list as rows, installed first (pointers into m_list)
    Details m_details;
    std::string m_versionsFor; // module whose versions were fetched
    std::vector<Version> m_versions;
    int m_pickedVersion = 0;
    std::string m_log; // output of the last change or preview
    bool m_logFailed = false;
    char m_search[96] = {};
    Filter m_filter = Filter::All;
    std::string m_tag;      // tag filter (blank: any)
    std::string m_selected; // module name shown on the right
    bool m_compact = false; // one line per row
    std::string m_confirmRemove;
    std::string m_confirmEngine; // engine module to add while the project has none (it changes what the game builds with)
    bool m_confirmEngineRequires = false;
    bool m_confirmUpdateAll = false;
    bool m_changed = false;
    std::string m_status;
};

} // namespace mealkit::editor
