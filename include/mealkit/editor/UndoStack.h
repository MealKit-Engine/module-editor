#pragma once

#include <utility>
#include <vector>

// Undo for panels that keep a document as text snapshots (tuning, tables, feedback).
// A panel calls mark() when its data changes and settle() each frame after drawing: one drag or typed value becomes one step,
// because the snapshot is only taken once no widget is active. reset() is for a document that was just read or rebuilt.
namespace mealkit::editor {

template <class T>
class UndoStack {
public:
    void reset(T now) {
        m_settled = std::move(now);
        m_undo.clear();
        m_redo.clear();
        m_pending = false;
    }
    void mark() { m_pending = true; }

    template <class Capture>
    void settle(bool widgetActive, Capture capture) {
        if (!m_pending || widgetActive) return;
        m_undo.push_back(std::move(m_settled));
        if (m_undo.size() > kLimit) m_undo.erase(m_undo.begin());
        m_settled = capture();
        m_redo.clear();
        m_pending = false;
    }

    bool canStep(bool back) const { return !(back ? m_undo : m_redo).empty(); }
    // Moves one step and gives the snapshot to restore. false if there's nothing to do.
    bool step(bool back, T& out) {
        std::vector<T>& from = back ? m_undo : m_redo;
        if (from.empty()) return false;
        (back ? m_redo : m_undo).push_back(std::move(m_settled));
        m_settled = std::move(from.back());
        from.pop_back();
        out = m_settled;
        m_pending = false;
        return true;
    }

private:
    static constexpr size_t kLimit = 100;
    T m_settled{};
    std::vector<T> m_undo, m_redo;
    bool m_pending = false;
};

} // namespace mealkit::editor
