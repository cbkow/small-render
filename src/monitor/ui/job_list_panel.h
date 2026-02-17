#pragma once

#include <string>
#include <set>
#include <map>
#include <chrono>

namespace SR {

class MonitorApp; // forward

class JobListPanel
{
public:
    void init(MonitorApp* app);
    void render();
    bool visible = true;

private:
    MonitorApp* m_app = nullptr;

    // Multi-select
    std::set<std::string> m_selectedJobIds;
    int m_lastClickedIndex = -1;
    bool m_pendingBulkDelete = false;
    bool m_pendingBulkCancel = false;

    // Progress cache
    struct JobProgress { int completed = 0; int total = 0; };
    std::map<std::string, JobProgress> m_progressCache;
    std::chrono::steady_clock::time_point m_lastProgressScan{};
    static constexpr int PROGRESS_SCAN_COOLDOWN_MS = 5000;
    void scanJobProgress();
};

} // namespace SR
