#include "mealkit/editor/TuningPanel.h"

#include "mealkit/core/Log.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace mealkit::editor {

std::string TuningPanel::docPath(const Doc& doc) const {
    return m_projectDir.empty() ? std::string() : m_projectDir + "\\assets\\" + doc.file;
}

void TuningPanel::setDocType(Doc& doc, const reflect::TypeInfo* type) {
    if (doc.value.type()) { // 지금 값을 텍스트로 보존
        doc.text.clear();
        reflect::toText(*doc.value.type(), doc.value.data(), doc.text);
    }
    doc.value = type ? reflect::Instance(*type) : reflect::Instance();
    if (!type) return;
    if (!doc.loaded) {
        load(doc);
        doc.loaded = true;
    } else {
        std::string unknown;
        reflect::fromText(*type, doc.value.data(), doc.text, &unknown);
        if (!unknown.empty()) MEALKIT_WARN("%s fields no longer declared: %s", doc.file, unknown.c_str());
        doc.changed = true;
    }
}

bool TuningPanel::load(Doc& doc) {
    if (!doc.value.type()) return false;
    doc.value = reflect::Instance(*doc.value.type()); // 기본값부터
    const std::string path = docPath(doc);
    doc.dirty = false;
    doc.changed = true;
    if (path.empty() || !fileExists(path)) return false;
    std::string error;
    if (!reflect::loadTextFile(*doc.value.type(), doc.value.data(), path, &error)) {
        m_status = trf("Could not read the tuning file: %s", error.c_str());
        return false;
    }
    return true;
}

bool TuningPanel::save(Doc& doc) {
    const std::string path = docPath(doc);
    if (!doc.value.type() || path.empty()) return false;
    const std::string header =
        std::string("MealKit tuning (") + doc.value.type()->name + "). Edit it in the editor's [Tuning] panel.";
    if (!reflect::saveTextFile(*doc.value.type(), doc.value.data(), path, header.c_str())) {
        m_status = trf("Could not save tuning: %s", path.c_str());
        return false;
    }
    doc.dirty = false;
    m_status = trf("Tuning saved: %s", (std::string("assets\\") + doc.file).c_str());
    return true;
}

void TuningPanel::resolveRows(const data::TableIndexList& index, std::vector<std::string>* problems) {
    if (m_tuning.value.type()) data::resolveRows(*m_tuning.value.type(), m_tuning.value.data(), index, problems, "game.tuning: ");
}

void TuningPanel::saveDirty() {
    if (m_tuning.dirty) save(m_tuning);
    if (m_viewTuning.dirty) save(m_viewTuning);
}

std::string TuningPanel::writePlayCopy(const Doc& doc, const char* fileName) {
    if (!doc.value.type() || m_projectDir.empty()) return {};
    const std::string dir = m_projectDir + "\\build\\Editor\\play";
    createDirectories(dir);
    const std::string path = dir + "\\" + fileName;
    return reflect::saveTextFile(*doc.value.type(), doc.value.data(), path, "editor play (unsaved values)")
               ? path
               : std::string();
}

bool TuningPanel::takeChanged(Doc& doc) {
    const bool changed = doc.changed;
    doc.changed = false;
    return changed;
}

void TuningPanel::drawDoc(Doc& doc, bool liveInMultiplayer, bool playing, bool multiplayer,
                          const data::TableIndexList& index) {
    const reflect::TypeInfo* type = doc.value.type();
    ImGui::TextDisabled("assets\\%s (%s)%s", doc.file, type->name, doc.dirty ? " *" : "");
    ImGui::BeginDisabled(!doc.dirty);
    if (ImGui::Button(tr("Save"))) save(doc);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Reload from File"))) load(doc);
    ImGui::SameLine();
    if (ImGui::Button(tr("Defaults"))) {
        doc.value = reflect::Instance(*type);
        doc.dirty = doc.changed = true;
    }
    if (playing && (!multiplayer || liveInMultiplayer))
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s", tr("Playing: changes apply immediately"));
    else if (playing)
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.4f, 1.0f), "%s",
                           tr("Changes do not apply during multiplayer play (from the next play)"));
    ImGui::Separator();
    ImGui::PushItemWidth(-ImGui::GetContentRegionAvail().x * 0.45f);
    const FieldContext context{&index};
    if (drawFields(*type, doc.value.data(), &context)) doc.dirty = doc.changed = true;
    ImGui::PopItemWidth();
}

void TuningPanel::draw(const char* dockWith, bool playing, bool multiplayer, const data::TableIndexList& index) {
    // 예전 레이아웃 파일에는 이 창이 없다: 처음 열 때 dockWith 창과 같은 자리에 탭으로 붙인다
    if (ImGuiWindow* other = ImGui::FindWindowByName(dockWith); other && other->DockId)
        ImGui::SetNextWindowDockID(other->DockId, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(trl(kWindow))) {
        ImGui::End();
        return;
    }
    if (!m_tuning.value.type()) {
        ImGui::TextDisabled("%s", tr("Tuning values appear when the game code is loaded."));
        ImGui::End();
        return;
    }
    // 게임 = 시뮬레이션 규칙 값(콘텐츠 해시), 화면 = 보이기만 하는 값(해시 밖, 여러 명 플레이 중에도 바로 적용)
    if (ImGui::BeginTabBar("##tuningTabs")) {
        if (ImGui::BeginTabItem(trl("Game###GameTuning"))) {
            drawDoc(m_tuning, false, playing, multiplayer, index);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(trl("Display###ViewTuning"))) {
            if (m_viewTuning.value.type()) drawDoc(m_viewTuning, true, playing, multiplayer, index);
            else ImGui::TextDisabled("%s", tr("The game declares no view tuning (ViewTuningType)."));
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

} // namespace mealkit::editor
