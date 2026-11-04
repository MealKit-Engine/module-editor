#include "mealkit/editor/TuningPanel.h"
#include "mealkit/editor/EditorColors.h"

#include "mealkit/core/Log.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/Project.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace mealkit::editor {

std::string TuningPanel::docPath(const Doc& doc) const {
    return m_projectDir.empty() ? std::string() : m_projectDir + "\\assets\\" + doc.file;
}

void TuningPanel::setDocType(Doc& doc, const reflect::TypeInfo* type) {
    if (doc.value.type()) { // keep the current values as text
        doc.text.clear();
        reflect::toText(*doc.value.type(), doc.value.data(), doc.text);
    }
    doc.value = type ? reflect::Instance(*type) : reflect::Instance();
    if (!type) return;
    const auto resetUndo = [&doc] {
        reflect::TextFields now;
        reflect::toText(*doc.value.type(), doc.value.data(), now);
        doc.undo.reset(std::move(now));
    };
    if (!doc.loaded) {
        load(doc);
        doc.loaded = true;
        resetUndo();
    } else {
        std::string unknown;
        reflect::fromText(*type, doc.value.data(), doc.text, &unknown);
        if (!unknown.empty()) MEALKIT_WARN("%s fields no longer declared: %s", doc.file, unknown.c_str());
        doc.changed = true;
        resetUndo();
    }
}

bool TuningPanel::load(Doc& doc) {
    if (!doc.value.type()) return false;
    doc.value = reflect::Instance(*doc.value.type()); // starting from defaults
    const std::string path = docPath(doc);
    doc.dirty = false;
    doc.changed = true;
    if (path.empty() || !fileExists(path)) return false;
    std::string error;
    if (!reflect::loadTextFile(*doc.value.type(), doc.value.data(), path, text::kTuning, &error)) {
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
    if (!reflect::saveTextFile(*doc.value.type(), doc.value.data(), path, text::kTuning, header.c_str())) {
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
    const std::string dir = m_projectDir + "\\build\\" + editorBuildName() + "\\play";
    createDirectories(dir);
    const std::string path = dir + "\\" + fileName;
    return reflect::saveTextFile(*doc.value.type(), doc.value.data(), path, text::kTuning, "editor play (unsaved values)")
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
    if (ImGui::Button(tr("Reload from File"))) {
        load(doc);
        doc.undo.mark();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Defaults"))) {
        doc.value = reflect::Instance(*type);
        doc.dirty = doc.changed = true;
        doc.undo.mark();
    }
    if (playing && (!multiplayer || liveInMultiplayer))
        ImGui::TextColored(color::kGood, "%s", tr("Playing: changes apply immediately"));
    else if (playing)
        ImGui::TextColored(color::kWarning, "%s",
                           tr("Changes do not apply during multiplayer play (from the next play)"));
    ImGui::Separator();
    ImGui::PushItemWidth(-ImGui::GetContentRegionAvail().x * 0.45f);
    const FieldContext context{&index};
    if (drawFields(*type, doc.value.data(), &context)) {
        doc.dirty = doc.changed = true;
        doc.undo.mark();
    }
    ImGui::PopItemWidth();
    doc.undo.settle(ImGui::IsAnyItemActive(), [&] {
        reflect::TextFields now;
        reflect::toText(*type, doc.value.data(), now);
        return now;
    });
}

void TuningPanel::drawContent(int which, bool playing, bool multiplayer, const data::TableIndexList& index) {
    Doc& doc = which == 0 ? m_tuning : m_viewTuning;
    if (doc.value.type()) drawDoc(doc, which == 1, playing, multiplayer, index); // Game = simulation rule values (content hash), Display = outside the hash, applied at once even in multiplayer
    else if (which == 0) ImGui::TextDisabled("%s", tr("Tuning values appear when the game code is loaded."));
    else ImGui::TextDisabled("%s", tr("The game declares no view tuning (ViewTuningType)."));
}

void TuningPanel::step(int which, bool back) {
    Doc& doc = which == 0 ? m_tuning : m_viewTuning;
    reflect::TextFields text;
    if (!doc.value.type() || !doc.undo.step(back, text)) return;
    doc.value = reflect::Instance(*doc.value.type());
    reflect::fromText(*doc.value.type(), doc.value.data(), text);
    doc.dirty = doc.changed = true;
}

} // namespace mealkit::editor
