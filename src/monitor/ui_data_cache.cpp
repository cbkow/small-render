#include "monitor/ui_data_cache.h"
#include "core/atomic_file_io.h"
#include "core/monitor_log.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <chrono>
#include <ctime>
#include <cstdio>

namespace SR {

namespace fs = std::filesystem;

UIDataCache::~UIDataCache()
{
    stop();
}

void UIDataCache::start(const std::filesystem::path& farmPath)
{
    if (m_running.load()) return;

    m_farmPath = farmPath;
    m_running.store(true);
    m_thread = std::thread(&UIDataCache::threadFunc, this);
}

void UIDataCache::stop()
{
    m_running.store(false);
    if (m_thread.joinable())
        m_thread.join();
}

// ─── Main thread setters ─────────────────────────────────────────────────────

void UIDataCache::setSelectedJobId(const std::string& jobId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_selectedJobId != jobId)
    {
        m_selectedJobId = jobId;
        m_wakeFlag.store(true); // wake bg thread + skip cooldowns
    }
}

void UIDataCache::setJobIds(const std::vector<std::string>& ids)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_jobIds = ids;
}

void UIDataCache::setLogRequest(const std::string& mode,
                                 const std::vector<std::string>& nodeIds)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_logMode = mode;
    m_logNodeIds = nodeIds;
}

void UIDataCache::setDispatchTables(const std::map<std::string, DispatchTable>& tables)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_coordinatorTables = tables;
    m_hasCoordinatorTables = true;

    // Coordinator fast path: merge progress for coordinator-tracked jobs only
    // (non-coordinator jobs like completed ones are handled by bg thread from disk)
    for (const auto& [jobId, dt] : tables)
    {
        JobProgress prog;
        for (const auto& dc : dt.chunks)
        {
            int count = dc.frame_end - dc.frame_start + 1;
            prog.total += count;
            if (dc.state == "completed")
                prog.completed += count;
            else if (dc.state == "assigned")
                prog.rendering += count;
            else if (dc.state == "failed")
                prog.failed += count;
        }
        m_progress[jobId] = prog;  // merge, not replace
    }

    // Frame states for selected job
    if (!m_selectedJobId.empty())
    {
        auto it = tables.find(m_selectedJobId);
        if (it != tables.end())
        {
            FrameStateSnapshot snap;
            snap.jobId = m_selectedJobId;
            snap.chunks = it->second.chunks;
            for (const auto& dc : it->second.chunks)
            {
                std::string state = "unclaimed";
                if (dc.state == "assigned") state = "rendering";
                else if (dc.state == "completed") state = "completed";
                else if (dc.state == "failed") state = "failed";
                for (int f = dc.frame_start; f <= dc.frame_end; ++f)
                    snap.frameStates.push_back({f, state});
            }
            m_frameStates = std::move(snap);
        }
    }
}

// ─── Main thread getters ─────────────────────────────────────────────────────

std::map<std::string, UIDataCache::JobProgress> UIDataCache::getProgressSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_progress;
}

UIDataCache::FrameStateSnapshot UIDataCache::getFrameStateSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_frameStates;
}

UIDataCache::TaskOutputSnapshot UIDataCache::getTaskOutputSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_taskOutput;
}

UIDataCache::RemoteLogSnapshot UIDataCache::getRemoteLogSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_remoteLogs;
}

// ─── Background thread ───────────────────────────────────────────────────────

void UIDataCache::threadFunc()
{
    while (m_running.load())
    {
        for (int i = 0; i < 10 && m_running.load(); ++i)
        {
            if (m_wakeFlag.load()) break; // wake early on job selection change
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (!m_running.load()) break;

        try
        {
            bool wake = m_wakeFlag.exchange(false);
            scanProgress();
            scanFrameStates(wake);
            scanTaskOutput(wake);
            scanRemoteLogs();
        }
        catch (const std::exception& e)
        {
            MonitorLog::instance().error("farm", std::string("UIDataCache exception: ") + e.what());
        }
        catch (...)
        {
            MonitorLog::instance().error("farm", "UIDataCache unknown exception");
        }
    }
}

// ─── Progress scanning ───────────────────────────────────────────────────────

void UIDataCache::scanProgress()
{
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastProgressScan).count();
    if (elapsed < 5000) return;
    m_lastProgressScan = now;

    // Copy inputs under lock
    std::vector<std::string> jobIds;
    bool hasCoordTables = false;
    std::set<std::string> coordJobIds;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        jobIds = m_jobIds;
        hasCoordTables = m_hasCoordinatorTables;
        if (hasCoordTables)
        {
            for (const auto& [k, v] : m_coordinatorTables)
                coordJobIds.insert(k);
        }
    }

    std::map<std::string, JobProgress> diskProgress;

    for (const auto& jobId : jobIds)
    {
        // Skip coordinator-tracked jobs — main thread handles those in setDispatchTables()
        if (hasCoordTables && coordJobIds.count(jobId))
            continue;

        // Read from disk for non-coordinator jobs (completed, pending, etc.)
        auto dispatchPath = m_farmPath / "jobs" / jobId / "dispatch.json";
        auto data = AtomicFileIO::safeReadJson(dispatchPath);
        if (!data.has_value())
            continue;

        try
        {
            DispatchTable dt = data.value().get<DispatchTable>();
            JobProgress prog;
            for (const auto& dc : dt.chunks)
            {
                int count = dc.frame_end - dc.frame_start + 1;
                prog.total += count;
                if (dc.state == "completed")
                    prog.completed += count;
                else if (dc.state == "assigned")
                    prog.rendering += count;
                else if (dc.state == "failed")
                    prog.failed += count;
            }
            diskProgress[jobId] = prog;
        }
        catch (...) {}
    }

    // Merge under lock: only update non-coordinator entries, never touch coordinator entries
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [jobId, prog] : diskProgress)
        m_progress[jobId] = prog;

    // Prune entries for jobs that no longer exist in the job list
    std::set<std::string> jobIdSet(jobIds.begin(), jobIds.end());
    for (auto it = m_progress.begin(); it != m_progress.end(); )
    {
        if (jobIdSet.find(it->first) == jobIdSet.end())
            it = m_progress.erase(it);
        else
            ++it;
    }
}

// ─── Frame state scanning ────────────────────────────────────────────────────

void UIDataCache::scanFrameStates(bool force)
{
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastFrameScan).count();
    if (!force && elapsed < 3000) return;
    m_lastFrameScan = now;

    // Copy inputs
    std::string jobId;
    bool hasCoordTables = false;
    bool jobIsCoordTracked = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        jobId = m_selectedJobId;
        hasCoordTables = m_hasCoordinatorTables;
        if (hasCoordTables && !jobId.empty())
            jobIsCoordTracked = (m_coordinatorTables.find(jobId) != m_coordinatorTables.end());
    }

    // Coordinator tracks this job → main thread handles it in setDispatchTables()
    if (jobIsCoordTracked) return;

    if (jobId.empty())
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_frameStates = {};
        return;
    }

    FrameStateSnapshot snap;
    snap.jobId = jobId;

    // Read dispatch table from disk (non-coordinator job: completed, pending, etc.)
    DispatchTable dt;
    bool gotTable = false;

    auto dispatchPath = m_farmPath / "jobs" / jobId / "dispatch.json";
    auto data = AtomicFileIO::safeReadJson(dispatchPath);
    if (data.has_value())
    {
        try { dt = data.value().get<DispatchTable>(); gotTable = true; }
        catch (...) {}
    }

    if (!gotTable)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_frameStates = snap;
        return;
    }

    snap.chunks = dt.chunks;

    // Build frame state map
    for (const auto& dc : dt.chunks)
    {
        std::string state = "unclaimed";
        if (dc.state == "assigned") state = "rendering";
        else if (dc.state == "completed") state = "completed";
        else if (dc.state == "failed") state = "failed";

        for (int f = dc.frame_start; f <= dc.frame_end; ++f)
            snap.frameStates.push_back({f, state});
    }

    // Scan event files for per-frame completions within "assigned" chunks
    std::error_code ec;
    auto eventsBaseDir = m_farmPath / "jobs" / jobId / "events";
    if (fs::is_directory(eventsBaseDir, ec))
    {
        // Build set of frames currently assigned
        std::set<int> assignedFrames;
        for (const auto& dc : dt.chunks)
        {
            if (dc.state == "assigned")
            {
                for (int f = dc.frame_start; f <= dc.frame_end; ++f)
                    assignedFrames.insert(f);
            }
        }

        if (!assignedFrames.empty())
        {
            std::set<int> completedFrames;
            for (const auto& nodeDir : fs::directory_iterator(eventsBaseDir, ec))
            {
                if (!nodeDir.is_directory(ec)) continue;
                for (const auto& entry : fs::directory_iterator(nodeDir.path(), ec))
                {
                    if (entry.path().extension() != ".json") continue;
                    std::string stem = entry.path().stem().string();
                    if (stem.find("_frame_finished_") == std::string::npos) continue;

                    auto pos = stem.find("_frame_finished_") + 16;
                    auto dash = stem.find('-', pos);
                    if (dash == std::string::npos) continue;
                    try
                    {
                        int frameNum = std::stoi(stem.substr(pos, dash - pos));
                        if (assignedFrames.count(frameNum))
                            completedFrames.insert(frameNum);
                    }
                    catch (...) {}
                }
            }

            // Upgrade assigned→completed for finished frames
            for (auto& [frame, state] : snap.frameStates)
            {
                if (state == "rendering" && completedFrames.count(frame))
                    state = "completed";
            }
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_frameStates = std::move(snap);
}

// ─── Task output scanning ────────────────────────────────────────────────────

void UIDataCache::scanTaskOutput(bool force)
{
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastTaskOutputScan).count();
    if (!force && elapsed < 3000) return;
    m_lastTaskOutputScan = now;

    std::string jobId;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        jobId = m_selectedJobId;
    }

    if (jobId.empty())
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_taskOutput = {};
        return;
    }

    TaskOutputSnapshot snap;
    snap.jobId = jobId;

    std::error_code ec;
    auto stdoutDir = m_farmPath / "jobs" / jobId / "stdout";

    struct LogFile
    {
        std::string nodeId;
        std::string rangeStr;
        uint64_t timestamp_ms = 0;
        fs::path path;
    };
    std::vector<LogFile> logFiles;

    if (fs::is_directory(stdoutDir, ec))
    {
        for (auto& nodeEntry : fs::directory_iterator(stdoutDir, ec))
        {
            if (!nodeEntry.is_directory(ec)) continue;
            std::string nodeId = nodeEntry.path().filename().string();

            std::error_code ec2;
            for (auto& fileEntry : fs::directory_iterator(nodeEntry.path(), ec2))
            {
                if (!fileEntry.is_regular_file(ec2)) continue;
                if (fileEntry.path().extension() != ".log") continue;

                std::string fname = fileEntry.path().filename().string();
                auto lastUnderscore = fname.rfind('_');
                auto dotPos = fname.rfind('.');
                if (lastUnderscore == std::string::npos || dotPos == std::string::npos)
                    continue;

                std::string rangeStr = fname.substr(0, lastUnderscore);
                std::string tsStr = fname.substr(lastUnderscore + 1, dotPos - lastUnderscore - 1);

                uint64_t ts = 0;
                try { ts = std::stoull(tsStr); }
                catch (...) { continue; }

                logFiles.push_back({nodeId, rangeStr, ts, fileEntry.path()});
            }
        }
    }

    std::sort(logFiles.begin(), logFiles.end(),
              [](const LogFile& a, const LogFile& b) {
                  if (a.rangeStr != b.rangeStr) return a.rangeStr < b.rangeStr;
                  return a.timestamp_ms < b.timestamp_ms;
              });

    for (const auto& lf : logFiles)
    {
        time_t secs = static_cast<time_t>(lf.timestamp_ms / 1000);
        struct tm tmBuf;
#ifdef _WIN32
        localtime_s(&tmBuf, &secs);
#else
        localtime_r(&secs, &tmBuf);
#endif
        char timeBuf[16];
        std::snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d",
                      tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);

        std::string header = lf.nodeId + "  |  f" + lf.rangeStr + "  |  " + timeBuf;
        snap.lines.push_back({header, true});

        std::ifstream ifs(lf.path);
        std::string line;
        while (std::getline(ifs, line))
            snap.lines.push_back({std::move(line), false});

        snap.lines.push_back({"", false});
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_taskOutput = std::move(snap);
}

// ─── Remote log scanning ─────────────────────────────────────────────────────

void UIDataCache::scanRemoteLogs()
{
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastRemoteLogScan).count();
    if (elapsed < 5000) return;
    m_lastRemoteLogScan = now;

    std::string logMode;
    std::vector<std::string> nodeIds;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        logMode = m_logMode;
        nodeIds = m_logNodeIds;
    }

    if (logMode.empty() || nodeIds.empty())
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_remoteLogs = {};
        return;
    }

    RemoteLogSnapshot snap;
    snap.cacheKey = logMode;

    for (const auto& nodeId : nodeIds)
    {
        int maxLines = (logMode == "all") ? 200 : 500;
        auto lines = MonitorLog::readNodeLog(m_farmPath, nodeId, maxLines);
        for (auto& l : lines)
        {
            if (logMode == "all")
                snap.lines.push_back("[" + nodeId + "] " + l);
            else
                snap.lines.push_back(std::move(l));
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_remoteLogs = std::move(snap);
}

} // namespace SR
