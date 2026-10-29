#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/core/Types.h"
#include "mealkit/live/Server.h"

#include <string>
#include <vector>

// Penpot import (ROADMAP "Game screen UI part 2"): the penpot/ service of the editor's live connection server (mealkit/live/Server.h).
//   GET  /<token>/penpot/manifest.json, plugin.js, ui.html, icon.png   Penpot plugin files (engine tools/penpot-plugin/dist)
//   GET  /<token>/penpot/api/project                                   project name, fonts, view model fields, actions used in .ui (plugin dropdowns)
//   POST /<token>/penpot/api/export                                    writes received .ui/PNG/strings/theme/Penpot source/layout boxes into the project
// Security: the server guards with 127.0.0.1 and a random per-run token (included in the plugin install address).
// Origin must be Penpot (https://*.penpot.app, self-hosted localhost) or this server, and writes (POST) only from this server's panel.
// Written paths are only .ui/.png under assets/ui/, .penpot/boxes/*.boxes/preview/*.png under art/ui/, and the project default language's string table (".." rejected).
namespace mealkit::editor {

struct ImportProject {
    std::string engineDir;  // plugin files (tools/penpot-plugin/dist)
    std::string projectDir; // where received files are written
    std::string name;
    std::string language; // table to add new string keys to (assets/strings/<language>.txt)
};

class ImportServer {
public:
    // Adds the penpot/ service to server (the server must be open first to have an address)
    void attach(live::Server& server, const ImportProject& project);
    void setViewModels(const std::vector<const reflect::TypeInfo*>* types) { m_viewModels = types; }

    std::string pluginUrl() const; // address pasted into the Penpot plugin manager
    // One line every time an import finishes (status bar)
    std::vector<std::string> takeEvents();

    live::Response handle(const live::Request& request); // path within the service (public so it can be tested without sockets)

private:
    live::Response serveFile(const std::string& name) const;
    live::Response project() const;
    live::Response importFiles(const std::string& body);

    live::Server* m_server = nullptr;
    ImportProject m_project;
    const std::vector<const reflect::TypeInfo*>* m_viewModels = nullptr;
    std::vector<std::string> m_events;
};

} // namespace mealkit::editor
