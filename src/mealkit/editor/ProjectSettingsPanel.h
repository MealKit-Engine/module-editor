#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/editor/Project.h"

#include <string>

// Editor [Project Settings] window (mealkit.project): edits engine items (game::ProjectSettings) and items added by the game (GameModule::projectSettingsType).
// The editor holds the values saved on disk (ProjectInfo); this window holds only values being edited. Saving changes the ProjectInfo passed in.
namespace mealkit::editor {

class ProjectSettingsPanel {
public:
    // Project settings type added by the game (nullptr before unloading the DLL). Values are kept as text.
    void setGameType(const reflect::TypeInfo* type);
    // Reverts edited values to the disk values (also when a project opens)
    void revert(const ProjectInfo& project);
    bool save(ProjectInfo& project); // false on failure (status text via takeStatus)
    // Changes only the start scene and saves immediately ([Scenes] window). Other values being edited in the window aren't saved
    bool setStartScene(ProjectInfo& project, const std::string& scene);
    // Re-receives only the tool-managed values (engine, engineVersion) (after an upgrade)
    void setToolValues(const game::ProjectSettings& from) {
        m_edit.engine = from.engine;
        m_edit.engineVersion = from.engineVersion;
    }

    bool dirty() const { return m_dirty; }
    // currentScene: current scene path relative to assets ([Use current scene as start scene], empty string if none)
    void drawContent(ProjectInfo& project, const std::string& currentScene);
    std::string takeStatus() { return std::move(m_status); }

private:
    game::ProjectSettings m_edit;   // values edited in the window
    reflect::Instance m_game;       // instance of the game DLL's project settings type
    reflect::TextFields m_gameText; // keeps values when there's no game type (when swapping the DLL)
    bool m_dirty = false;
    std::string m_status;
};

} // namespace mealkit::editor
