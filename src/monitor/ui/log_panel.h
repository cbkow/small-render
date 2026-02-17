#pragma once

#include <string>
#include <vector>
#include <chrono>

namespace SR {

class MonitorApp; // forward

class LogPanel
{
public:
    void init(MonitorApp* app);
    void render();
    bool visible = true;

private:
    MonitorApp* m_app = nullptr;

    // Mode
    enum class Mode { MonitorLog, TaskOutput };
    Mode m_mode = Mode::MonitorLog;

    // Monitor log filter: 0=This Node, 1=All Nodes, 2+=specific peer
    int m_filterIdx = 0;
    std::vector<std::string> m_peerNodeIds;
    std::vector<std::string> m_peerHostnames;

    // Remote log cache
    std::vector<std::string> m_remoteLines;
    std::string m_remoteCacheKey;
    std::chrono::steady_clock::time_point m_lastRemoteLoad{};
    static constexpr int REMOTE_RELOAD_MS = 5000;

    // Task output state
    struct TaskOutputLine
    {
        std::string text;
        bool isHeader = false;
    };
    std::string m_taskOutputJobId;
    std::vector<TaskOutputLine> m_taskOutputLines;
    std::chrono::steady_clock::time_point m_lastTaskOutputLoad{};
    static constexpr int TASK_OUTPUT_RELOAD_MS = 3000;

    void renderTaskOutput();

    bool m_autoScroll = true;
};

} // namespace SR
