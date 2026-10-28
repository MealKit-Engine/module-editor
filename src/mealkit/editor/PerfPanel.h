#pragma once

#include "mealkit/game/FrameStats.h"

#include <deque>

// [성능] 창: game::FrameStats의 보고 주기(0.25초)마다 FrameSample을 쌓아 최근 30초를 그래프로,
// docs/ROADMAP.md "성능 예산" 표의 한도와 같이 보여 준다(넘으면 빨강). 숫자는 FrameSample 그대로다.
namespace mealkit::editor {

class PerfPanel {
public:
    static constexpr const char* kWindow = "Performance###Performance";
    static constexpr size_t kHistory = 120; // 0.25초 × 120 = 30초

    // 예산 (ROADMAP "성능 예산", 최소 사양 1280×720 60 fps)
    static constexpr f64 kFrameMs = 1000.0 / 60.0;
    static constexpr f64 kGpuMs = 8.0;
    static constexpr f64 kSimMsPerTick = 4.0;
    static constexpr f64 kDrawCalls = 200.0;
    static constexpr f64 kNetKBps = 10.0; // 호스트 송신

    void push(const game::FrameSample& sample);
    bool* openFlag() { return &m_open; }
    // 예산을 넘은 항목이 있다 (상태 표시줄 fps 색)
    bool overBudget() const;
    void draw();

private:
    std::deque<game::FrameSample> m_samples;
    bool m_open = false;
};

} // namespace mealkit::editor
