#include "mealkit/editor/PerfPanel.h"

#include "mealkit/editor/Strings.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <vector>

namespace mealkit::editor {

void PerfPanel::push(const game::FrameSample& sample) {
    m_samples.push_back(sample);
    if (m_samples.size() > kHistory) m_samples.pop_front();
}

bool PerfPanel::overBudget() const {
    if (m_samples.empty()) return false;
    const game::FrameSample& s = m_samples.back();
    return (s.hasGpuTime && s.gpuMs > kGpuMs) || (s.hasGame && s.simMsPerTick > kSimMsPerTick) ||
           (s.hasRenderer && s.drawCalls > kDrawCalls) || (s.hasSession && s.sentBytesPerSec / 1024.0 > kNetKBps);
}

void PerfPanel::drawContent() {
    if (m_samples.empty()) {
        ImGui::TextDisabled("%s", tr("Collecting..."));
        return;
    }
    const game::FrameSample& last = m_samples.back();
    const ImVec4 red(1.0f, 0.45f, 0.4f, 1.0f), ok(0.6f, 0.88f, 0.6f, 1.0f);
    // One row: name, current value / budget, graph of the last 30 seconds (the budget line is the graph's height reference)
    const auto row = [&](const char* name, bool has, const char* unit, f64 budget,
                         const std::function<f64(const game::FrameSample&)>& get) {
        if (!has) return;
        std::vector<float> values;
        values.reserve(m_samples.size());
        f64 peak = budget;
        for (const game::FrameSample& s : m_samples) {
            values.push_back(static_cast<float>(get(s)));
            peak = std::max(peak, get(s));
        }
        const f64 now = get(last);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(name);
        ImGui::TableNextColumn();
        ImGui::TextColored(now > budget ? red : ok, "%.2f %s", now, unit);
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%.0f %s", budget, unit);
        ImGui::TableNextColumn();
        const ImVec2 size(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight() * 1.6f);
        ImGui::PushID(name);
        ImGui::PlotLines("##plot", values.data(), static_cast<int>(values.size()), 0, nullptr, 0.0f,
                         static_cast<float>(peak * 1.1), size);
        // Budget line
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        const f32 y = max.y - (max.y - min.y) * static_cast<f32>(budget / (peak * 1.1));
        ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), ImGui::GetColorU32(red), 1.0f);
        ImGui::PopID();
    };
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##perf", 4, flags)) {
        ImGui::TableSetupColumn(tr("Item"));
        ImGui::TableSetupColumn(tr("Now"));
        ImGui::TableSetupColumn(tr("Budget"));
        ImGui::TableSetupColumn(tr("Last 30 s"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        row(tr("Frame (CPU)"), true, "ms", kFrameMs, [](const game::FrameSample& s) { return s.frameMs; });
        row(tr("Frame (GPU)"), last.hasGpuTime, "ms", kGpuMs, [](const game::FrameSample& s) { return s.gpuMs; });
        row(tr("Simulation tick"), last.hasGame, "ms", kSimMsPerTick,
            [](const game::FrameSample& s) { return s.simMsPerTick; });
        row(tr("Draw calls"), last.hasRenderer, "", kDrawCalls,
            [](const game::FrameSample& s) { return static_cast<f64>(s.drawCalls); });
        row(tr("Network sent"), last.hasSession, "KB/s", kNetKBps,
            [](const game::FrameSample& s) { return s.sentBytesPerSec / 1024.0; });
        ImGui::EndTable();
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s", tr("Budgets: docs/ROADMAP.md \"Performance budget\" (minimum spec, 1280x720, 60 fps). Values are from this PC."));
    if (last.hasRenderer)
        ImGui::TextDisabled(tr("%u instances, %u culled, worst frame %.1f ms, 1%% low %.1f ms"), last.instances, last.culled,
                            last.worstFrameMs, last.p99FrameMs);
}

} // namespace mealkit::editor
