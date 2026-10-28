#pragma once

#include "mealkit/core/Log.h"
#include "mealkit/core/Types.h"

#include <deque>
#include <mutex>
#include <string>

// [로그] 창: 엔진·게임 DLL의 MEALKIT_INFO/WARN/ERROR를 모아 레벨별로 거르고 찾는다 (stdout에도 그대로 나간다).
namespace mealkit::editor {

class LogPanel {
public:
    static constexpr const char* kWindow = "Log###Log";

    LogPanel();
    ~LogPanel();
    LogPanel(const LogPanel&) = delete;
    LogPanel& operator=(const LogPanel&) = delete;

    bool* openFlag() { return &m_open; }
    void setCodeEditor(const std::string& editor) { m_codeEditor = editor; }
    void draw();

private:
    struct Entry {
        log::Level level;
        f64 seconds;
        std::string message;
    };
    static void sink(log::Level level, double seconds, const char* message);

    std::mutex m_lock; // 다른 스레드의 로그가 들어온다
    std::deque<Entry> m_entries;
    u32 m_counts[3] = {};
    bool m_added = false;
    bool m_open = true;
    bool m_show[3] = {true, true, true};
    bool m_follow = true; // 맨 아래에 있으면 새 줄을 따라간다
    char m_search[128] = {};
    std::string m_codeEditor;
};

} // namespace mealkit::editor
