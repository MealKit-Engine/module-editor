#include "mealkit/editor/ProjectSettingsPanel.h"

#include "mealkit/core/Log.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Strings.h"

#include <imgui.h>

namespace mealkit::editor {

void ProjectSettingsPanel::setGameType(const reflect::TypeInfo* type) {
    if (m_game.type()) { // keep the current values as text
        m_gameText.clear();
        reflect::toText(*m_game.type(), m_game.data(), m_gameText);
    }
    m_game = type ? reflect::Instance(*type) : reflect::Instance();
    if (type) {
        std::string unknown;
        reflect::fromText(*type, m_game.data(), m_gameText, &unknown);
        if (!unknown.empty()) MEALKIT_WARN("Project settings not declared in %s: %s", type->name, unknown.c_str());
    }
}

void ProjectSettingsPanel::revert(const ProjectInfo& project) {
    m_edit = project.settings;
    m_gameText = project.gameFields;
    if (m_game.type()) {
        m_game = reflect::Instance(*m_game.type());
        reflect::fromText(*m_game.type(), m_game.data(), m_gameText);
    }
    m_dirty = false;
}

bool ProjectSettingsPanel::setStartScene(ProjectInfo& project, const std::string& scene) {
    if (project.directory.empty()) return false;
    ProjectInfo next = project;
    next.settings.scene = scene;
    if (!game::saveProjectFile(next.filePath(), next)) {
        m_status = trf("Could not save project settings: %s", next.filePath().c_str());
        return false;
    }
    project = next;
    m_edit.scene = scene;
    m_status = trf("Start scene: %s", scene.c_str());
    return true;
}

bool ProjectSettingsPanel::save(ProjectInfo& project) {
    if (project.directory.empty()) return false;
    ProjectInfo next = project;
    next.settings = m_edit;
    if (m_game.type()) {
        next.gameFields.clear();
        reflect::toText(*m_game.type(), m_game.data(), next.gameFields);
    } else {
        next.gameFields = m_gameText;
    }
    if (!game::saveProjectFile(next.filePath(), next)) {
        m_status = trf("Could not save project settings: %s", next.filePath().c_str());
        return false;
    }
    if (next.settings.tickRate != project.settings.tickRate) game::applyTickRate(next.settings); // from the next play
    project = next;
    m_dirty = false;
    m_status = trf("Project settings saved: %s", "mealkit.project");
    return true;
}

void ProjectSettingsPanel::drawContent(ProjectInfo& project, const std::string& currentScene) {
    if (project.directory.empty()) return;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ImGui::InputTextWithHint("##search", tr("Search"), m_search, sizeof(m_search));
    ImGui::TextDisabled("%s%s", project.filePath().c_str(), m_dirty ? " *" : "");
    ImGui::BeginDisabled(!m_dirty);
    if (iconButton(icon::kSave, tr("Save"))) save(project);
    ImGui::SameLine();
    if (iconButton(icon::kUndo, tr("Revert"))) revert(project);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(currentScene.empty() || currentScene == m_edit.scene);
    if (ImGui::Button(tr("Use Current Scene as Start Scene"))) {
        m_edit.scene = currentScene;
        m_dirty = true;
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("%s", tr("The game and the editor use these values from the next run. Gray items are managed by the tools."));

    ImGui::PushItemWidth(-ImGui::GetContentRegionAvail().x * 0.4f);
    ImGui::SeparatorText(tr("Engine"));
    FieldContext context;
    context.filter = m_search;
    if (drawFields(reflect::typeOf<game::ProjectSettings>(), &m_edit, &context)) m_dirty = true;
    ImGui::SeparatorText(tr("Game"));
    if (!m_game.type()) {
        ImGui::TextDisabled(tr("Game items appear when the game code is loaded (%zu lines kept)."), m_gameText.size());
    } else if (m_game.type()->fields.empty()) {
        ImGui::TextDisabled(tr("The game declares no items (%s)."), m_game.type()->name);
    } else if (drawFields(*m_game.type(), m_game.data(), &context)) {
        m_dirty = true;
    }
    ImGui::PopItemWidth();
}

} // namespace mealkit::editor
