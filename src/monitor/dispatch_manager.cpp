#include "monitor/dispatch_manager.h"
#include "core/atomic_file_io.h"
#include "core/platform.h"
#include "core/monitor_log.h"

#include <algorithm>
#include <chrono>

namespace SR {

namespace fs = std::filesystem;

void DispatchManager::start(const fs::path& farmPath,
                             const std::string& nodeId,
                             const std::string& nodeOS,
                             const TimingConfig& timing,
                             const std::vector<std::string>& tags,
                             std::function<std::vector<NodeInfo>()> nodeSnapshotFn,
                             std::function<std::vector<JobInfo>()> jobSnapshotFn)
{
    if (m_running)
        return;

    m_farmPath = farmPath;
    m_nodeId = nodeId;
    m_nodeOS = nodeOS;
    m_timing = timing;
    m_tags = tags;
    m_nodeSnapshotFn = std::move(nodeSnapshotFn);
    m_jobSnapshotFn = std::move(jobSnapshotFn);
    m_assignments.clear();
    m_dispatchTables.clear();
    m_dirtyTables.clear();
    m_completionWritten.clear();
    m_recovered = false;
    m_running = true;

    MonitorLog::instance().info("dispatch", "Started as coordinator");
}

void DispatchManager::stop()
{
    if (!m_running)
        return;

    // Write any dirty tables before stopping
    for (const auto& jobId : m_dirtyTables)
    {
        auto it = m_dispatchTables.find(jobId);
        if (it == m_dispatchTables.end()) continue;

        auto& dt = it->second;
        dt.updated_at_ms = nowMs();
        nlohmann::json j = dt;
        auto path = m_farmPath / "jobs" / jobId / "dispatch.json";
        AtomicFileIO::writeJson(path, j);
    }
    m_dirtyTables.clear();

    m_running = false;
    MonitorLog::instance().info("dispatch", "Stopped");
}

void DispatchManager::update()
{
    if (!m_running)
        return;

    auto jobs = m_jobSnapshotFn();

    // One-time recovery on first update
    if (!m_recovered)
    {
        recoverFromDisk(jobs);
        m_recovered = true;
    }

    // Ensure dispatch tables exist for all active jobs
    for (const auto& job : jobs)
    {
        if (job.current_state == "active" &&
            m_dispatchTables.find(job.manifest.job_id) == m_dispatchTables.end())
        {
            initDispatchTable(job.manifest.job_id, job.manifest);
        }
    }

    processLocalCompletions();
    processWorkerReports();
    detectDeadWorkers();
    checkJobCompletions();

    if (m_nodeActive)
        assignWork();

    writeDispatchTables();
}

void DispatchManager::processAction(const CommandManager::Action& action)
{
    if (action.type == "chunk_completed" || action.type == "chunk_failed")
    {
        m_workerReports.push(action);
    }
}

void DispatchManager::queueLocalCompletion(const std::string& jobId,
                                            const ChunkRange& chunk,
                                            const std::string& state)
{
    m_localCompletionQueue.push({jobId, chunk, state});
}

void DispatchManager::handleJobStateChange(const std::string& jobId,
                                            const std::string& newState)
{
    if (newState == "paused" || newState == "cancelled")
    {
        // Find any assigned chunks for this job and send abort to workers
        std::vector<std::string> nodesToAbort;
        for (auto& [nodeId, assignment] : m_assignments)
        {
            if (assignment.jobId == jobId)
                nodesToAbort.push_back(nodeId);
        }

        for (const auto& nodeId : nodesToAbort)
        {
            if (nodeId != m_nodeId && m_commandSenderFn)
            {
                auto& a = m_assignments[nodeId];
                m_commandSenderFn(nodeId, "abort_chunk", jobId, "job_" + newState,
                                  a.chunk.frame_start, a.chunk.frame_end);
            }
            m_assignments.erase(nodeId);
        }

        // Mark all assigned chunks in dispatch table back to pending
        auto it = m_dispatchTables.find(jobId);
        if (it != m_dispatchTables.end())
        {
            for (auto& chunk : it->second.chunks)
            {
                if (chunk.state == "assigned")
                {
                    chunk.state = "pending";
                    chunk.assigned_to.clear();
                    chunk.assigned_at_ms = 0;
                }
            }
            markDirty(jobId);
        }
    }
    else if (newState == "active")
    {
        // Resume: dispatch table already has pending chunks, assignWork() will pick them up
        auto it = m_dispatchTables.find(jobId);
        if (it == m_dispatchTables.end())
        {
            // Table may have been cleaned up — rebuild from manifest
            auto jobs = m_jobSnapshotFn();
            for (const auto& job : jobs)
            {
                if (job.manifest.job_id == jobId)
                {
                    initDispatchTable(jobId, job.manifest);
                    break;
                }
            }
        }
    }
}

void DispatchManager::setLocalDispatchCallback(DispatchCallback fn)
{
    m_localDispatchFn = std::move(fn);
}

void DispatchManager::setCommandSender(CommandSenderFn fn)
{
    m_commandSenderFn = std::move(fn);
}

void DispatchManager::updateTiming(const TimingConfig& timing)
{
    m_timing = timing;
}

void DispatchManager::updateTags(const std::vector<std::string>& tags)
{
    m_tags = tags;
}

void DispatchManager::setNodeActive(bool active)
{
    m_nodeActive = active;
}

// ─── Dispatch cycle steps ───────────────────────────────────────────────────

void DispatchManager::processLocalCompletions()
{
    while (!m_localCompletionQueue.empty())
    {
        auto entry = std::move(m_localCompletionQueue.front());
        m_localCompletionQueue.pop();

        auto it = m_dispatchTables.find(entry.jobId);
        if (it == m_dispatchTables.end()) continue;

        for (auto& chunk : it->second.chunks)
        {
            if (chunk.frame_start == entry.chunk.frame_start &&
                chunk.frame_end == entry.chunk.frame_end)
            {
                if (entry.state == "completed")
                {
                    chunk.state = "completed";
                    chunk.completed_at_ms = nowMs();
                }
                else if (entry.state == "failed")
                {
                    chunk.retry_count++;
                    auto jobs = m_jobSnapshotFn();
                    int maxRetries = 3;
                    for (const auto& j : jobs)
                    {
                        if (j.manifest.job_id == entry.jobId)
                        {
                            maxRetries = j.manifest.max_retries;
                            break;
                        }
                    }
                    chunk.state = (chunk.retry_count >= maxRetries) ? "failed" : "pending";
                    chunk.assigned_to.clear();
                    chunk.assigned_at_ms = 0;
                }
                else // abandoned
                {
                    chunk.state = "pending";
                    chunk.assigned_to.clear();
                    chunk.assigned_at_ms = 0;
                }
                markDirty(entry.jobId);
                break;
            }
        }

        // Remove from assignments
        auto ait = m_assignments.find(m_nodeId);
        if (ait != m_assignments.end() && ait->second.jobId == entry.jobId)
            m_assignments.erase(ait);

        MonitorLog::instance().info("dispatch", "Local " + entry.state + ": job=" + entry.jobId +
            " chunk=" + entry.chunk.rangeStr());
    }
}

void DispatchManager::processWorkerReports()
{
    while (!m_workerReports.empty())
    {
        auto action = std::move(m_workerReports.front());
        m_workerReports.pop();

        auto it = m_dispatchTables.find(action.jobId);
        if (it == m_dispatchTables.end()) continue;

        for (auto& chunk : it->second.chunks)
        {
            if (chunk.frame_start == action.frameStart &&
                chunk.frame_end == action.frameEnd)
            {
                if (action.type == "chunk_completed")
                {
                    chunk.state = "completed";
                    chunk.completed_at_ms = nowMs();
                }
                else // chunk_failed
                {
                    chunk.retry_count++;
                    auto jobs = m_jobSnapshotFn();
                    int maxRetries = 3;
                    for (const auto& j : jobs)
                    {
                        if (j.manifest.job_id == action.jobId)
                        {
                            maxRetries = j.manifest.max_retries;
                            break;
                        }
                    }
                    chunk.state = (chunk.retry_count >= maxRetries) ? "failed" : "pending";
                    chunk.assigned_to.clear();
                    chunk.assigned_at_ms = 0;
                }
                markDirty(action.jobId);
                break;
            }
        }

        // Remove from assignments
        auto ait = m_assignments.find(action.fromNodeId);
        if (ait != m_assignments.end() && ait->second.jobId == action.jobId)
            m_assignments.erase(ait);

        MonitorLog::instance().info("dispatch", "Worker " + action.type + " from " +
            action.fromNodeId + ": job=" + action.jobId);
    }
}

void DispatchManager::detectDeadWorkers()
{
    auto nodes = m_nodeSnapshotFn();
    auto now = nowMs();

    // Stale assignment timeout: generous enough for command propagation + inbox poll + render start
    // Use dead detection time * 2 as baseline, minimum 60 seconds
    int64_t staleMs = (std::max)(
        int64_t(60000),
        int64_t(m_timing.dead_threshold_scans) * int64_t(m_timing.heartbeat_interval_ms) * 2
    );

    std::vector<std::string> staleNodes;
    for (auto& [nodeId, assignment] : m_assignments)
    {
        if (nodeId == m_nodeId) continue; // self is never stale

        // Case 1: worker is dead — reassign immediately
        if (isNodeDead(nodeId, nodes))
        {
            staleNodes.push_back(nodeId);
            continue;
        }

        // Case 2: assignment has been pending too long and worker isn't rendering it
        int64_t age = now - assignment.assignedAtMs;
        if (age > staleMs)
        {
            bool workerRenderingThisJob = false;
            for (const auto& n : nodes)
            {
                if (n.heartbeat.node_id == nodeId)
                {
                    workerRenderingThisJob =
                        (n.heartbeat.render_state == "rendering" &&
                         n.heartbeat.active_job == assignment.jobId);
                    break;
                }
            }

            if (!workerRenderingThisJob)
            {
                staleNodes.push_back(nodeId);
                MonitorLog::instance().warn("dispatch", "Stale assignment to " + nodeId +
                    " chunk=" + assignment.chunk.rangeStr() +
                    " job=" + assignment.jobId +
                    " (age=" + std::to_string(age / 1000) + "s, worker not rendering)");
            }
        }
    }

    for (const auto& nodeId : staleNodes)
    {
        auto& assignment = m_assignments[nodeId];
        auto it = m_dispatchTables.find(assignment.jobId);
        if (it != m_dispatchTables.end())
        {
            for (auto& chunk : it->second.chunks)
            {
                if (chunk.frame_start == assignment.chunk.frame_start &&
                    chunk.frame_end == assignment.chunk.frame_end &&
                    chunk.state == "assigned")
                {
                    chunk.retry_count++;
                    auto jobs = m_jobSnapshotFn();
                    int maxRetries = 3;
                    for (const auto& j : jobs)
                    {
                        if (j.manifest.job_id == assignment.jobId)
                        {
                            maxRetries = j.manifest.max_retries;
                            break;
                        }
                    }
                    chunk.state = (chunk.retry_count >= maxRetries) ? "failed" : "pending";
                    chunk.assigned_to.clear();
                    chunk.assigned_at_ms = 0;
                    markDirty(assignment.jobId);

                    MonitorLog::instance().warn("dispatch", "Reassigning chunk " +
                        assignment.chunk.rangeStr() + " from " + nodeId +
                        " for job " + assignment.jobId);
                    break;
                }
            }
        }
        m_assignments.erase(nodeId);
    }
}

void DispatchManager::checkJobCompletions()
{
    auto jobs = m_jobSnapshotFn();

    for (const auto& job : jobs)
    {
        if (job.current_state != "active")
            continue;

        const auto& jobId = job.manifest.job_id;
        if (m_completionWritten.count(jobId))
            continue;

        auto it = m_dispatchTables.find(jobId);
        if (it == m_dispatchTables.end())
            continue;

        bool allDone = true;
        for (const auto& chunk : it->second.chunks)
        {
            if (chunk.state != "completed")
            {
                allDone = false;
                break;
            }
        }

        if (!allDone)
            continue;

        // All chunks completed — write job state entry
        auto now = nowMs();
        JobStateEntry stateEntry;
        stateEntry.state = "completed";
        stateEntry.priority = 0;
        stateEntry.node_id = m_nodeId;
        stateEntry.timestamp_ms = now;

        nlohmann::json j = stateEntry;
        auto stateDir = m_farmPath / "jobs" / jobId / "state";
        std::error_code ec;
        fs::create_directories(stateDir, ec);
        std::string filename = std::to_string(now) + ".json";
        AtomicFileIO::writeJson(stateDir / filename, j);

        m_completionWritten.insert(jobId);
        MonitorLog::instance().info("dispatch", "JOB COMPLETED: " + jobId);
    }
}

void DispatchManager::assignWork()
{
    auto nodes = m_nodeSnapshotFn();
    auto jobs = m_jobSnapshotFn();

    // Build list of idle workers (nodes not already assigned)
    std::vector<const NodeInfo*> idleWorkers;
    for (const auto& node : nodes)
    {
        if (node.isDead) continue;
        if (node.heartbeat.node_state != "active") continue;
        if (node.heartbeat.render_state != "idle") continue;  // must show idle in heartbeat
        if (m_assignments.count(node.heartbeat.node_id)) continue;
        idleWorkers.push_back(&node);
    }

    if (idleWorkers.empty())
        return;

    // Sort jobs by priority (highest first)
    std::vector<const JobInfo*> activeJobs;
    for (const auto& job : jobs)
    {
        if (job.current_state == "active")
            activeJobs.push_back(&job);
    }
    std::sort(activeJobs.begin(), activeJobs.end(),
        [](const JobInfo* a, const JobInfo* b) {
            return a->current_priority > b->current_priority;
        });

    // For each idle worker, find a pending chunk
    for (const auto* worker : idleWorkers)
    {
        const auto& workerNodeId = worker->heartbeat.node_id;
        const auto& workerOS = worker->heartbeat.os;
        const auto& workerTags = worker->heartbeat.tags;

        for (const auto* job : activeJobs)
        {
            const auto& jobId = job->manifest.job_id;

            // Check OS compatibility
            if (!hasOSCmd(job->manifest, workerOS))
                continue;

            // Check tag requirements
            if (!hasRequiredTags(job->manifest.tags_required, workerTags))
            {
                std::string reqStr, hasStr;
                for (const auto& t : job->manifest.tags_required) { if (!reqStr.empty()) reqStr += ","; reqStr += t; }
                for (const auto& t : workerTags) { if (!hasStr.empty()) hasStr += ","; hasStr += t; }
                MonitorLog::instance().warn("dispatch", "Tag mismatch: job '" + jobId +
                    "' requires [" + reqStr + "], worker " + workerNodeId + " has [" + hasStr + "]");
                continue;
            }

            auto it = m_dispatchTables.find(jobId);
            if (it == m_dispatchTables.end())
                continue;

            // Find first pending chunk
            DispatchChunk* pendingChunk = nullptr;
            for (auto& chunk : it->second.chunks)
            {
                if (chunk.state == "pending")
                {
                    pendingChunk = &chunk;
                    break;
                }
            }

            if (!pendingChunk)
                continue;

            // Assign!
            pendingChunk->state = "assigned";
            pendingChunk->assigned_to = workerNodeId;
            pendingChunk->assigned_at_ms = nowMs();
            markDirty(jobId);

            ChunkRange cr;
            cr.frame_start = pendingChunk->frame_start;
            cr.frame_end = pendingChunk->frame_end;

            m_assignments[workerNodeId] = {jobId, cr, pendingChunk->assigned_at_ms};

            if (workerNodeId == m_nodeId)
            {
                // Self-dispatch: call local callback directly
                if (m_localDispatchFn)
                    m_localDispatchFn(job->manifest, cr);
                MonitorLog::instance().info("dispatch", "Self-assigned: job=" + jobId +
                    " chunk=" + cr.rangeStr());
            }
            else
            {
                // Send assign_chunk command to worker
                if (m_commandSenderFn)
                    m_commandSenderFn(workerNodeId, "assign_chunk", jobId,
                                      "coordinator_dispatch",
                                      cr.frame_start, cr.frame_end);
                MonitorLog::instance().info("dispatch", "Assigned to " + workerNodeId +
                    ": job=" + jobId + " chunk=" + cr.rangeStr());
            }

            break; // one assignment per idle worker per cycle
        }
    }
}

void DispatchManager::writeDispatchTables()
{
    if (m_dirtyTables.empty())
        return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastWrite).count();
    if (elapsed < WRITE_THROTTLE_MS)
        return;

    m_lastWrite = now;

    for (const auto& jobId : m_dirtyTables)
    {
        auto it = m_dispatchTables.find(jobId);
        if (it == m_dispatchTables.end()) continue;

        auto& dt = it->second;
        dt.updated_at_ms = nowMs();
        dt.coordinator_id = m_nodeId;

        nlohmann::json j = dt;
        auto path = m_farmPath / "jobs" / jobId / "dispatch.json";
        AtomicFileIO::writeJson(path, j);
    }

    m_dirtyTables.clear();
}

// ─── Manual chunk controls ──────────────────────────────────────────────────

void DispatchManager::reassignChunk(const std::string& jobId, int frameStart, int frameEnd)
{
    auto it = m_dispatchTables.find(jobId);
    if (it == m_dispatchTables.end()) return;

    for (auto& chunk : it->second.chunks)
    {
        if (chunk.frame_start == frameStart && chunk.frame_end == frameEnd &&
            chunk.state == "assigned")
        {
            // Send abort to the worker currently rendering this chunk
            if (!chunk.assigned_to.empty() && m_commandSenderFn)
            {
                if (chunk.assigned_to == m_nodeId)
                {
                    // Self-assigned: abort via local dispatch callback won't work,
                    // but the completion callback handles reassignment.
                    // We need to tell MonitorApp to abort the local render.
                    // For now, send abort_chunk to self inbox (processed next cycle).
                    m_commandSenderFn(m_nodeId, "abort_chunk", jobId,
                                      "coordinator_reassign", frameStart, frameEnd);
                }
                else
                {
                    m_commandSenderFn(chunk.assigned_to, "abort_chunk", jobId,
                                      "coordinator_reassign", frameStart, frameEnd);
                }

                // Remove from assignments
                m_assignments.erase(chunk.assigned_to);
            }

            chunk.state = "pending";
            chunk.assigned_to.clear();
            chunk.assigned_at_ms = 0;
            markDirty(jobId);

            MonitorLog::instance().info("dispatch", "Manual reassign: job=" + jobId +
                " chunk=" + std::to_string(frameStart) + "-" + std::to_string(frameEnd));
            return;
        }
    }
}

void DispatchManager::retryFailedChunk(const std::string& jobId, int frameStart, int frameEnd)
{
    auto it = m_dispatchTables.find(jobId);
    if (it == m_dispatchTables.end()) return;

    for (auto& chunk : it->second.chunks)
    {
        if (chunk.frame_start == frameStart && chunk.frame_end == frameEnd &&
            chunk.state == "failed")
        {
            chunk.state = "pending";
            chunk.assigned_to.clear();
            chunk.assigned_at_ms = 0;
            // Keep retry_count — don't reset so max_retries can still be enforced
            markDirty(jobId);

            MonitorLog::instance().info("dispatch", "Manual retry: job=" + jobId +
                " chunk=" + std::to_string(frameStart) + "-" + std::to_string(frameEnd));
            return;
        }
    }
}

// ─── Helpers ────────────────────────────────────────────────────────────────

bool DispatchManager::isNodeIdle(const std::string& nodeId,
                                  const std::vector<NodeInfo>& nodes) const
{
    for (const auto& n : nodes)
    {
        if (n.heartbeat.node_id == nodeId)
            return n.heartbeat.render_state == "idle" &&
                   n.heartbeat.node_state == "active" &&
                   !n.isDead;
    }
    return false;
}

bool DispatchManager::isNodeDead(const std::string& nodeId,
                                  const std::vector<NodeInfo>& nodes) const
{
    for (const auto& n : nodes)
    {
        if (n.heartbeat.node_id == nodeId)
            return n.isDead && n.reclaimEligible;
    }
    return true; // not found = dead
}

bool DispatchManager::hasOSCmd(const JobManifest& manifest, const std::string& nodeOS) const
{
    auto it = manifest.cmd.find(nodeOS);
    return it != manifest.cmd.end() && !it->second.empty();
}

bool DispatchManager::hasRequiredTags(const std::vector<std::string>& required,
                                      const std::vector<std::string>& nodeTags) const
{
    for (const auto& req : required)
    {
        bool found = false;
        for (const auto& tag : nodeTags)
        {
            if (tag == req) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

void DispatchManager::initDispatchTable(const std::string& jobId, const JobManifest& manifest)
{
    auto chunks = computeChunks(manifest.frame_start, manifest.frame_end, manifest.chunk_size);

    DispatchTable dt;
    dt.coordinator_id = m_nodeId;
    dt.updated_at_ms = nowMs();
    dt.chunks.reserve(chunks.size());

    for (const auto& cr : chunks)
    {
        DispatchChunk dc;
        dc.frame_start = cr.frame_start;
        dc.frame_end = cr.frame_end;
        dt.chunks.push_back(dc);
    }

    m_dispatchTables[jobId] = std::move(dt);
    markDirty(jobId);

    MonitorLog::instance().info("dispatch", "Init dispatch table: job=" + jobId +
        " chunks=" + std::to_string(chunks.size()));
}

void DispatchManager::markDirty(const std::string& jobId)
{
    m_dirtyTables.insert(jobId);
}

int64_t DispatchManager::nowMs() const
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ─── Recovery ───────────────────────────────────────────────────────────────

void DispatchManager::recoverFromDisk(const std::vector<JobInfo>& jobs)
{
    std::error_code ec;

    for (const auto& job : jobs)
    {
        if (job.current_state != "active")
            continue;

        const auto& jobId = job.manifest.job_id;
        auto dispatchPath = m_farmPath / "jobs" / jobId / "dispatch.json";

        if (!fs::is_regular_file(dispatchPath, ec))
            continue;

        auto data = AtomicFileIO::safeReadJson(dispatchPath);
        if (!data.has_value())
            continue;

        try
        {
            DispatchTable dt = data.value().get<DispatchTable>();

            // Mark "assigned" chunks to dead nodes as "pending",
            // and rebuild m_assignments for chunks that remain assigned
            auto nodes = m_nodeSnapshotFn();
            for (auto& chunk : dt.chunks)
            {
                if (chunk.state == "assigned")
                {
                    if (chunk.assigned_to.empty() || isNodeDead(chunk.assigned_to, nodes))
                    {
                        chunk.state = "pending";
                        chunk.assigned_to.clear();
                        chunk.assigned_at_ms = 0;
                    }
                    else
                    {
                        // Rebuild in-memory assignment tracking so detectDeadWorkers
                        // and stale-assignment timeout can monitor this assignment
                        ChunkRange cr;
                        cr.frame_start = chunk.frame_start;
                        cr.frame_end = chunk.frame_end;
                        m_assignments[chunk.assigned_to] = {jobId, cr, chunk.assigned_at_ms};
                    }
                }
            }

            m_dispatchTables[jobId] = std::move(dt);
            markDirty(jobId);

            MonitorLog::instance().info("dispatch", "Recovered dispatch table: " + jobId);
        }
        catch (const std::exception& e)
        {
            MonitorLog::instance().error("dispatch", "Failed to recover dispatch table for " +
                jobId + ": " + std::string(e.what()));
        }
    }
}

} // namespace SR
