#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/editor/BuildProfile.h"
#include "mealkit/editor/Project.h"
#include "mealkit/platform/Process.h"

#include <string>
#include <vector>

// Editor project builds, the [Build Output] window and the [Build Profiles] window: runs the project's build.bat (game code, shipping) or the engine's upgrade_project.ps1
// without a window and collects output. What to save before building and what to do after (reload DLL, rebuild) is decided by the editor.
namespace mealkit::editor {

enum class BuildKind {
    GameCode, // build.bat Editor: game DLL (the app swaps it in when done)
    Export,   // build.bat Export: shipping exe + assets → export/<os>/<name>
    Profile,  // a build profile (profiles/<id>.profile), run as configuration profile:<id>: the one the [Build Profiles] window asked for,
              // else the active one. Without any profile it is an Export
    Upgrade,  // engine's tools\upgrade_project.ps1: project template files to the current engine version (then a game code build)
};

// One compiler error or warning found in the build output (the same message from the same line is kept once, with a count)
struct BuildDiagnostic {
    bool error = true; // false = warning
    std::string file;  // as written in the log
    int line = 0;
    std::string message;
    int count = 1;
};

// Parses one output line: MSVC "file(27): error C2065: msg", GCC/Clang "file:27:5: error: msg". false if it isn't an error or warning
bool parseBuildDiagnostic(const std::string& line, BuildDiagnostic* out);

// What the [Build Profiles] window asked for this frame
struct BuildWindowRequest {
    bool build = false;          // build the profile shown in the window (start(BuildKind::Profile) takes it)
    bool run = false;            // ... and start the game when the build succeeds (Build and Run)
    bool projectChanged = false; // the game section was edited: the editor saves settings to mealkit.project
    game::ProjectSettings settings;
    bool openProjectSettings = false;
};

class BuildPanel {
public:

    // Starts running (false if already building). now: editor time (for showing elapsed time). run: start the game when it succeeds
    bool start(BuildKind kind, const ProjectInfo& project, const std::string& engineDirectory, f64 now, bool run = false);
    // Collects output. true if the build finished this time (see the result with kind()/succeeded())
    bool poll(const ProjectInfo& project, f64 now);
    bool running() const { return m_process.running(); }
    BuildKind kind() const { return m_kind; }
    bool succeeded() const { return m_succeeded; }
    bool cancelled() const { return m_cancelled; } // the last build was stopped by [Cancel] (not a failure)
    int step() const { return m_step; }
    int steps() const { return m_steps; }
    f64 startTime() const { return m_start; }
    const std::string& title() const { return m_title; } // what the last/current build is
    void cancel(); // stops the running build; poll() reports the end
    const std::vector<BuildDiagnostic>& diagnostics() const { return m_diagnostics; }
    int errorCount() const { return m_errors; }
    int warningCount() const { return m_warnings; }
    const std::string& projectDirectory() const { return m_projectDirectory; }
    const std::string& codeEditor() const { return m_codeEditor; }
    // The last build stopped because the engine editor build failed (tools\build.ps1's message): usually this editor holds mealkit.dll
    bool engineBuildFailed() const { return !m_succeeded && m_log.find("Engine editor build failed") != std::string::npos; }

    // Build profiles of the project (read again whenever the window is drawn after loadProfiles, e.g. when a project opens)
    void loadProfiles(const ProjectInfo& project);
    // The active profile (Ctrl+B, Build and Run, the toolbar) is a choice of this user (saved with the editor's panel state); the window
    // shows any profile without changing it, like Unity's Build Profiles. nullptr when the project has none (builds are then Export)
    const BuildProfile* activeProfile() const;
    const std::string& activeProfileId() const { return m_activeId; }
    void setActiveProfileId(const std::string& id) { // a saved choice; once profiles are loaded only an existing one is taken
        if (m_profilesLoaded && !findProfile(id)) return;
        m_activeId = m_viewId = id;
    }
    std::string activeTitle() const; // the active profile's name, or "Release" without profiles
    // The folder [Open Release Folder] opens: the active profile's output, else export/<os>/<name>
    std::string releaseFolder(const ProjectInfo& project) const;
    bool takeSettingsRequest() { const bool open = m_wantSettings; m_wantSettings = false; return open; } // [Build Profiles...] in the output

    bool takeOpenRequest() { const bool open = m_open; m_open = false; return open; } // a build wants its log seen
    // Status bar "Building... N s" (draws nothing and returns false when not building)
    bool drawStatus(f64 now) const;
    // [Build Output] window: the result line ([Retry] / [Cancel], error and warning counts) and the errors, warnings or full log.
    // Puts the build chosen by button into request and returns true (the editor starts it after saving)
    bool drawContent(bool editing, const ProjectInfo& project, f64 now, BuildKind* request);
    // [Build Profiles] window (Unity Build Profiles, Godot Export): profiles on the left; on the right the game's shipping settings
    // (mealkit.project, the same for every profile), the shown profile's fields (saved to its file as they change), its last build
    // and [Set Active] [Build] [Build and Run]. Returns true when out asks for something
    bool drawSettings(bool* open, bool editing, const ProjectInfo& project, f64 now, BuildWindowRequest* out);
    std::string takeStatus() { return std::move(m_status); }
    void setCodeEditor(const std::string& editor) { m_codeEditor = editor; } // clicking a "file(line)" in the log opens it

private:
    void addOutputLine(const std::string& line);
    const BuildProfile* findProfile(const std::string& id) const;
    std::string outputFolder(const ProjectInfo& project, const BuildProfile* profile) const; // nullptr: the plain Export
    std::string runnablePath(const ProjectInfo& project, const BuildProfile* profile) const; // the exe Build and Run starts
    void drawGameSection(bool canEdit, const ProjectInfo& project, BuildWindowRequest* out);
    void drawBuildState(const ProjectInfo& project, const BuildProfile* profile, f64 now);
    ChildProcess m_process;
    bool m_cancelled = false;
    std::vector<BuildDiagnostic> m_diagnostics;
    int m_errors = 0, m_warnings = 0;
    f64 m_elapsed = 0.0; // of the last finished build
    int m_view = 0;      // 0 errors, 1 warnings, 2 full log
    bool m_pickView = false;
    std::string m_projectDirectory;
    std::string m_title; // what the last build was (header line)
    BuildKind m_kind = BuildKind::GameCode;
    bool m_succeeded = false;
    std::string m_log;
    bool m_open = false;
    bool m_scroll = false;
    f64 m_start = 0.0;
    int m_step = 0, m_steps = 0; // ninja's last "[n/N]" line, shown beside the elapsed time
    std::string m_status;
    std::string m_codeEditor;
    std::vector<BuildProfile> m_profiles;
    std::string m_activeId;       // Ctrl+B and Build and Run build this one
    std::string m_viewId;         // shown in the [Build Profiles] window
    std::string m_requestId;      // the window's [Build] asked for this one (taken by the next start)
    std::string m_buildingId;     // the profile of the current/last build ("" = plain Export)
    std::string m_runningProfile; // title of the profile being built (log and status)
    bool m_profilesLoaded = false;
    bool m_runAfter = false;
    std::string m_runPath;        // exe to start when the build succeeds
    bool m_wantSettings = false;
    // Per profile ("" = plain Export): how its last build in this session ended
    struct LastBuild {
        std::string id;
        bool succeeded = false, cancelled = false;
        f64 elapsed = 0.0;
        int errors = 0;
    };
    std::vector<LastBuild> m_lastBuilds;
    // What is in the shown profile's output folder (read again every few seconds while the window is open)
    struct OutputInfo {
        std::string folder;
        std::string when; // the exe's time "11-24 21:03", empty when there is no exe
        unsigned long long bytes = 0;
        f64 checked = -100.0;
    };
    OutputInfo m_outputInfo;
    std::vector<std::string> m_scenes; // assets/**/*.scene ('/' separated, relative to assets) for the shipped scenes list
    f64 m_scenesChecked = -100.0;
    f32 m_footerWidth = 0.0f; // [Set Active] [Build] [Build and Run], measured last frame
};

} // namespace mealkit::editor
