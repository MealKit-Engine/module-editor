#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/editor/Project.h"
#include "mealkit/platform/Process.h"

#include <string>
#include <vector>

namespace mealkit::editor {

// [Debug] > [Tests]: runs the project's tests/*.scenario with <name>_headless (game/Scenario.h), shows failed checks, and accepts
// intended changes with --update-expected. Builds the headless exe first when it's missing or older than the game sources.
// Reads saved files, so unsaved tuning and scene edits aren't tested.
class TestsPanel {
public:
    void runAll(const ProjectInfo& project);
    void poll(const ProjectInfo& project, f64 dt); // every frame, on any page
    void drawContent(const ProjectInfo& project);
    bool running() const { return m_stage != Stage::Idle; }
    u32 failedCount() const;

private:
    enum class State : u8 { NotRun, Queued, Running, Passed, Failed };
    struct Result {
        std::string file; // name in tests/
        State state = State::NotRun;
        bool update = false; // this run rewrites the expected values
        std::string summary;
        std::vector<std::string> failures; // "line 12: tick 300 slots[1].falls == 1 (got 2)"
    };
    enum class Stage : u8 { Idle, Building, Testing };

    void refresh(const ProjectInfo& project);
    void queue(const ProjectInfo& project);
    void startNext(const ProjectInfo& project);
    void finishTest();

    ChildProcess m_process;
    Stage m_stage = Stage::Idle;
    std::vector<Result> m_results;
    int m_current = -1;
    std::string m_output, m_status;
    f64 m_refresh = 0.0;
};

} // namespace mealkit::editor
