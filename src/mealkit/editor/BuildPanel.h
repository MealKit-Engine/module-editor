#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/editor/Project.h"
#include "mealkit/platform/Process.h"

#include <string>

// Editor project builds and the [Build Log] window: runs the project's build.bat (game code, shipping) or the engine's upgrade_project.ps1
// without a window and collects output. What to save before building and what to do after (reload DLL, rebuild) is decided by the editor.
namespace mealkit::editor {

enum class BuildKind {
    GameCode, // build.bat Editor: game DLL (the app swaps it in when done)
    Export,   // build.bat Export: shipping exe + assets → export\<name>
    Upgrade,  // engine's tools\upgrade_project.ps1: project template files to the current engine version (then a game code build)
};

class BuildPanel {
public:

    // Starts running (false if already building). now: editor time (for showing elapsed time)
    bool start(BuildKind kind, const ProjectInfo& project, const std::string& engineDirectory, f64 now);
    // Collects output. true if the build finished this time (see the result with kind()/succeeded())
    bool poll(const ProjectInfo& project, f64 now);
    bool running() const { return m_process.running(); }
    BuildKind kind() const { return m_kind; }
    bool succeeded() const { return m_succeeded; }
    // The last build stopped because the engine editor build failed (tools\build.ps1's message): usually this editor holds mealkit.dll
    bool engineBuildFailed() const { return !m_succeeded && m_log.find("Engine editor build failed") != std::string::npos; }

    bool takeOpenRequest() { const bool open = m_open; m_open = false; return open; } // a build wants its log seen
    // Status bar "Building... N s" (draws nothing and returns false when not building)
    bool drawStatus(f64 now) const;
    // [Build Log] window. Puts the build chosen by button into request and returns true (the editor starts it after saving)
    bool drawContent(bool editing, const ProjectInfo& project, f64 now, BuildKind* request);
    std::string takeStatus() { return std::move(m_status); }
    void setCodeEditor(const std::string& editor) { m_codeEditor = editor; } // clicking a "file(line)" in the log opens it

private:
    ChildProcess m_process;
    BuildKind m_kind = BuildKind::GameCode;
    bool m_succeeded = false;
    std::string m_log;
    bool m_open = false;
    bool m_scroll = false;
    f64 m_start = 0.0;
    int m_step = 0, m_steps = 0; // ninja's last "[n/N]" line, shown beside the elapsed time
    std::string m_status;
    std::string m_codeEditor;
};

} // namespace mealkit::editor
