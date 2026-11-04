#include "mealkit/editor/Editor.h"
#include "mealkit/editor/EditorColors.h"

#include "mealkit/asset/ModelCache.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Window.h"

#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
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

// gizmo=radius fields of the selected object: three circles and a handle at the camera's right. Dragging is one undo step
bool Editor::updateFieldHandles() {
    Entity* e = selected();
    const reflect::TypeInfo* type = e ? entityType(*e) : nullptr;
    if (!type) return false;
    const reflect::FieldInfo* field = nullptr;
    for (const reflect::FieldInfo& f : type->fields)
        if (f.value->kind == reflect::Kind::Float && splitFieldLabel(f.label).gizmo == "radius") field = &f;
    if (!field) return false;

    reflect::Instance instance(*type);
    reflect::fromText(*type, instance.data(), e->fields);
    f32& radius = *reinterpret_cast<f32*>(static_cast<u8*>(instance.data()) + field->offset);
    const Vec3 center = scene::worldTransform(m_scene, *e).position;
    const Mat4 viewProj = m_view * m_proj;
    const auto project = [&](Vec3 p, ImVec2* out) {
        const f32 cx = p.x * viewProj.m[0][0] + p.y * viewProj.m[1][0] + p.z * viewProj.m[2][0] + viewProj.m[3][0];
        const f32 cy = p.x * viewProj.m[0][1] + p.y * viewProj.m[1][1] + p.z * viewProj.m[2][1] + viewProj.m[3][1];
        const f32 cw = p.x * viewProj.m[0][3] + p.y * viewProj.m[1][3] + p.z * viewProj.m[2][3] + viewProj.m[3][3];
        if (cw <= 0.0f) return false;
        *out = ImVec2(m_viewport.x + (cx / cw * 0.5f + 0.5f) * m_viewport.width, m_viewport.y + (0.5f - cy / cw * 0.5f) * m_viewport.height);
        return true;
    };

    ImDrawList* draw = ImGui::GetBackgroundDrawList(); // over the scene view, under the editor windows
    const ImU32 line = ImGui::GetColorU32(ImVec4(1.0f, 0.85f, 0.45f, 0.8f));
    const Vec3 axes[3][2] = {{{1, 0, 0}, {0, 1, 0}}, {{1, 0, 0}, {0, 0, 1}}, {{0, 1, 0}, {0, 0, 1}}};
    for (const auto& axis : axes) {
        ImVec2 previous;
        bool hasPrevious = false;
        for (int i = 0; i <= 64; ++i) {
            const f32 a = static_cast<f32>(i) / 64.0f * 6.2831853f;
            ImVec2 now;
            const bool visible = project(center + (axis[0] * std::cos(a) + axis[1] * std::sin(a)) * radius, &now);
            if (visible && hasPrevious) draw->AddLine(previous, now, line, 1.5f);
            previous = now;
            hasPrevious = visible;
        }
    }
    const Vec3 forward = cameraForward();
    const Vec3 right = normalize(cross(Vec3{0, 1, 0}, forward));
    ImVec2 handle;
    if (!project(center + right * radius, &handle)) return false;
    const ImGuiIO& io = ImGui::GetIO();
    const f32 dx = io.MousePos.x - handle.x, dy = io.MousePos.y - handle.y;
    const bool dragging = m_handleEntity == e->id && m_handleField == field->name;
    const bool hovered = !sceneMouseBlocked() && !ImGuizmo::IsUsing() && dx * dx + dy * dy < 100.0f;
    draw->AddCircleFilled(handle, hovered || dragging ? 7.0f : 5.0f, ImGui::GetColorU32(ImVec4(1.0f, 0.85f, 0.45f, 1.0f)));
    if (hovered || dragging) ImGui::SetTooltip("%s: %.2f", trData(splitFieldLabel(field->label).name.c_str()), static_cast<double>(radius));

    if (!dragging && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        pushUndo(capture());
        m_handleEntity = e->id;
        m_handleField = field->name;
        return true;
    }
    if (!dragging) return hovered;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        m_handleEntity = 0;
        return true;
    }
    // The mouse on the plane through the center facing the camera
    Vec3 origin, dir;
    mouseRay(io.MousePos.x, io.MousePos.y, &origin, &dir);
    const f32 facing = dot(dir, forward);
    if (std::fabs(facing) > 0.001f) {
        const Vec3 hit = origin + dir * (dot(center - origin, forward) / facing);
        const f32 next = length(hit - center);
        if (next != radius) {
            radius = next;
            reflect::TextFields fields;
            reflect::toText(*type, instance.data(), fields);
            e->fields = std::move(fields);
        }
    }
    return true;
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
        ImGui::PushStyleColor(ImGuiCol_Text, p.error ? color::kDanger : color::kWarning);
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
