#pragma once

#include <string>
#include <vector>

namespace SR {

class MonitorApp; // forward

class FarmCleanupDialog
{
public:
    void init(MonitorApp* app);
    void render();      // Call every frame — renders modal if open
    void open();        // Trigger scan + open popup

private:
    void scanItems();
    void cleanSelected();

    MonitorApp* m_app = nullptr;
    bool m_shouldOpen = false;

    struct CleanupItem
    {
        std::string id;       // slug, nodeId, or dir path
        std::string label;    // display text
        std::string detail;   // secondary info
        bool selected = false;
    };

    std::vector<CleanupItem> m_completedJobs;
    std::vector<CleanupItem> m_deadNodes;
    std::vector<CleanupItem> m_orphanedDirs;
};

} // namespace SR
