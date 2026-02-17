#include "monitor/job_manager.h"
#include "core/atomic_file_io.h"
#include "core/platform.h"

#include "core/monitor_log.h"

#include <algorithm>
#include <chrono>

namespace SR {

void JobManager::scan(const std::filesystem::path& farmPath)
{
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastScan).count();
    if (elapsed < SCAN_COOLDOWN_MS && !m_invalidated)
        return;
    m_lastScan = now;
    m_invalidated = false;

    try
    {
        scanImpl(farmPath);
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("job", std::string("Scan exception: ") + e.what());
    }
    catch (...)
    {
        MonitorLog::instance().error("job", "Scan unknown exception");
    }
}

void JobManager::scanImpl(const std::filesystem::path& farmPath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_jobs.clear();

    namespace fs = std::filesystem;
    std::error_code ec;
    auto jobsDir = farmPath / "jobs";
    if (!fs::is_directory(jobsDir, ec))
        return;

    for (const auto& entry : fs::directory_iterator(jobsDir, ec))
    {
        if (!entry.is_directory(ec))
            continue;

        auto manifestPath = entry.path() / "manifest.json";
        if (!fs::exists(manifestPath, ec))
            continue;

        auto data = AtomicFileIO::safeReadJson(manifestPath);
        if (!data.has_value())
            continue;

        try
        {
            JobInfo info;
            info.manifest = data.value().get<JobManifest>();

            // Read latest state from state/ directory
            auto stateDir = entry.path() / "state";
            if (fs::is_directory(stateDir, ec))
            {
                // Collect state files, sort by filename descending (newest first)
                std::vector<fs::path> stateFiles;
                for (const auto& sf : fs::directory_iterator(stateDir, ec))
                {
                    if (sf.is_regular_file(ec) && sf.path().extension() == ".json")
                        stateFiles.push_back(sf.path());
                }

                std::sort(stateFiles.begin(), stateFiles.end(),
                    [](const fs::path& a, const fs::path& b) {
                        return a.filename().string() > b.filename().string();
                    });

                // Read first valid state file
                for (const auto& sf : stateFiles)
                {
                    auto stateData = AtomicFileIO::safeReadJson(sf);
                    if (!stateData.has_value())
                        continue;

                    try
                    {
                        auto stateEntry = stateData.value().get<JobStateEntry>();
                        info.current_state = stateEntry.state;
                        info.current_priority = stateEntry.priority;
                        break;
                    }
                    catch (...) { continue; }
                }
            }

            m_jobs.push_back(std::move(info));
        }
        catch (const std::exception& e)
        {
            MonitorLog::instance().error("job", "Failed to parse manifest: " + entry.path().string() + " - " + std::string(e.what()));
        }
    }

    // Sort: priority desc, then submitted_at asc (oldest first = FIFO within same priority)
    std::sort(m_jobs.begin(), m_jobs.end(),
        [](const JobInfo& a, const JobInfo& b) {
            if (a.current_priority != b.current_priority)
                return a.current_priority > b.current_priority;
            return a.manifest.submitted_at_ms < b.manifest.submitted_at_ms;
        });
}

std::string JobManager::submitJob(const std::filesystem::path& farmPath,
                                  const JobManifest& manifest, int priority)
{
    namespace fs = std::filesystem;
    std::error_code ec;

    auto jobsDir = farmPath / "jobs";
    auto jobDir = jobsDir / manifest.job_id;

    // Create directories
    fs::create_directories(jobDir / "state", ec);
    if (ec)
    {
        MonitorLog::instance().error("job", "Failed to create job dirs: " + ec.message());
        return {};
    }
    fs::create_directories(jobDir / "claims", ec);
    fs::create_directories(jobDir / "events", ec);

    // Check manifest doesn't already exist (race protection)
    auto manifestPath = jobDir / "manifest.json";
    if (fs::exists(manifestPath, ec))
    {
        MonitorLog::instance().error("job", "Manifest already exists: " + manifestPath.string());
        return {};
    }

    // Write manifest
    nlohmann::json manifestJson = manifest;
    if (!AtomicFileIO::writeJson(manifestPath, manifestJson))
    {
        MonitorLog::instance().error("job", "Failed to write manifest");
        return {};
    }

    // Write initial state file
    auto now = std::chrono::system_clock::now();
    auto timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    JobStateEntry state;
    state.state = "active";
    state.priority = priority;
    state.node_id = manifest.submitted_by;
    state.timestamp_ms = timestampMs;

    auto stateFilename = std::to_string(timestampMs) + "_" + manifest.submitted_by + ".json";
    nlohmann::json stateJson = state;
    if (!AtomicFileIO::writeJson(jobDir / "state" / stateFilename, stateJson))
    {
        MonitorLog::instance().error("job", "Failed to write initial state");
        return {};
    }

    // Force rescan on next scan() call
    invalidate();

    MonitorLog::instance().info("job", "Job submitted: " + manifest.job_id);
    return manifest.job_id;
}

bool JobManager::writeStateEntry(const std::filesystem::path& farmPath,
                                  const std::string& jobId,
                                  const std::string& state,
                                  int priority,
                                  const std::string& nodeId)
{
    namespace fs = std::filesystem;

    auto now = std::chrono::system_clock::now();
    auto timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    JobStateEntry entry;
    entry.state = state;
    entry.priority = priority;
    entry.node_id = nodeId;
    entry.timestamp_ms = timestampMs;

    auto stateDir = farmPath / "jobs" / jobId / "state";
    std::error_code ec;
    fs::create_directories(stateDir, ec);

    auto stateFilename = std::to_string(timestampMs) + "_" + nodeId + ".json";
    nlohmann::json j = entry;
    if (!AtomicFileIO::writeJson(stateDir / stateFilename, j))
    {
        MonitorLog::instance().error("job", "Failed to write state entry for " + jobId);
        return false;
    }

    invalidate();

    MonitorLog::instance().info("job", "State entry: job=" + jobId + " state=" + state + " priority=" + std::to_string(priority));
    return true;
}

std::vector<JobInfo> JobManager::getJobSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_jobs;
}

void JobManager::invalidate()
{
    m_invalidated = true;
}

} // namespace SR
