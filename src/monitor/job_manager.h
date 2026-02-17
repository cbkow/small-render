#pragma once

#include "core/job_types.h"

#include <filesystem>
#include <vector>
#include <string>
#include <chrono>
#include <mutex>

namespace SR {

class JobManager
{
public:
    void scan(const std::filesystem::path& farmPath);
    const std::vector<JobInfo>& jobs() const { return m_jobs; }

    // Thread-safe snapshot for DispatchManager
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
    void scanImpl(const std::filesystem::path& farmPath);

    std::vector<JobInfo> m_jobs;
    mutable std::mutex m_mutex;
    std::chrono::steady_clock::time_point m_lastScan{};
    static constexpr int SCAN_COOLDOWN_MS = 3000;
    bool m_invalidated = true;
};

} // namespace SR
