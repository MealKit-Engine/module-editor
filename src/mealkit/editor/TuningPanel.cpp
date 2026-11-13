#include "mealkit/editor/TuningPanel.h"
#include "mealkit/editor/EditorColors.h"

#include "mealkit/core/Log.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Project.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace mealkit::editor {
namespace {

// Copy of a value instance through its text (Instance owns DLL-typed memory)
void copyValues(const reflect::Instance& from, reflect::Instance& to) {
    to = reflect::Instance(*from.type());
    reflect::TextFields text;
    reflect::toText(*from.type(), from.data(), text);
    reflect::fromText(*from.type(), to.data(), text);
}

} // namespace

std::string TuningPanel::docPath(const Doc& doc) const {
    return m_projectDir.empty() ? std::string() : m_projectDir + "\\assets\\" + doc.file;
}

void TuningPanel::setDocType(Doc& doc, const reflect::TypeInfo* type) {
    if (doc.value.type()) { // keep the current values as text
        doc.text.clear();
        reflect::toText(*doc.value.type(), doc.value.data(), doc.text);
    }
    reflect::TextFields saved; // the file's values, also kept as text: the old type's code leaves with the DLL
    const bool hadSaved = doc.saved.type() != nullptr;
    if (hadSaved) reflect::toText(*doc.saved.type(), doc.saved.data(), saved);
    doc.saved = reflect::Instance();
    if (hadSaved) doc.savedText = std::move(saved);
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
        doc.saved = reflect::Instance(*type); // the file's values in the new type
        reflect::fromText(*type, doc.saved.data(), doc.savedText);
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
    doc.active = -1;
    if (path.empty() || !fileExists(path)) {
        copyValues(doc.value, doc.saved);
        return false;
    }
    std::string error;
    const bool ok = reflect::loadTextFile(*doc.value.type(), doc.value.data(), path, text::kTuning, &error);
    if (!ok) m_status = trf("Could not read the tuning file: %s", error.c_str());
    copyValues(doc.value, doc.saved);
    return ok;
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
    copyValues(doc.value, doc.saved);
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

std::string TuningPanel::presetPath(const Doc& doc, const std::string& name) const {
    std::string stem = doc.file;
    stem.resize(stem.size() - 7); // ".tuning"
    return m_projectDir + "\\assets\\" + stem + "." + name + ".tuning";
}

// Names of assets/<stem>.<name>.tuning
std::vector<std::string> TuningPanel::presets(const Doc& doc) const {
    std::vector<std::string> out;
    if (m_projectDir.empty()) return out;
    std::string prefix = doc.file;
    prefix.resize(prefix.size() - 6); // keeps the dot: "game."
    for (const std::string& f : listFiles(m_projectDir + "\\assets", ".tuning"))
        if (f.size() > prefix.size() + 7 && f.rfind(prefix, 0) == 0) out.push_back(f.substr(prefix.size(), f.size() - prefix.size() - 7));
    return out;
}

bool TuningPanel::applyFile(Doc& doc, const std::string& path) {
    reflect::Instance values(*doc.value.type());
    std::string error;
    if (!reflect::loadTextFile(*doc.value.type(), values.data(), path, text::kTuning, &error)) {
        m_status = trf("Could not read the tuning file: %s", error.c_str());
        return false;
    }
    doc.value = std::move(values);
    doc.changed = true;
    reflect::TextFields now, file;
    reflect::toText(*doc.value.type(), doc.value.data(), now);
    reflect::toText(*doc.saved.type(), doc.saved.data(), file);
    doc.dirty = now != file;
    doc.undo.mark();
    return true;
}

// Shows slot A or B: a preset, or the file's values when the slot is empty
void TuningPanel::applySlot(Doc& doc, int slot) {
    if (doc.slot[slot].empty()) {
        copyValues(doc.saved, doc.value);
        doc.changed = true;
        doc.dirty = false;
        doc.undo.mark();
    } else if (!applyFile(doc, presetPath(doc, doc.slot[slot]))) return;
    doc.active = slot;
    m_status = trf("Tuning %s: %s", slot == 0 ? "A" : "B", doc.slot[slot].empty() ? tr("(file)") : doc.slot[slot].c_str());
}

void TuningPanel::drawPresets(Doc& doc) {
    const std::vector<std::string> names = presets(doc);
    // A and B: pick a preset (or the file) for each, then switch while playing
    for (int s = 0; s < 2; ++s) {
        ImGui::PushID(s);
        if (s) ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
        const char* shown = doc.slot[s].empty() ? tr("(file)") : doc.slot[s].c_str();
        if (beginCombo(s == 0 ? "A" : "B", shown)) {
            if (ImGui::Selectable(tr("(file)"), doc.slot[s].empty())) doc.slot[s].clear();
            for (const std::string& n : names)
                if (ImGui::Selectable(n.c_str(), n == doc.slot[s])) doc.slot[s] = n;
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
    ImGui::SameLine();
    const int next = doc.active == 0 ? 1 : 0;
    if (ImGui::Button((trf("Show %s", next == 0 ? "A" : "B") + "##ab").c_str())) applySlot(doc, next);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Puts the other set's values in place. While playing they apply at once."));
    if (doc.active >= 0) {
        ImGui::SameLine();
        ImGui::TextColored(color::kGood, "%s", trf("Showing %s", doc.active == 0 ? "A" : "B").c_str());
    }
    ImGui::SameLine();
    if (iconButton(icon::kAdd, tr("Save as Preset"))) {
        m_presetName[0] = 0;
        ImGui::OpenPopup("##savepreset");
    }
    if (ImGui::BeginPopup("##savepreset")) {
        ImGui::TextUnformatted(tr("Preset name (letters, digits, - and _)"));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##name", m_presetName, sizeof(m_presetName), ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string name = m_presetName;
        bool valid = !name.empty();
        for (char c : name) valid &= (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button(tr("Save")) || (enter && valid)) {
            const std::string path = presetPath(doc, name);
            const std::string header = std::string("MealKit tuning preset (") + doc.value.type()->name + "). A/B sets of the editor's [Tuning] panel.";
            if (reflect::saveTextFile(*doc.value.type(), doc.value.data(), path, text::kTuning, header.c_str()))
                m_status = trf("Preset saved: %s", (std::string("assets\\") + fileName(path)).c_str());
            else m_status = trf("Could not save tuning: %s", path.c_str());
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
}

void TuningPanel::drawDoc(Doc& doc, bool liveInMultiplayer, bool playing, bool multiplayer,
                          const data::TableIndexList& index) {
    const reflect::TypeInfo* type = doc.value.type();
    ImGui::TextDisabled("assets\\%s (%s)%s", doc.file, type->name, doc.dirty ? " *" : "");
    ImGui::BeginDisabled(!doc.dirty);
    if (iconButton(icon::kSave, tr("Save"))) save(doc);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (iconButton(icon::kRefresh, tr("Reload from File"))) {
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
    drawPresets(doc);
    ImGui::Separator();
    ImGui::PushItemWidth(-ImGui::GetContentRegionAvail().x * 0.45f);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ImGui::InputTextWithHint("##search", tr("Search"), m_search, sizeof(m_search));
    ImGui::SameLine();
    checkbox(tr("Only changed"), &m_onlyChanged);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    if (beginCombo(tr("Compare with"), doc.compareFile ? tr("File") : tr("Defaults"))) {
        if (ImGui::Selectable(tr("Defaults"), !doc.compareFile)) doc.compareFile = false;
        if (ImGui::Selectable(tr("File"), doc.compareFile)) doc.compareFile = true;
        ImGui::EndCombo();
    }
    const reflect::Instance defaults(*type);
    FieldContext context{&index, doc.compareFile && doc.saved.type() ? doc.saved.data() : defaults.data()};
    context.filter = m_search;
    context.onlyChanged = m_onlyChanged;
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

void TuningPanel::revertDoc(int which) {
    Doc& doc = which == 0 ? m_tuning : m_viewTuning;
    load(doc);
    doc.undo.mark();
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
