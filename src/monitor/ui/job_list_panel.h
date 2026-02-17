#pragma once

#include <string>
#include <set>

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
    bool m_pendingCancelAll = false;
};

} // namespace SR
