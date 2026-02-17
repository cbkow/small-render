#pragma once

#include <string>
#include <vector>

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

    void renderTaskOutput();

    bool m_autoScroll = true;
};

} // namespace SR
