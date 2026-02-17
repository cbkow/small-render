#pragma once

#include "core/job_types.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

namespace SR {

class UIDataCache
{
public:
    ~UIDataCache();

    UIDataCache() = default;
    UIDataCache(const UIDataCache&) = delete;
    UIDataCache& operator=(const UIDataCache&) = delete;

    void start(const std::filesystem::path& farmPath);
    void stop();

    // Main thread pushes context each frame
    void setSelectedJobId(const std::string& jobId);
    void setJobIds(const std::vector<std::string>& ids);
    void setLogRequest(const std::string& mode,
                       const std::vector<std::string>& nodeIds);

    // Coordinator shortcut: inject dispatch tables (avoids disk read)
    void setDispatchTables(const std::map<std::string, DispatchTable>& tables);

    // Main thread reads snapshots
    struct JobProgress { int completed = 0; int total = 0; int rendering = 0; int failed = 0; };
    std::map<std::string, JobProgress> getProgressSnapshot() const;

    struct FrameStateSnapshot
    {
        std::string jobId;
        std::vector<std::pair<int, std::string>> frameStates;  // frame → state string
        std::vector<DispatchChunk> chunks;
    };
    FrameStateSnapshot getFrameStateSnapshot() const;

    struct TaskOutputLine
    {
        std::string text;
        bool isHeader = false;
    };
    struct TaskOutputSnapshot
    {
        std::string jobId;
        std::vector<TaskOutputLine> lines;
    };
    TaskOutputSnapshot getTaskOutputSnapshot() const;

    struct RemoteLogSnapshot
    {
        std::string cacheKey;
        std::vector<std::string> lines;
    };
    RemoteLogSnapshot getRemoteLogSnapshot() const;

private:
    void threadFunc();
    void scanProgress();
    void scanFrameStates(bool force = false);
    void scanTaskOutput(bool force = false);
    void scanRemoteLogs();

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    mutable std::mutex m_mutex;
    std::filesystem::path m_farmPath;

    // Input context (written by main thread under lock)
    std::string m_selectedJobId;
    std::vector<std::string> m_jobIds;
    std::string m_logMode;
    std::vector<std::string> m_logNodeIds;
    bool m_hasCoordinatorTables = false;
    std::map<std::string, DispatchTable> m_coordinatorTables;

    // Output snapshots (written by bg thread under lock)
    std::map<std::string, JobProgress> m_progress;
    FrameStateSnapshot m_frameStates;
    TaskOutputSnapshot m_taskOutput;
    RemoteLogSnapshot m_remoteLogs;

    // Wake flag: set by main thread to break bg thread out of sleep early
    std::atomic<bool> m_wakeFlag{false};

    // Scan timers (bg thread only, no lock needed)
    std::chrono::steady_clock::time_point m_lastProgressScan{};
    std::chrono::steady_clock::time_point m_lastFrameScan{};
    std::chrono::steady_clock::time_point m_lastTaskOutputScan{};
    std::chrono::steady_clock::time_point m_lastRemoteLogScan{};
};

} // namespace SR
