#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/editor/Project.h"
#include "mealkit/platform/Process.h"

#include <map>
#include <string>
#include <vector>

namespace mealkit::editor {

// [Debug] > [Balance]: runs bot matches with <name>_headless (--seed 1..N --exit-tick --final-state) and shows each state number's
// average, lowest and highest over the matches. A second tuning file gives an A/B comparison over the same seeds.
// Values inside a list ("slots[].shoves") are added up per match, so unused slots don't change them.
class BalancePanel {
public:
    void run(const ProjectInfo& project);
    void poll(const ProjectInfo& project);
    void drawContent(const ProjectInfo& project);
    bool running() const { return m_stage != Stage::Idle; }

private:
    enum class Stage : u8 { Idle, Building, Running };
    struct Stats {
        std::vector<f64> values[2]; // per match; [1] = compared tuning
    };
    void startMatch(const ProjectInfo& project);
    void finishMatch();

    ChildProcess m_process;
    Stage m_stage = Stage::Idle;
    int m_matches = 20;
    float m_seconds = 180.0f;
    std::string m_compare; // tuning file under assets, empty = no comparison
    std::string m_filter;
    bool m_changedOnly = false;
    int m_done = 0;   // matches finished in this run (both sets)
    int m_failed = 0;
    std::string m_output, m_status, m_lastError;
    std::map<std::string, Stats> m_stats;
};

} // namespace mealkit::editor
