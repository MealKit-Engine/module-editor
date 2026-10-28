#include "mealkit/editor/Preferences.h"

#include "mealkit/editor/Project.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace mealkit::editor {
namespace {

std::string settingsPath() { return editorUserDirectory() + "\\editor_settings.txt"; }

const char* const kKeys[] = {"cameraSpeed", "uiScale",  "autosaveSeconds", "externalEditor",
                             "projectLayout", "snapMove", "snapRotate", "snapScale"};

bool ownKey(const std::string& key) {
    for (const char* k : kKeys)
        if (key == k) return true;
    return false;
}

} // namespace

Preferences loadPreferences() {
    Preferences p;
    std::string text;
    if (!readTextFile(settingsPath(), text)) return p;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t space = line.find(' ');
        if (space == std::string::npos) continue;
        const std::string key = line.substr(0, space), value = line.substr(space + 1);
        if (key == "cameraSpeed") p.cameraSpeed = std::clamp(static_cast<f32>(atof(value.c_str())), 1.0f, 400.0f);
        else if (key == "uiScale") p.uiScale = std::clamp(static_cast<f32>(atof(value.c_str())), 0.5f, 3.0f);
        else if (key == "autosaveSeconds") p.autosaveSeconds = static_cast<u32>(strtoul(value.c_str(), nullptr, 10));
        else if (key == "externalEditor") p.externalEditor = value;
        else if (key == "projectLayout") p.projectLayout = value != "0";
        else if (key == "snapMove") p.snapMove = std::clamp(static_cast<f32>(atof(value.c_str())), 0.01f, 100.0f);
        else if (key == "snapRotate") p.snapRotate = std::clamp(static_cast<f32>(atof(value.c_str())), 0.1f, 180.0f);
        else if (key == "snapScale") p.snapScale = std::clamp(static_cast<f32>(atof(value.c_str())), 0.001f, 10.0f);
    }
    return p;
}

bool savePreferences(const Preferences& p) {
    std::string old, out;
    readTextFile(settingsPath(), old);
    size_t pos = 0;
    while (pos < old.size()) { // 다른 기능의 줄(language, toolPath)은 그대로
        size_t end = old.find('\n', pos);
        if (end == std::string::npos) end = old.size();
        const std::string line = old.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && !ownKey(line.substr(0, line.find(' ')))) out += line + "\n";
    }
    char buf[512];
    snprintf(buf, sizeof(buf),
             "cameraSpeed %g\nuiScale %g\nautosaveSeconds %u\nexternalEditor %s\nprojectLayout %d\nsnapMove %g\n"
             "snapRotate %g\nsnapScale %g\n",
             p.cameraSpeed, p.uiScale, p.autosaveSeconds, p.externalEditor.c_str(), p.projectLayout ? 1 : 0, p.snapMove,
             p.snapRotate, p.snapScale);
    createDirectories(editorUserDirectory());
    return writeTextFile(settingsPath(), out + buf);
}

bool drawSnapSettings(Preferences& p) {
    bool changed = false;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    changed |= ImGui::InputFloat(tr("Move snap (m)"), &p.snapMove, 0.25f, 1.0f, "%.2f");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    changed |= ImGui::InputFloat(tr("Rotate snap (degrees)"), &p.snapRotate, 5.0f, 15.0f, "%.1f");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    changed |= ImGui::InputFloat(tr("Scale snap"), &p.snapScale, 0.05f, 0.25f, "%.2f");
    p.snapMove = std::clamp(p.snapMove, 0.01f, 100.0f);
    p.snapRotate = std::clamp(p.snapRotate, 0.1f, 180.0f);
    p.snapScale = std::clamp(p.snapScale, 0.001f, 10.0f);
    return changed;
}

bool PreferencesPanel::draw(Preferences& p) {
    if (!m_open) return false;
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(trl(kWindow), &m_open, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return false;
    }
    bool changed = false;
    ImGui::SeparatorText(tr("Scene view"));
    changed |= ImGui::SliderFloat(tr("Camera speed"), &p.cameraSpeed, 1.0f, 100.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    changed |= drawSnapSettings(p);
    ImGui::SeparatorText(tr("Interface"));
    changed |= ImGui::SliderFloat(tr("Text size"), &p.uiScale, 0.75f, 2.0f, "%.2fx");
    changed |= ImGui::Checkbox(tr("Save the window layout per project"), &p.projectLayout);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Takes effect the next time a project opens"));
    ImGui::SeparatorText(tr("Files"));
    int seconds = static_cast<int>(p.autosaveSeconds);
    if (ImGui::InputInt(tr("Autosave every (seconds, 0: off)"), &seconds, 30)) {
        p.autosaveSeconds = static_cast<u32>(std::clamp(seconds, 0, 3600));
        changed = true;
    }
    if (ImGui::IsWindowAppearing()) snprintf(m_editor, sizeof(m_editor), "%s", p.externalEditor.c_str());
    ImGui::InputText(tr("Code editor"), m_editor, sizeof(m_editor));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        p.externalEditor = m_editor;
        changed = true;
    }
    ImGui::End();
    if (changed) savePreferences(p);
    return changed;
}

} // namespace mealkit::editor
