#include "mealkit/editor/Editor.h"

#include "mealkit/asset/ModelCache.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Window.h"

#include <algorithm>
#include <set>

#include <imgui.h>
#include <imgui_internal.h>

// [Data] window (tuning, tables, feedback and sounds under one list) and [Problems] window (everything that needs fixing in one list)
namespace mealkit::editor {
namespace {

using scene::Entity;

constexpr const char* kDataWindow = "Data###Data";

} // namespace

void Editor::openDataPage(DataPage page, int index) {
    m_dataOpen = true;
    m_dataPage = page;
    m_dataIndex = index;
    m_dataFrontFrames = 3;
    if (page == DataPage::Table) m_tablesPanel.setCurrent(index);
}

void Editor::toggleDataPage(DataPage page, int index) {
    if (m_dataOpen && m_dataPage == page && (page == DataPage::Table || m_dataIndex == index)) m_dataOpen = false;
    else openDataPage(page, page == DataPage::Table ? m_tablesPanel.current() : index);
}

bool Editor::dataCanStep(bool back) const {
    switch (m_dataPage) {
    case DataPage::Tuning: return m_tuningPanel.canStep(m_dataIndex, back);
    case DataPage::Table: return m_tablesPanel.canStep(m_dataIndex, back);
    case DataPage::Feedback: return m_feedbackPanel.canStep(back);
    case DataPage::Sounds: return false;
    }
    return false;
}

void Editor::dataStep(bool back) {
    switch (m_dataPage) {
    case DataPage::Tuning: m_tuningPanel.step(m_dataIndex, back); break;
    case DataPage::Table: m_tablesPanel.step(m_dataIndex, back); break;
    case DataPage::Feedback: m_feedbackPanel.step(back); break;
    case DataPage::Sounds: break;
    }
}

void Editor::drawDataHub(f64 dt, bool editing) {
    m_feedbackPanel.update(dt); // autosave continues after the window closes
    m_dataFocused = false;
    if (!m_dataOpen) return;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 62.0f, ImGui::GetFontSize() * 36.0f), ImGuiCond_FirstUseEver);
    if (m_dataFrontFrames > 0) {
        --m_dataFrontFrames;
        ImGui::SetNextWindowFocus();
    }
    if (!ImGui::Begin(trl(kDataWindow), &m_dataOpen)) {
        ImGui::End();
        return;
    }
    m_dataFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // ---- Left: every data document, with unsaved marks
    if (ImGui::BeginChild("##dataList", ImVec2(ImGui::GetFontSize() * 15.0f, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders)) {
        auto entry = [this](const std::string& label, DataPage page, int index, bool dirty) {
            const std::string text = label + (dirty ? " *" : "") + "##" + std::to_string(static_cast<int>(page)) + "_" + std::to_string(index);
            if (ImGui::Selectable(text.c_str(), m_dataPage == page && m_dataIndex == index)) openDataPage(page, index);
        };
        ImGui::SeparatorText(tr("Tuning"));
        entry(tr("Game"), DataPage::Tuning, 0, m_tuningPanel.docDirty(0));
        entry(tr("Display"), DataPage::Tuning, 1, m_tuningPanel.docDirty(1));
        ImGui::SeparatorText(tr("Tables"));
        for (int i = 0; i < m_tablesPanel.docCount(); ++i) entry(m_tablesPanel.docName(i), DataPage::Table, i, m_tablesPanel.docDirty(i));
        if (m_tablesPanel.docCount() == 0) ImGui::TextDisabled("%s", tr("Tables appear when the game code is loaded (MEALKIT_TABLE)."));
        ImGui::SeparatorText(tr("Reactions"));
        entry(tr("Feedback"), DataPage::Feedback, 0, false);
        if (m_audio) entry(tr("Sounds"), DataPage::Sounds, 0, false);
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ---- Right: the chosen document
    if (ImGui::BeginChild("##dataPage")) {
        ImGui::BeginDisabled(!dataCanStep(true));
        if (ImGui::Button(tr("Undo"))) dataStep(true);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!dataCanStep(false));
        if (ImGui::Button(tr("Redo"))) dataStep(false);
        ImGui::EndDisabled();
        switch (m_dataPage) {
        case DataPage::Tuning:
            m_tuningPanel.drawContent(m_dataIndex, m_mode == Mode::Play, m_playPlayers > 1, m_tablesPanel.index());
            break;
        case DataPage::Table: m_tablesPanel.drawContent(m_dataIndex, m_window->nativeHandle(), editing); break;
        case DataPage::Feedback: m_feedbackPanel.drawContent(m_feedback, m_cameraPos + cameraForward() * 10.0f); break;
        case DataPage::Sounds:
            if (m_audio) m_soundPanel.drawContent(*m_audio);
            break;
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// Gathers problems twice a second: missing models, models over budget, and row references in tables, tuning and scene objects
void Editor::updateProblems(f64 dt) {
    m_problemTimer += dt;
    if (m_problemTimer < 0.5) return;
    m_problemTimer = 0.0;
    m_problems.clear();
    m_badges.clear();
    m_differsFromDefault.clear();
    for (const Entity& e : m_scene.entities) { // field changes from the kind's defaults
        const reflect::TypeInfo* type = entityType(e);
        if (!type || type->fields.empty()) continue;
        const reflect::TextFields defaults = scene::defaultFields(*type);
        for (const auto& field : e.fields)
            if (std::find(defaults.begin(), defaults.end(), field) == defaults.end()) {
                m_differsFromDefault.insert(e.id);
                break;
            }
    }

    const std::string root = asset::ModelCache::assetsRoot();
    std::set<std::string> seen;
    for (const Entity& e : m_scene.entities) {
        if (e.model.empty()) continue;
        const bool first = seen.insert(e.model).second; // one list line per model, a badge on every object using it
        std::string native = e.model;
        std::replace(native.begin(), native.end(), '/', '\\');
        if (!fileExists(root + "\\" + native) && !fileExists(e.model)) {
            std::string text = trf("Missing model: %s (%s)", e.model.c_str(), e.name.c_str());
            m_badges[e.id] = {true, text};
            if (first) m_problems.push_back({true, std::move(text), DataPage::Sounds, -1, e.id});
            continue;
        }
        const asset::Model* model = m_models ? m_models->get(e.model) : nullptr;
        if (!model) continue;
        for (const std::string& over : model->budgetProblems) {
            std::string text = trf("Over budget: %s: %s", e.model.c_str(), over.c_str());
            m_badges[e.id] = {false, text};
            if (first) m_problems.push_back({false, std::move(text), DataPage::Sounds, -1, e.id});
        }
    }

    const data::TableIndexList& index = m_tablesPanel.index();
    if (index.empty()) return;
    std::vector<std::pair<int, std::string>> tableProblems;
    m_tablesPanel.collectProblems(tableProblems);
    for (auto& [doc, text] : tableProblems) m_problems.push_back({true, std::move(text), DataPage::Table, doc, 0});
    std::vector<std::string> found;
    m_tuningPanel.resolveRows(index, &found);
    for (std::string& text : found) m_problems.push_back({true, std::move(text), DataPage::Tuning, 0, 0});
    for (const Entity& e : m_scene.entities) {
        const reflect::TypeInfo* type = entityType(e);
        if (!type || type->fields.empty() || !m_gameTypes) continue;
        reflect::Instance instance(*type);
        reflect::fromText(*type, instance.data(), e.fields);
        found.clear();
        data::resolveRows(*type, instance.data(), index, &found, fileName(m_scenePath) + " " + e.name + ": ");
        for (std::string& text : found) {
            m_badges[e.id] = {true, text};
            m_problems.push_back({true, std::move(text), DataPage::Sounds, -1, e.id});
        }
    }
}

void Editor::drawProblems() {
    if (!m_problemsOpen) return;
    // The count is in the tab, so problems show without opening it. Old layout files don't have this window: first open docks it by the log
    if (ImGuiWindow* log = ImGui::FindWindowByName(LogPanel::kWindow); log && log->DockId)
        ImGui::SetNextWindowDockID(log->DockId, ImGuiCond_FirstUseEver);
    const std::string title = (m_problems.empty() ? std::string(tr("Problems")) : trf("Problems (%zu)", m_problems.size())) + "###Problems";
    if (!ImGui::Begin(title.c_str(), &m_problemsOpen)) {
        ImGui::End();
        return;
    }
    if (m_problems.empty()) ImGui::TextDisabled("%s", tr("No problems found."));
    for (size_t i = 0; i < m_problems.size(); ++i) {
        const Problem& p = m_problems[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Text, p.error ? ImVec4(1.0f, 0.55f, 0.45f, 1.0f) : ImVec4(0.95f, 0.75f, 0.4f, 1.0f));
        const bool clicked = ImGui::Selectable(p.text.c_str());
        ImGui::PopStyleColor();
        if (clicked) { // copied first: opening a page must not depend on the list staying the same
            const u32 entity = p.entity;
            const DataPage page = p.page;
            const int doc = p.doc;
            if (entity != 0) {
                m_selected = entity;
                focusInspector();
            } else if (doc >= 0) {
                openDataPage(page, doc);
            }
        }
        ImGui::PopID();
    }
    ImGui::End();
}

} // namespace mealkit::editor
