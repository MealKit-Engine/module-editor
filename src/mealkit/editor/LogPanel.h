#pragma once

#include "mealkit/core/Log.h"
#include "mealkit/core/Types.h"

#include <deque>
#include <mutex>
#include <string>

// [Log] window: collects MEALKIT_INFO/WARN/ERROR from the engine and game DLL, filters by level and searches (also goes to stdout as is).
namespace mealkit::editor {

class LogPanel {
public:

    LogPanel();
    ~LogPanel();
    LogPanel(const LogPanel&) = delete;
    LogPanel& operator=(const LogPanel&) = delete;

    void setCodeEditor(const std::string& editor) { m_codeEditor = editor; }
    // The Log page of the [Debug] window, in two parts: the problems list goes between them
    void drawFilters();
    void drawLines();
    // Play starts: clears the lines when [Clear on Play] is on
    void onPlayStart() {
        if (!m_clearOnPlay) return;
        std::lock_guard<std::mutex> guard(m_lock);
        m_entries.clear();
        for (u32& c : m_counts) c = 0;
    }

private:
    struct Entry {
        log::Level level;
        f64 seconds;
        std::string message;
    };
    static void sink(log::Level level, double seconds, const char* message);

    std::mutex m_lock; // logs come in from other threads
    std::deque<Entry> m_entries;
    u32 m_counts[3] = {};
    bool m_added = false;
    bool m_show[3] = {true, true, true};
    bool m_group = false;       // same message once, with a count
    bool m_clearOnPlay = false;
    bool m_follow = true; // when at the bottom, follows new lines
    char m_search[128] = {};
    std::string m_codeEditor;
};

} // namespace mealkit::editor
