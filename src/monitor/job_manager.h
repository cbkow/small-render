#pragma once

#include "core/job_types.h"

#include <filesystem>
#include <vector>
#include <string>
#include <chrono>
#include <mutex>
#include <thread>
#include <atomic>

namespace SR {

class JobManager
{
public:
    ~JobManager();

    JobManager() = default;
    JobManager(const JobManager&) = delete;
    JobManager& operator=(const JobManager&) = delete;

    // Start background scanning thread. First scan is synchronous.
    void start(const std::filesystem::path& farmPath);

    // Stop background thread.
    void stop();

    // Thread-safe snapshot for UI / DispatchManager
    std::vector<JobInfo> getJobSnapshot() const;

    std::string submitJob(const std::filesystem::path& farmPath,
                          const JobManifest& manifest, int priority);

    bool writeStateEntry(const std::filesystem::path& farmPath,
                         const std::string& jobId,
                         const std::string& state,
                         int priority,
                         const std::string& nodeId);

    void invalidate();

private:
    void threadFunc();
    std::vector<JobInfo> doScan();

    std::filesystem::path m_farmPath;
    std::vector<JobInfo> m_jobs;
    mutable std::mutex m_mutex;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_invalidated{true};
    std::thread m_thread;
    static constexpr int SCAN_COOLDOWN_MS = 3000;
};

} // namespace SR
