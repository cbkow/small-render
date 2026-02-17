#pragma once

#include "core/job_types.h"
#include "core/heartbeat.h"
#include "core/config.h"
#include "monitor/command_manager.h"

#include <filesystem>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <queue>
#include <functional>
#include <chrono>

namespace SR {

class DispatchManager
{
public:
    DispatchManager() = default;

    void start(const std::filesystem::path& farmPath,
               const std::string& nodeId,
               const std::string& nodeOS,
               const TimingConfig& timing,
               const std::vector<std::string>& tags,
               std::function<std::vector<NodeInfo>()> nodeSnapshotFn,
               std::function<std::vector<JobInfo>()> jobSnapshotFn);
    void stop();

    // Main-thread dispatch cycle — called from MonitorApp::update()
    void update();

    // Route worker reports (chunk_completed, chunk_failed) from CommandManager
    void processAction(const CommandManager::Action& action);

    // Queue local completion from own RenderCoordinator
    void queueLocalCompletion(const std::string& jobId, const ChunkRange& chunk,
                              const std::string& state);

    // Handle job state changes (pause/cancel/resume) from job controls
    void handleJobStateChange(const std::string& jobId, const std::string& newState);

    // Manual chunk controls (coordinator-only, called from UI)
    void reassignChunk(const std::string& jobId, int frameStart, int frameEnd);
    void retryFailedChunk(const std::string& jobId, int frameStart, int frameEnd);

    // Set callback for dispatching to local RenderCoordinator
    using DispatchCallback = std::function<void(const JobManifest&, const ChunkRange&)>;
    void setLocalDispatchCallback(DispatchCallback fn);

    // Set callback for sending commands to workers
    using CommandSenderFn = std::function<void(const std::string& target,
                                               const std::string& type,
                                               const std::string& jobId,
                                               const std::string& reason,
                                               int frameStart, int frameEnd)>;
    void setCommandSender(CommandSenderFn fn);

    // Live config updates
    void updateTiming(const TimingConfig& timing);
    void updateTags(const std::vector<std::string>& tags);
    void setNodeActive(bool active);

    bool isRunning() const { return m_running; }

private:
    // Dispatch cycle steps
    void processLocalCompletions();
    void processWorkerReports();
    void detectDeadWorkers();
    void checkJobCompletions();
    void assignWork();
    void writeDispatchTables();

    // Helpers
    bool isNodeIdle(const std::string& nodeId, const std::vector<NodeInfo>& nodes) const;
    bool isNodeDead(const std::string& nodeId, const std::vector<NodeInfo>& nodes) const;
    bool hasOSCmd(const JobManifest& manifest, const std::string& nodeOS) const;
    bool hasRequiredTags(const std::vector<std::string>& required,
                         const std::vector<std::string>& nodeTags) const;
    void initDispatchTable(const std::string& jobId, const JobManifest& manifest);
    void markDirty(const std::string& jobId);
    int64_t nowMs() const;

    // Recovery
    void recoverFromDisk(const std::vector<JobInfo>& jobs);

    // Config
    std::filesystem::path m_farmPath;
    std::string m_nodeId;
    std::string m_nodeOS;
    TimingConfig m_timing;
    std::vector<std::string> m_tags;
    bool m_running = false;
    bool m_nodeActive = true;
    bool m_recovered = false;

    // Callbacks
    std::function<std::vector<NodeInfo>()> m_nodeSnapshotFn;
    std::function<std::vector<JobInfo>()> m_jobSnapshotFn;
    DispatchCallback m_localDispatchFn;
    CommandSenderFn m_commandSenderFn;

    // Current assignments: nodeId -> (jobId, chunk, timestamp)
    struct Assignment
    {
        std::string jobId;
        ChunkRange chunk;
        int64_t assignedAtMs = 0;
    };
    std::map<std::string, Assignment> m_assignments;

    // In-memory dispatch tables: jobId -> DispatchTable
    std::map<std::string, DispatchTable> m_dispatchTables;

    // Jobs needing dispatch.json re-write
    std::set<std::string> m_dirtyTables;

    // Local completion queue (from own RenderCoordinator)
    struct CompletionEntry
    {
        std::string jobId;
        ChunkRange chunk;
        std::string state;
    };
    std::queue<CompletionEntry> m_localCompletionQueue;

    // Worker reports (from CommandManager)
    std::queue<CommandManager::Action> m_workerReports;

    // Write throttle
    std::chrono::steady_clock::time_point m_lastWrite{};
    static constexpr int WRITE_THROTTLE_MS = 2000;

    // Jobs already marked completed (avoid duplicate state writes)
    std::set<std::string> m_completionWritten;
};

} // namespace SR
