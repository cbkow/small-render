#pragma once

#include "core/config.h"
#include "core/node_identity.h"
#include "monitor/agent_supervisor.h"
#include "monitor/heartbeat_manager.h"
#include "monitor/dispatch_manager.h"
#include "monitor/render_coordinator.h"
#include "monitor/template_manager.h"
#include "monitor/job_manager.h"
#include "monitor/command_manager.h"
#include "monitor/submission_manager.h"
#include "monitor/ui_data_cache.h"
#include "core/udp_notify.h"
#include "core/message_dedup.h"
#include "monitor/ui/dashboard.h"

#include "core/system_tray.h"

#include <filesystem>
#include <chrono>
#include <memory>
#include <string>

namespace SR {

enum class NodeState { Active, Stopped };

class MonitorApp
{
public:
    // Phase 1 bootstrap: load node_id, load config, create dirs
    bool init();

    // Called each frame — logic only (messages, commands, scans)
    void update();

    // Called each frame — UI rendering (only when window is visible, inside ImGui frame)
    void renderUI();

    // Save config and clean up
    void shutdown();

    // Config accessors
    Config& config() { return m_config; }
    const Config& config() const { return m_config; }
    const NodeIdentity& identity() const { return m_identity; }
    AgentSupervisor& agentSupervisor() { return m_agentSupervisor; }
    HeartbeatManager& heartbeatManager() { return m_heartbeatManager; }
    DispatchManager& dispatchManager() { return m_dispatchManager; }
    TemplateManager& templateManager() { return m_templateManager; }
    JobManager& jobManager() { return m_jobManager; }
    RenderCoordinator& renderCoordinator() { return m_renderCoordinator; }
    CommandManager& commandManager() { return m_commandManager; }

    // Cached snapshots (refreshed each frame from bg threads, zero FS)
    const std::vector<JobInfo>& cachedJobs() const { return m_cachedJobs; }
    const std::vector<JobTemplate>& cachedTemplates() const { return m_cachedTemplates; }

    // UIDataCache accessor
    UIDataCache& uiDataCache() { return *m_uiDataCache; }
    const UIDataCache& uiDataCache() const { return *m_uiDataCache; }

    // Job controls (called from UI)
    void pauseJob(const std::string& jobId);
    void resumeJob(const std::string& jobId);
    void cancelJob(const std::string& jobId);
    void requeueJob(const std::string& jobId);
    void deleteJob(const std::string& jobId);

    // Chunk controls (coordinator-only, called from UI)
    void reassignChunk(const std::string& jobId, int frameStart, int frameEnd);
    void retryFailedChunk(const std::string& jobId, int frameStart, int frameEnd);

    // Node state controls
    void setNodeState(NodeState state);
    NodeState nodeState() const { return m_nodeState; }

    // Coordinator queries
    bool isCoordinator() const { return m_config.is_coordinator; }
    std::string findCoordinatorNodeId() const;

    // Tray state (called each frame by main.cpp)
    TrayIconState trayState() const;
    std::string trayTooltip() const;
    std::string trayStatusText() const;

    // Exit flow
    void requestExit();
    bool isExitPending() const { return m_exitRequested && !m_shouldExit; }
    bool shouldExit() const { return m_shouldExit; }
    void beginForceExit();
    void cancelExit();

    void saveConfig();

    // Phase 2: farm lifecycle
    bool startFarm();
    void stopFarm();
    bool isFarmRunning() const { return m_farmRunning; }
    const std::filesystem::path& farmPath() const { return m_farmPath; }
    bool hasFarmError() const { return !m_farmError.empty(); }
    const std::string& farmError() const { return m_farmError; }

    // Job selection state
    void selectJob(const std::string& id);
    void requestSubmissionMode();
    const std::string& selectedJobId() const { return m_selectedJobId; }
    bool shouldEnterSubmission();   // returns and clears the flag (one-shot)

    // CLI submission support
    struct SubmitRequest
    {
        std::string file;
        std::string templateId;
    };
    void setPendingSubmitRequest(const std::string& file, const std::string& templateId);
    bool hasPendingSubmitRequest() const { return !m_pendingSubmitRequest.file.empty(); }
    SubmitRequest consumeSubmitRequest();   // returns and clears

private:
    void loadConfig();
    void checkSubmitRequest();  // Poll for CLI submission signal file

    // UDP multicast fast path
    void handleUdpMessages();
    void sendUdpHeartbeat();
    void processAction(const CommandManager::Action& action);

    // Worker-side: handle assign_chunk from coordinator
    void handleAssignChunk(const CommandManager::Action& action);

    // Worker-side: retry sending buffered completions to coordinator
    void flushPendingCompletions();

    std::filesystem::path m_appDataDir;
    std::filesystem::path m_configPath;

    NodeIdentity m_identity;
    Config m_config;
    AgentSupervisor m_agentSupervisor;
    HeartbeatManager m_heartbeatManager;
    DispatchManager m_dispatchManager;
    TemplateManager m_templateManager;
    JobManager m_jobManager;
    RenderCoordinator m_renderCoordinator;
    CommandManager m_commandManager;
    SubmissionManager m_submissionManager;
    std::unique_ptr<UIDataCache> m_uiDataCache;
    UdpNotify m_udpNotify;
    MessageDedup m_dedup;
    std::chrono::steady_clock::time_point m_lastUdpHeartbeat{};
    std::chrono::steady_clock::time_point m_lastDedupPurge{};
    Dashboard m_dashboard;

    // Cached snapshots (refreshed each frame from bg threads)
    std::vector<JobInfo> m_cachedJobs;
    std::vector<JobTemplate> m_cachedTemplates;

    // Farm state
    std::filesystem::path m_farmPath;
    std::string m_farmError;
    bool m_farmRunning = false;
    NodeState m_nodeState = NodeState::Active;

    // Job selection
    std::string m_selectedJobId;
    bool m_requestSubmission = false;

    // CLI submission
    SubmitRequest m_pendingSubmitRequest;

    // Worker-side: buffered completions when coordinator is offline
    struct PendingCompletion { std::string jobId; ChunkRange chunk; std::string state; };
    std::vector<PendingCompletion> m_pendingCompletions;

    // Worker-side: deferred assignments waiting for manifest to propagate
    struct DeferredAssignment
    {
        CommandManager::Action action;
        int retryCount = 0;
        std::chrono::steady_clock::time_point nextRetry;
    };
    std::vector<DeferredAssignment> m_deferredAssignments;
    void processDeferredAssignments();

    // Exit state
    bool m_exitRequested = false;
    bool m_shouldExit = false;
};

} // namespace SR
