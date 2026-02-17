#pragma once

#include "monitor/ui/settings_panel.h"
#include "monitor/ui/node_panel.h"
#include "monitor/ui/job_list_panel.h"
#include "monitor/ui/job_detail_panel.h"
#include "monitor/ui/log_panel.h"
#include "monitor/ui/farm_cleanup_dialog.h"

namespace SR {

class MonitorApp; // forward

class Dashboard
{
public:
    void init(MonitorApp* app);
    void render();

private:
    void renderMenuBar();
    void buildDefaultLayout();

    MonitorApp* m_app = nullptr;
    bool m_firstFrame = true;

    // Panels
    SettingsPanel      m_settingsPanel;
    NodePanel          m_nodePanel;
    JobListPanel       m_jobListPanel;
    JobDetailPanel     m_jobDetailPanel;
    LogPanel           m_logPanel;
    FarmCleanupDialog  m_farmCleanupDialog;

    bool m_showSettings = false;
};

} // namespace SR
