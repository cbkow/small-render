#include "monitor/render_coordinator.h"
#include "monitor/agent_supervisor.h"
#include "core/atomic_file_io.h"
#include "core/platform.h"
#include "core/monitor_log.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace SR {

namespace fs = std::filesystem;

void RenderCoordinator::init(const fs::path& farmPath, const std::string& nodeId,
                              const std::string& nodeOS, CompletionCallback completionFn,
                              AgentSupervisor* supervisor)
{
    m_farmPath = farmPath;
    m_nodeId = nodeId;
    m_nodeOS = nodeOS;
    m_completionFn = std::move(completionFn);
    m_supervisor = supervisor;
    m_eventSeq = 0;
    m_eventSeqLoaded = false;
    m_activeRender.reset();
    m_stopped = false;

    MonitorLog::instance().info("render", "Initialized for node " + nodeId);
}

void RenderCoordinator::queueDispatch(const JobManifest& manifest, const ChunkRange& chunk)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_dispatchQueue.push({manifest, chunk});
    MonitorLog::instance().info("render", "Queued dispatch: job=" + manifest.job_id + " chunk=" + chunk.rangeStr());
}

void RenderCoordinator::update(AgentSupervisor& supervisor)
{
    // If no active render and dispatch queue has items and agent is connected
    if (!m_activeRender.has_value())
    {
        PendingDispatch pending;
        bool hasPending = false;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (!m_dispatchQueue.empty())
            {
                pending = std::move(m_dispatchQueue.front());
                m_dispatchQueue.pop();
                hasPending = true;
            }
        }

        if (hasPending && m_stopped)
        {
            // Stopped: don't start new work, abandon the chunk
            MonitorLog::instance().info("render", "Stopped - skipping dispatch, abandoning chunk");
            if (m_completionFn)
                m_completionFn(pending.manifest.job_id, pending.chunk, "abandoned");
            hasPending = false;
        }

        if (hasPending)
        {
            if (!supervisor.isAgentConnected())
            {
                MonitorLog::instance().warn("render", "Agent not connected, re-queuing dispatch");
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_dispatchQueue.push(std::move(pending));
                return;
            }

            // Set up ActiveRender
            ActiveRender ar;
            ar.manifest = std::move(pending.manifest);
            ar.chunk = pending.chunk;
            ar.ackReceived = false;
            ar.progressPct = 0.0f;
            ar.startTime = std::chrono::steady_clock::now();

            MonitorLog::instance().info("render", "Starting render: job=" + ar.manifest.job_id + " chunk=" + ar.chunk.rangeStr());

            m_activeRender = std::move(ar);

            // Dispatch the chunk as a single task
            dispatchChunk(supervisor);
        }
    }

    // Agent disconnect detection during active render
    if (m_activeRender.has_value() && !supervisor.isAgentConnected())
    {
        MonitorLog::instance().error("render", "Agent disconnected during render!");
        auto& ar = m_activeRender.value();
        flushStdout();
        emitEvent("chunk_failed", ar.chunk, {{"error", "Agent disconnected"}});
        failChunk("Agent disconnected during render");
    }
}

void RenderCoordinator::abortCurrentRender(const std::string& reason)
{
    if (!m_activeRender.has_value())
        return;

    auto& ar = m_activeRender.value();
    std::string jobId = ar.manifest.job_id;

    MonitorLog::instance().warn("render", "Aborting render: job=" + jobId + " chunk=" + ar.chunk.rangeStr() + " reason=" + reason);

    // Tell the agent to abort
    if (m_supervisor)
        m_supervisor->sendAbort(reason);

    // Emit failure event and flush stdout
    flushStdout();
    emitEvent("chunk_failed", ar.chunk, {{"error", reason}});

    // Fail the chunk
    failChunk(reason);
}

void RenderCoordinator::purgeJob(const std::string& jobId)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    std::queue<PendingDispatch> keep;
    while (!m_dispatchQueue.empty())
    {
        auto& item = m_dispatchQueue.front();
        if (item.manifest.job_id != jobId)
            keep.push(std::move(item));
        m_dispatchQueue.pop();
    }
    m_dispatchQueue = std::move(keep);
}

void RenderCoordinator::setStopped(bool stopped)
{
    m_stopped = stopped;
}

void RenderCoordinator::handleAgentMessage(const std::string& type, const nlohmann::json& j)
{
    if (!m_activeRender.has_value())
    {
        MonitorLog::instance().warn("render", "Received " + type + " with no active render, ignoring");
        return;
    }

    auto& ar = m_activeRender.value();

    if (type == "ack")
    {
        ar.ackReceived = true;
        ar.startTime = std::chrono::steady_clock::now();
        emitEvent("chunk_started", ar.chunk);
        MonitorLog::instance().info("render", "Chunk " + ar.chunk.rangeStr() + " acknowledged");
    }
    else if (type == "progress")
    {
        ar.progressPct = j.value("progress_pct", 0.0f);
    }
    else if (type == "stdout")
    {
        if (j.contains("lines") && j["lines"].is_array())
        {
            std::vector<std::string> lines;
            for (const auto& line : j["lines"])
            {
                if (line.is_string())
                    lines.push_back(line.get<std::string>());
            }
            appendStdout(lines);
        }
    }
    else if (type == "frame_completed")
    {
        int frame = j.value("frame", -1);
        if (frame >= 0)
        {
            ar.completedFrames.insert(frame);
            ChunkRange singleFrame{frame, frame};
            emitEvent("frame_finished", singleFrame);
            MonitorLog::instance().info("render",
                "Frame " + std::to_string(frame) + " finished for job " + ar.manifest.job_id);
        }
    }
    else if (type == "completed")
    {
        onChunkCompleted(j);
    }
    else if (type == "failed")
    {
        onChunkFailed(j);
    }
}

std::string RenderCoordinator::currentJobId() const
{
    if (m_activeRender.has_value())
        return m_activeRender->manifest.job_id;
    return {};
}

ChunkRange RenderCoordinator::currentChunk() const
{
    if (m_activeRender.has_value())
        return m_activeRender->chunk;
    return {};
}

std::string RenderCoordinator::currentChunkLabel() const
{
    if (!m_activeRender.has_value())
        return {};

    const auto& chunk = m_activeRender->chunk;
    if (chunk.frame_start == chunk.frame_end)
        return "f" + std::to_string(chunk.frame_start);
    return "f" + std::to_string(chunk.frame_start) + "-" + std::to_string(chunk.frame_end);
}

float RenderCoordinator::currentProgress() const
{
    if (m_activeRender.has_value())
        return m_activeRender->progressPct;
    return 0.0f;
}

// ─── Task JSON building ────────────────────────────────────────────────────

nlohmann::json RenderCoordinator::buildTaskJson(const JobManifest& manifest, const ChunkRange& chunk)
{
    // Get executable for this OS
    auto cmdIt = manifest.cmd.find(m_nodeOS);
    std::string executable;
    if (cmdIt != manifest.cmd.end())
        executable = cmdIt->second;

    // Build args from flags with token substitution
    std::vector<std::string> args;
    for (const auto& f : manifest.flags)
    {
        if (!f.flag.empty())
            args.push_back(substituteTokens(f.flag, chunk));
        if (f.value.has_value())
            args.push_back(substituteTokens(f.value.value(), chunk));
    }

    // Build progress spec
    nlohmann::json progressJson = nullptr;
    if (!manifest.progress.patterns.empty() || manifest.progress.completion_pattern.has_value())
    {
        progressJson = manifest.progress;
    }

    // Build output detection
    nlohmann::json outputJson = nullptr;
    if (manifest.output_detection.stdout_regex.has_value())
    {
        outputJson = {
            {"regex", manifest.output_detection.stdout_regex.value()},
            {"capture_group", manifest.output_detection.path_group},
        };
    }

    // Working dir
    std::string workingDir;
    if (manifest.process.working_dir.has_value())
        workingDir = substituteTokens(manifest.process.working_dir.value(), chunk);

    nlohmann::json task = {
        {"type", "task"},
        {"job_id", manifest.job_id},
        {"frame_start", chunk.frame_start},
        {"frame_end", chunk.frame_end},
        {"command", {
            {"executable", executable},
            {"args", args},
        }},
        {"working_dir", workingDir.empty() ? nlohmann::json(nullptr) : nlohmann::json(workingDir)},
        {"environment", manifest.environment},
        {"progress", progressJson},
        {"output_detection", outputJson},
        {"timeout_seconds", manifest.timeout_seconds.has_value()
            ? nlohmann::json(manifest.timeout_seconds.value())
            : nlohmann::json(nullptr)},
    };

    return task;
}

void RenderCoordinator::dispatchChunk(AgentSupervisor& supervisor)
{
    if (!m_activeRender.has_value())
        return;

    auto& ar = m_activeRender.value();
    ar.ackReceived = false;
    ar.progressPct = 0.0f;
    ar.startTime = std::chrono::steady_clock::now();
    ar.stdoutBuffer.clear();

    // Build stdout log filename: {rangeStr}_{timestamp_ms}.log
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    ar.stdoutLogName = ar.chunk.rangeStr() + "_" + std::to_string(nowMs) + ".log";

    // Ensure output directory exists before dispatching
    if (ar.manifest.output_dir.has_value() && !ar.manifest.output_dir.value().empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(ar.manifest.output_dir.value(), ec);
        if (ec)
            MonitorLog::instance().warn("render", "Failed to create output dir: " + ar.manifest.output_dir.value() + " (" + ec.message() + ")");
    }

    auto taskJson = buildTaskJson(ar.manifest, ar.chunk);
    std::string taskStr = taskJson.dump();

    MonitorLog::instance().info("render", "Dispatching chunk " + ar.chunk.rangeStr() + " for job " + ar.manifest.job_id);

    supervisor.sendTask(taskStr);
}

std::string RenderCoordinator::substituteTokens(const std::string& input, const ChunkRange& chunk) const
{
    std::string result = input;

    // {frame} → alias for {chunk_start} (backward compat)
    {
        std::string token = "{frame}";
        size_t pos = 0;
        while ((pos = result.find(token, pos)) != std::string::npos)
        {
            result.replace(pos, token.length(), std::to_string(chunk.frame_start));
            pos += std::to_string(chunk.frame_start).length();
        }
    }

    // {chunk_start}
    {
        std::string token = "{chunk_start}";
        size_t pos = 0;
        while ((pos = result.find(token, pos)) != std::string::npos)
        {
            result.replace(pos, token.length(), std::to_string(chunk.frame_start));
            pos += std::to_string(chunk.frame_start).length();
        }
    }

    // {chunk_end}
    {
        std::string token = "{chunk_end}";
        size_t pos = 0;
        while ((pos = result.find(token, pos)) != std::string::npos)
        {
            result.replace(pos, token.length(), std::to_string(chunk.frame_end));
            pos += std::to_string(chunk.frame_end).length();
        }
    }

    return result;
}

// ─── Event files ────────────────────────────────────────────────────────────

void RenderCoordinator::emitEvent(const std::string& type, const ChunkRange& chunk, const nlohmann::json& extra)
{
    if (!m_activeRender.has_value())
        return;

    auto& ar = m_activeRender.value();
    auto eventsDir = m_farmPath / "jobs" / ar.manifest.job_id / "events" / m_nodeId;
    ensureDir(eventsDir);

    uint64_t seq = nextEventSeq();
    std::string rangeStr = chunk.rangeStr();

    // Filename: {6-digit seq}_{type}_{rangeStr}.json
    std::ostringstream fname;
    fname << std::setfill('0') << std::setw(6) << seq
          << "_" << type
          << "_" << rangeStr
          << ".json";

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    nlohmann::json event = {
        {"_version", 1},
        {"node_id", m_nodeId},
        {"seq", seq},
        {"frame_start", chunk.frame_start},
        {"frame_end", chunk.frame_end},
        {"type", type},
        {"timestamp_ms", now},
    };

    // Merge extra fields
    for (auto& [key, val] : extra.items())
    {
        event[key] = val;
    }

    AtomicFileIO::writeJson(eventsDir / fname.str(), event);
}

uint64_t RenderCoordinator::nextEventSeq()
{
    if (!m_eventSeqLoaded && m_activeRender.has_value())
    {
        // Scan existing event files to find max seq
        auto& ar = m_activeRender.value();
        auto eventsDir = m_farmPath / "jobs" / ar.manifest.job_id / "events" / m_nodeId;

        std::error_code ec;
        if (fs::is_directory(eventsDir, ec))
        {
            for (const auto& entry : fs::directory_iterator(eventsDir, ec))
            {
                if (!entry.is_regular_file(ec) || entry.path().extension() != ".json")
                    continue;

                std::string name = entry.path().stem().string();
                // Parse first 6 digits as seq number
                if (name.size() >= 6)
                {
                    try
                    {
                        uint64_t s = std::stoull(name.substr(0, 6));
                        if (s > m_eventSeq)
                            m_eventSeq = s;
                    }
                    catch (...) {}
                }
            }
        }
        m_eventSeqLoaded = true;
    }

    return ++m_eventSeq;
}

// ─── Stdout log files ───────────────────────────────────────────────────────

void RenderCoordinator::appendStdout(const std::vector<std::string>& lines)
{
    if (!m_activeRender.has_value())
        return;

    // Append to buffer
    auto& ar = m_activeRender.value();
    for (const auto& line : lines)
        ar.stdoutBuffer.push_back(line);

    // Flush to disk
    flushStdout();
}

void RenderCoordinator::flushStdout()
{
    if (!m_activeRender.has_value())
        return;

    auto& ar = m_activeRender.value();
    if (ar.stdoutBuffer.empty())
        return;

    auto stdoutDir = m_farmPath / "jobs" / ar.manifest.job_id / "stdout" / m_nodeId;
    ensureDir(stdoutDir);

    auto logPath = stdoutDir / ar.stdoutLogName;

    // Append mode
    std::ofstream ofs(logPath, std::ios::app);
    if (ofs.is_open())
    {
        for (const auto& line : ar.stdoutBuffer)
            ofs << line << "\n";
        ofs.flush();
    }
    else
    {
        MonitorLog::instance().error("render", "Failed to open stdout log: " + logPath.string());
    }

    ar.stdoutBuffer.clear();
}

// ─── Completion / failure ───────────────────────────────────────────────────

void RenderCoordinator::onChunkCompleted(const nlohmann::json& j)
{
    if (!m_activeRender.has_value())
        return;

    auto& ar = m_activeRender.value();
    flushStdout();

    int64_t elapsed_ms = j.value("elapsed_ms", int64_t(0));
    int exit_code = j.value("exit_code", 0);
    std::string output_file;
    if (j.contains("output_file") && !j["output_file"].is_null())
        output_file = j["output_file"].get<std::string>();

    emitEvent("chunk_finished", ar.chunk, {
        {"elapsed_ms", elapsed_ms},
        {"exit_code", exit_code},
        {"output_file", output_file.empty() ? nlohmann::json(nullptr) : nlohmann::json(output_file)},
    });

    std::string jobId = ar.manifest.job_id;
    ChunkRange chunk = ar.chunk;

    MonitorLog::instance().info("render", "Chunk " + chunk.rangeStr() + " completed for job " + jobId + " (exit_code=" + std::to_string(exit_code) + ", elapsed=" + std::to_string(elapsed_ms) + "ms)");

    m_activeRender.reset();
    if (m_completionFn)
        m_completionFn(jobId, chunk, "completed");
}

void RenderCoordinator::onChunkFailed(const nlohmann::json& j)
{
    if (!m_activeRender.has_value())
        return;

    auto& ar = m_activeRender.value();
    flushStdout();

    int exit_code = j.value("exit_code", -1);
    std::string error = j.value("error", std::string("Unknown error"));

    emitEvent("chunk_failed", ar.chunk, {
        {"exit_code", exit_code},
        {"error", error},
    });

    MonitorLog::instance().error("render", "Chunk " + ar.chunk.rangeStr() + " failed: " + error);

    failChunk(error);
}

void RenderCoordinator::failChunk(const std::string& error)
{
    if (!m_activeRender.has_value())
        return;

    std::string jobId = m_activeRender->manifest.job_id;
    ChunkRange chunk = m_activeRender->chunk;

    MonitorLog::instance().error("render", "Chunk " + chunk.rangeStr() + " FAILED for job " + jobId + ": " + error);

    m_activeRender.reset();
    if (m_completionFn)
        m_completionFn(jobId, chunk, "failed");
}

} // namespace SR
