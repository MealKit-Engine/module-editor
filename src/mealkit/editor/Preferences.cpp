#include "mealkit/editor/Preferences.h"

#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Project.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace mealkit::editor {
namespace {

std::string settingsPath() { return editorUserDirectory() + MEALKIT_SEP "editor_settings.txt"; }

const char* const kKeys[] = {"cameraSpeed", "uiScale",  "autosaveSeconds", "externalEditor",
                             "projectLayout", "snapMove", "snapRotate", "snapScale", "compact", "colorPickerSize", "gameAspect", "playMenus",
                             "window"};

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
        else if (key == "compact") p.compact = value != "0";
        else if (key == "snapMove") p.snapMove = std::clamp(static_cast<f32>(atof(value.c_str())), 0.01f, 100.0f);
        else if (key == "snapRotate") p.snapRotate = std::clamp(static_cast<f32>(atof(value.c_str())), 0.1f, 180.0f);
        else if (key == "snapScale") p.snapScale = std::clamp(static_cast<f32>(atof(value.c_str())), 0.001f, 10.0f);
        else if (key == "gameAspect") p.gameAspect = static_cast<u32>(strtoul(value.c_str(), nullptr, 10));
        else if (key == "playMenus") p.playMenus = atoi(value.c_str()) != 0;
        else if (key == "colorPickerSize") p.colorPickerSize = std::clamp(static_cast<f32>(atof(value.c_str())), 9.0f, 24.0f);
        else if (key == "window") {
            int x = 0, y = 0, w = 0, h = 0, maximized = 0;
            if (std::sscanf(value.c_str(), "%d %d %d %d %d", &x, &y, &w, &h, &maximized) == 5) p.window = {x, y, w, h, maximized != 0};
        }
    }
    return p;
}

bool savePreferences(const Preferences& p) {
    std::string old, out;
    readTextFile(settingsPath(), old);
    size_t pos = 0;
    while (pos < old.size()) { // lines of other features (language, toolPath) stay as is
        size_t end = old.find('\n', pos);
        if (end == std::string::npos) end = old.size();
        const std::string line = old.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && !ownKey(line.substr(0, line.find(' ')))) out += line + "\n";
    }
    char buf[512];
    snprintf(buf, sizeof(buf),
             "cameraSpeed %g\nuiScale %g\nautosaveSeconds %u\nexternalEditor %s\nprojectLayout %d\nsnapMove %g\n"
             "snapRotate %g\nsnapScale %g\ncompact %d\ncolorPickerSize %g\ngameAspect %u\nplayMenus %d\n",
             p.cameraSpeed, p.uiScale, p.autosaveSeconds, p.externalEditor.c_str(), p.projectLayout ? 1 : 0, p.snapMove,
             p.snapRotate, p.snapScale, p.compact ? 1 : 0, p.colorPickerSize, p.gameAspect, p.playMenus ? 1 : 0);
    out += buf;
    if (p.window.width > 0) {
        snprintf(buf, sizeof(buf), "window %d %d %d %d %d\n", p.window.x, p.window.y, p.window.width, p.window.height, p.window.maximized ? 1 : 0);
        out += buf;
    }
    createDirectories(editorUserDirectory());
    return writeTextFile(settingsPath(), out);
}

bool drawSnapSettings(Preferences& p) {
    bool changed = false;
    const auto number = [](const char* label) {
        std::string hidden = propertyRow(label);
        limitNextWidth(12.0f);
        return hidden;
    };
    changed |= ImGui::InputFloat(number(tr("Move snap (m)")).c_str(), &p.snapMove, 0.25f, 1.0f, "%.2f");
    changed |= ImGui::InputFloat(number(tr("Rotate snap (degrees)")).c_str(), &p.snapRotate, 5.0f, 15.0f, "%.1f");
    changed |= ImGui::InputFloat(number(tr("Scale snap")).c_str(), &p.snapScale, 0.05f, 0.25f, "%.2f");
    p.snapMove = std::clamp(p.snapMove, 0.01f, 100.0f);
    p.snapRotate = std::clamp(p.snapRotate, 0.1f, 180.0f);
    p.snapScale = std::clamp(p.snapScale, 0.001f, 10.0f);
    return changed;
}

bool PreferencesPanel::drawContent(Preferences& p) {
    bool changed = false;
    const auto slider = [](const char* label) {
        std::string hidden = propertyRow(label);
        limitNextWidth(16.0f);
        return hidden;
    };
    ImGui::SeparatorText(tr("Viewport"));
    changed |= ImGui::SliderFloat(slider(tr("Camera speed")).c_str(), &p.cameraSpeed, 1.0f, 100.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    changed |= drawSnapSettings(p);
    ImGui::SeparatorText(tr("Interface"));
    changed |= ImGui::SliderFloat(slider(tr("Text size")).c_str(), &p.uiScale, 0.75f, 2.0f, "%.2fx");
    changed |= checkbox(propertyRow(tr("Compact rows")).c_str(), &p.compact);
    changed |= checkbox(propertyRow(tr("Save the window layout per project"), tr("Takes effect the next time a project opens")).c_str(), &p.projectLayout);
    ImGui::SeparatorText(tr("Files"));
    int seconds = static_cast<int>(p.autosaveSeconds);
    const std::string autosave = propertyRow(tr("Autosave every (seconds, 0: off)"));
    limitNextWidth(12.0f);
    if (ImGui::InputInt(autosave.c_str(), &seconds, 30)) {
        p.autosaveSeconds = static_cast<u32>(std::clamp(seconds, 0, 3600));
        changed = true;
    }
    if (ImGui::IsWindowAppearing()) snprintf(m_editor, sizeof(m_editor), "%s", p.externalEditor.c_str());
    ImGui::InputText(propertyRow(tr("Code editor")).c_str(), m_editor, sizeof(m_editor));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        p.externalEditor = m_editor;
        changed = true;
    }
    if (changed) savePreferences(p);
    return changed;
}

} // namespace mealkit::editor
