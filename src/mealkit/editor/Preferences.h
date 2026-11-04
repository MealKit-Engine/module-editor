#pragma once

#include "mealkit/core/Types.h"

#include <string>

// Editor preferences: per user (%LOCALAPPDATA%\MealKit\editor_settings.txt, "key value" lines). Independent of projects.
// Other lines in the same file (language, toolPath) are read and written by those features and not touched here.
namespace mealkit::editor {

struct Preferences {
    f32 cameraSpeed = 15.0f;          // camera fly speed when the editor starts (values changed with the wheel aren't saved)
    bool compact = false;             // tighter rows, so more fits on small screens
    f32 uiScale = 1.0f;               // text size scale (multiplied on top of the DPI scale)
    u32 autosaveSeconds = 90;         // interval for writing unsaved changes to <scene>.autosave (0 = off)
    std::string externalEditor = "code"; // editor used for opening code (VS Code style "-g file:line")
    bool projectLayout = true;        // keep the window layout per project in build\editor\layout.ini
    f32 snapMove = 0.5f;              // gizmo snap: move (m, also the floor grid spacing)
    f32 snapRotate = 15.0f;           // rotate (degrees)
    f32 snapScale = 0.1f;             // scale
};

Preferences loadPreferences();
bool savePreferences(const Preferences& prefs);
// Snap unit input fields (preferences window/viewport toolbar). true if changed (the caller saves)
bool drawSnapSettings(Preferences& prefs);

class PreferencesPanel {
public:
    static constexpr const char* kWindow = "Preferences###Preferences";
    bool* openFlag() { return &m_open; }
    // true if a value changed (and it was saved)
    bool draw(Preferences& prefs);

private:
    bool m_open = false;
    char m_editor[260] = {}; // editor path being edited
};

} // namespace mealkit::editor
