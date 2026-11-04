#pragma once

#include "mealkit/game/FrameStats.h"

#include <deque>

// [Performance] window: accumulates a FrameSample every game::FrameStats report period (0.25 s) and graphs the last 30 seconds,
// shown alongside the limits from the docs/ROADMAP.md "Performance budget" table (red when over). Numbers are FrameSample as is.
namespace mealkit::editor {

class PerfPanel {
public:
    static constexpr size_t kHistory = 120; // 0.25 s × 120 = 30 s

    // Budget (ROADMAP "Performance budget", min spec 1280×720 60 fps)
    static constexpr f64 kFrameMs = 1000.0 / 60.0;
    static constexpr f64 kGpuMs = 8.0;
    static constexpr f64 kSimMsPerTick = 4.0;
    static constexpr f64 kDrawCalls = 200.0;
    static constexpr f64 kNetKBps = 10.0; // host send

    void push(const game::FrameSample& sample);
    // Some item is over budget (status bar fps color)
    bool overBudget() const;
    void drawContent(); // the Performance page of the [Debug] window

private:
    std::deque<game::FrameSample> m_samples;
};

} // namespace mealkit::editor
