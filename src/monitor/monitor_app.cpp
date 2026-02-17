#include "monitor/monitor_app.h"
#include "monitor/farm_init.h"
#include "core/platform.h"
#include "core/atomic_file_io.h"
#include "core/monitor_log.h"

#include <imgui.h>
#include <filesystem>

namespace SR {

bool MonitorApp::init()
{
    // Resolve app data directory
    m_appDataDir = getAppDataDir();
    m_configPath = m_appDataDir / "config.json";

    MonitorLog::instance().info("farm", "App data dir: " + m_appDataDir.string());

    // Load or generate node identity
    m_identity.loadOrGenerate(m_appDataDir);
    m_identity.querySystemInfo();

    // Load config
    loadConfig();

    // Apply saved font scale
    ImGui::GetIO().FontGlobalScale = m_config.font_scale;

    // Initialize agent supervisor (IPC server + background thread)
    m_agentSupervisor.start(m_identity.nodeId());
    if (m_config.auto_start_agent)
    {
        m_agentSupervisor.spawnAgent();
    }

    // Initialize dashboard
    m_dashboard.init(this);

    // Phase 2: if sync root is configured and valid, start farm
    if (!m_config.sync_root.empty() && std::filesystem::is_directory(m_config.sync_root))
    {
        startFarm();
    }

    MonitorLog::instance().info("farm", "Init complete");
    return true;
}

void MonitorApp::update()
{
    try
    {
        m_agentSupervisor.processMessages();

        // Check for CLI submit requests from another process
        checkSubmitRequest();

        if (m_farmRunning)
        {
            // Process remote commands
            auto actions = m_commandManager.popActions();
            for (const auto& action : actions)
            {
                if (action.type == "assign_chunk")
                {
                    // Worker receives assignment from coordinator
                    handleAssignChunk(action);
                }
                else if (action.type == "abort_chunk")
                {
                    // Worker receives abort from coordinator
                    if (m_renderCoordinator.currentJobId() == action.jobId)
                        m_renderCoordinator.abortCurrentRender("Coordinator abort: " + action.reason);
                }
                else if (action.type == "chunk_completed" || action.type == "chunk_failed")
                {
                    // Coordinator receives worker reports
                    if (m_config.is_coordinator)
                        m_dispatchManager.processAction(action);
                }
                else if (action.type == "stop_job")
                {
                    if (m_renderCoordinator.currentJobId() == action.jobId)
                        m_renderCoordinator.abortCurrentRender("Remote stop: " + action.reason);
                }
                else if (action.type == "stop_all")
                {
                    setNodeState(NodeState::Stopped);
                }
                else if (action.type == "resume_all")
                {
                    setNodeState(NodeState::Active);
                }
            }

            m_templateManager.scan(m_farmPath);
            m_jobManager.scan(m_farmPath);

            if (m_config.is_coordinator)
            {
                m_dispatchManager.update();
                m_submissionManager.update();
            }

            m_renderCoordinator.update(m_agentSupervisor);

            // Worker: retry sending buffered completions when coordinator is back
            if (!m_config.is_coordinator && !m_pendingCompletions.empty())
                flushPendingCompletions();

            // Sync render state to heartbeat so peers can see it
            if (m_renderCoordinator.isRendering())
            {
                m_heartbeatManager.setRenderState("rendering",
                    m_renderCoordinator.currentJobId(),
                    m_renderCoordinator.currentChunkLabel());
            }
            else
            {
                m_heartbeatManager.setRenderState("idle", "", "");
            }
        }

    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("farm", std::string("Update exception: ") + e.what());
    }
    catch (...)
    {
        MonitorLog::instance().error("farm", "Update unknown exception");
    }
}

void MonitorApp::renderUI()
{
    try
    {
        m_dashboard.render();
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("farm", std::string("RenderUI exception: ") + e.what());
    }
    catch (...)
    {
        MonitorLog::instance().error("farm", "RenderUI unknown exception");
    }
}

void MonitorApp::selectJob(const std::string& id)
{
    m_selectedJobId = id;
    m_requestSubmission = false;
}

void MonitorApp::requestSubmissionMode()
{
    m_requestSubmission = true;
    m_selectedJobId.clear();
}

bool MonitorApp::shouldEnterSubmission()
{
    bool val = m_requestSubmission;
    m_requestSubmission = false;
    return val;
}

void MonitorApp::setPendingSubmitRequest(const std::string& file, const std::string& templateId)
{
    m_pendingSubmitRequest.file = file;
    m_pendingSubmitRequest.templateId = templateId;
    requestSubmissionMode();
}

MonitorApp::SubmitRequest MonitorApp::consumeSubmitRequest()
{
    SubmitRequest req = std::move(m_pendingSubmitRequest);
    m_pendingSubmitRequest = {};
    return req;
}

void MonitorApp::checkSubmitRequest()
{
    auto requestPath = m_appDataDir / "submit_request.json";
    std::error_code ec;
    if (!std::filesystem::exists(requestPath, ec))
        return;

    auto data = AtomicFileIO::safeReadJson(requestPath);
    std::filesystem::remove(requestPath, ec); // Delete immediately to prevent re-processing

    if (!data.has_value())
        return;

    try
    {
        auto& j = data.value();
        std::string file = j.value("file", "");
        std::string templateId = j.value("template_id", "");

        if (!file.empty())
        {
            setPendingSubmitRequest(file, templateId);
            MonitorLog::instance().info("farm", "Submit request received via CLI: " + file);
        }
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().warn("farm", std::string("Failed to parse submit request: ") + e.what());
    }
}

void MonitorApp::shutdown()
{
    // Stop heartbeat first (writes final "stopped" heartbeat)
    stopFarm();

    // Then stop agent
    m_agentSupervisor.stop();

    // Save config last
    saveConfig();
    MonitorLog::instance().info("farm", "Shutdown complete");
}

bool MonitorApp::startFarm()
{
    m_farmError.clear();

    auto result = FarmInit::init(m_config.sync_root, m_identity.nodeId());
    if (!result.success)
    {
        m_farmError = result.error;
        MonitorLog::instance().error("farm", "Farm init failed: " + m_farmError);
        return false;
    }

    m_farmPath = result.farmPath;
    MonitorLog::instance().startFileLogging(m_farmPath, m_identity.nodeId());
    m_heartbeatManager.setIsCoordinator(m_config.is_coordinator);
    m_heartbeatManager.start(m_farmPath, m_identity, m_config.timing, m_config.tags);

    m_commandManager.start(m_farmPath, m_identity.nodeId());

    if (m_config.is_coordinator)
    {
        // Check for existing coordinator
        auto nodes = m_heartbeatManager.getNodeSnapshot();
        for (const auto& n : nodes)
        {
            if (!n.isLocal && !n.isDead && n.heartbeat.is_coordinator)
            {
                m_farmError = "Another coordinator is already active: " +
                              n.heartbeat.hostname + " (" + n.heartbeat.node_id + ")";
                MonitorLog::instance().error("farm", m_farmError);
                m_commandManager.stop();
                m_heartbeatManager.stop();
                MonitorLog::instance().stopFileLogging();
                return false;
            }
        }

        // Start DispatchManager
        m_dispatchManager.start(
            m_farmPath, m_identity.nodeId(), getOS(),
            m_config.timing, m_config.tags,
            [this]() { return m_heartbeatManager.getNodeSnapshot(); },
            [this]() { return m_jobManager.getJobSnapshot(); }
        );

        m_dispatchManager.setLocalDispatchCallback(
            [this](const JobManifest& m, const ChunkRange& c) {
                m_renderCoordinator.queueDispatch(m, c);
            }
        );

        m_dispatchManager.setCommandSender(
            [this](const std::string& target, const std::string& type,
                   const std::string& jobId, const std::string& reason,
                   int frameStart, int frameEnd) {
                m_commandManager.sendCommand(target, type, jobId, reason, frameStart, frameEnd);
            }
        );

        // Coordinator: completions go to DispatchManager
        m_renderCoordinator.init(m_farmPath, m_identity.nodeId(), getOS(),
            [this](const std::string& jobId, const ChunkRange& chunk, const std::string& state) {
                m_dispatchManager.queueLocalCompletion(jobId, chunk, state);
            },
            &m_agentSupervisor
        );

        // Start SubmissionManager (coordinator processes DCC submission inbox)
        m_submissionManager.start(
            m_farmPath, m_identity.nodeId(), getOS(),
            [this](const std::string& templateId) -> std::optional<JobTemplate> {
                for (const auto& t : m_templateManager.templates())
                {
                    if (t.template_id == templateId && t.valid)
                        return t;
                }
                return std::nullopt;
            },
            [this](const JobManifest& manifest, int priority) -> std::string {
                return m_jobManager.submitJob(m_farmPath, manifest, priority);
            }
        );

        MonitorLog::instance().info("farm", "Started as coordinator");
    }
    else
    {
        // Worker: completions sent to coordinator via command file
        m_renderCoordinator.init(m_farmPath, m_identity.nodeId(), getOS(),
            [this](const std::string& jobId, const ChunkRange& chunk, const std::string& state) {
                std::string coordId = findCoordinatorNodeId();
                if (coordId.empty())
                {
                    MonitorLog::instance().warn("farm", "No coordinator found, buffering completion for retry");
                    m_pendingCompletions.push_back({jobId, chunk, state});
                    return;
                }
                std::string cmdType = (state == "completed") ? "chunk_completed" : "chunk_failed";
                m_commandManager.sendCommand(coordId, cmdType, jobId, state,
                                             chunk.frame_start, chunk.frame_end);
            },
            &m_agentSupervisor
        );

        MonitorLog::instance().info("farm", "Started as worker");
    }

    m_agentSupervisor.setMessageHandler(
        [this](const std::string& type, const nlohmann::json& j) {
            m_renderCoordinator.handleAgentMessage(type, j);
        }
    );

    m_farmRunning = true;

    MonitorLog::instance().info("farm", "Farm started at: " + m_farmPath.string());
    return true;
}

void MonitorApp::stopFarm()
{
    if (!m_farmRunning)
        return;

    m_commandManager.stop();

    if (m_config.is_coordinator)
    {
        m_dispatchManager.stop();
        m_submissionManager.stop();
    }

    m_heartbeatManager.stop();
    MonitorLog::instance().stopFileLogging();
    m_farmRunning = false;
    m_farmPath.clear();
    m_farmError.clear();
    m_nodeState = NodeState::Active;
    m_pendingCompletions.clear();
}

// ─── Coordinator query ──────────────────────────────────────────────────────

std::string MonitorApp::findCoordinatorNodeId() const
{
    auto nodes = m_heartbeatManager.getNodeSnapshot();
    for (const auto& n : nodes)
    {
        if (!n.isDead && n.heartbeat.is_coordinator)
            return n.heartbeat.node_id;
    }
    return {};
}

// ─── Worker: handle assign_chunk ─────────────────────────────────────────────

void MonitorApp::handleAssignChunk(const CommandManager::Action& action)
{
    if (m_renderCoordinator.isRendering())
    {
        // Already busy — report failure back to coordinator
        std::string coordId = findCoordinatorNodeId();
        if (!coordId.empty())
        {
            m_commandManager.sendCommand(coordId, "chunk_failed", action.jobId,
                                         "worker_busy", action.frameStart, action.frameEnd);
        }
        return;
    }

    // Read manifest from disk
    auto manifestPath = m_farmPath / "jobs" / action.jobId / "manifest.json";
    auto data = AtomicFileIO::safeReadJson(manifestPath);
    if (!data.has_value())
    {
        MonitorLog::instance().error("farm", "Can't read manifest for assigned job: " + action.jobId);
        std::string coordId = findCoordinatorNodeId();
        if (!coordId.empty())
        {
            m_commandManager.sendCommand(coordId, "chunk_failed", action.jobId,
                                         "manifest_read_failed", action.frameStart, action.frameEnd);
        }
        return;
    }

    try
    {
        JobManifest manifest = data.value().get<JobManifest>();
        ChunkRange chunk;
        chunk.frame_start = action.frameStart;
        chunk.frame_end = action.frameEnd;

        m_renderCoordinator.queueDispatch(manifest, chunk);
        MonitorLog::instance().info("farm", "Accepted assignment: job=" + action.jobId +
            " chunk=" + chunk.rangeStr());
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("farm", "Failed to parse manifest: " + std::string(e.what()));
        std::string coordId = findCoordinatorNodeId();
        if (!coordId.empty())
        {
            m_commandManager.sendCommand(coordId, "chunk_failed", action.jobId,
                                         "manifest_parse_failed", action.frameStart, action.frameEnd);
        }
    }
}

// ─── Worker: flush buffered completions ──────────────────────────────────────

void MonitorApp::flushPendingCompletions()
{
    std::string coordId = findCoordinatorNodeId();
    if (coordId.empty())
        return; // Still no coordinator — try again next update

    for (const auto& pc : m_pendingCompletions)
    {
        std::string cmdType = (pc.state == "completed") ? "chunk_completed" : "chunk_failed";
        m_commandManager.sendCommand(coordId, cmdType, pc.jobId, pc.state,
                                     pc.chunk.frame_start, pc.chunk.frame_end);
    }

    MonitorLog::instance().info("farm", "Flushed " + std::to_string(m_pendingCompletions.size()) +
                                " buffered completion(s) to coordinator");
    m_pendingCompletions.clear();
}

// ─── Job controls ───────────────────────────────────────────────────────────

void MonitorApp::pauseJob(const std::string& jobId)
{
    if (!m_farmRunning) return;

    // Get current priority
    int priority = 50;
    for (const auto& j : m_jobManager.jobs())
    {
        if (j.manifest.job_id == jobId)
        {
            priority = j.current_priority;
            break;
        }
    }

    m_jobManager.writeStateEntry(m_farmPath, jobId, "paused", priority, m_identity.nodeId());

    // Kill current render if it's this job
    if (m_renderCoordinator.currentJobId() == jobId)
        m_renderCoordinator.abortCurrentRender("Job paused");

    if (m_config.is_coordinator)
    {
        m_dispatchManager.handleJobStateChange(jobId, "paused");
    }

    // Notify peers
    auto nodes = m_heartbeatManager.getNodeSnapshot();
    for (const auto& n : nodes)
    {
        if (n.isLocal || n.isDead) continue;
        m_commandManager.sendCommand(n.heartbeat.node_id, "stop_job", jobId, "user_request");
    }

    MonitorLog::instance().info("job", "Paused job: " + jobId);
}

void MonitorApp::resumeJob(const std::string& jobId)
{
    if (!m_farmRunning) return;

    int priority = 50;
    for (const auto& j : m_jobManager.jobs())
    {
        if (j.manifest.job_id == jobId)
        {
            priority = j.current_priority;
            break;
        }
    }

    m_jobManager.writeStateEntry(m_farmPath, jobId, "active", priority, m_identity.nodeId());

    if (m_config.is_coordinator)
    {
        m_dispatchManager.handleJobStateChange(jobId, "active");
    }

    MonitorLog::instance().info("job", "Resumed job: " + jobId);
}

void MonitorApp::cancelJob(const std::string& jobId)
{
    if (!m_farmRunning) return;

    m_jobManager.writeStateEntry(m_farmPath, jobId, "cancelled", 0, m_identity.nodeId());

    // Abort current render if it's this job
    if (m_renderCoordinator.currentJobId() == jobId)
        m_renderCoordinator.abortCurrentRender("Job cancelled");

    if (m_config.is_coordinator)
    {
        m_dispatchManager.handleJobStateChange(jobId, "cancelled");
    }

    // Notify peers
    auto nodes = m_heartbeatManager.getNodeSnapshot();
    for (const auto& n : nodes)
    {
        if (n.isLocal || n.isDead) continue;
        m_commandManager.sendCommand(n.heartbeat.node_id, "stop_job", jobId, "job_cancelled");
    }

    MonitorLog::instance().info("job", "Cancelled job: " + jobId);
}

void MonitorApp::requeueJob(const std::string& jobId)
{
    if (!m_farmRunning) return;

    // Find the source job
    const JobInfo* source = nullptr;
    for (const auto& j : m_jobManager.jobs())
    {
        if (j.manifest.job_id == jobId)
        {
            source = &j;
            break;
        }
    }
    if (!source) return;

    // Build new slug: strip any existing "-requeueN" suffix, then find next number
    namespace fs = std::filesystem;
    std::error_code ec;
    auto jobsDir = m_farmPath / "jobs";

    std::string baseSlug = jobId;
    auto rqPos = baseSlug.rfind("-requeue");
    if (rqPos != std::string::npos)
        baseSlug = baseSlug.substr(0, rqPos);

    // Find highest existing requeue number to avoid recycling old numbers
    int maxN = 0;
    std::string prefix = baseSlug + "-requeue";
    for (auto& entry : fs::directory_iterator(jobsDir, ec))
    {
        if (!entry.is_directory(ec)) continue;
        std::string name = entry.path().filename().string();
        if (name.size() > prefix.size() && name.substr(0, prefix.size()) == prefix)
        {
            try { int n = std::stoi(name.substr(prefix.size())); if (n > maxN) maxN = n; }
            catch (...) {}
        }
    }

    std::string newSlug = baseSlug + "-requeue" + std::to_string(maxN + 1);
    if (newSlug.empty())
    {
        MonitorLog::instance().error("job", "Too many requeues for: " + jobId);
        return;
    }

    // Copy manifest with new identity
    JobManifest manifest = source->manifest;
    manifest.job_id = newSlug;
    manifest.submitted_by = m_identity.nodeId();
    auto now = std::chrono::system_clock::now();
    manifest.submitted_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    // Submit as new job with the original job's current priority
    auto result = m_jobManager.submitJob(m_farmPath, manifest, source->current_priority);
    if (result.empty())
    {
        MonitorLog::instance().error("job", "Failed to requeue job: " + jobId);
        return;
    }

    selectJob(newSlug);
    MonitorLog::instance().info("job", "Requeued job: " + jobId + " -> " + newSlug);
}

void MonitorApp::deleteJob(const std::string& jobId)
{
    if (!m_farmRunning) return;

    // Cancel first
    cancelJob(jobId);

    // Delete entire job directory
    namespace fs = std::filesystem;
    std::error_code ec;
    auto jobDir = m_farmPath / "jobs" / jobId;
    fs::remove_all(jobDir, ec);
    if (ec)
    {
        MonitorLog::instance().error("job", "Failed to delete job dir: " + ec.message());
    }

    m_jobManager.invalidate();
    m_selectedJobId.clear();

    MonitorLog::instance().info("job", "Deleted job: " + jobId);
}

// ─── Chunk controls ─────────────────────────────────────────────────────────

void MonitorApp::reassignChunk(const std::string& jobId, int frameStart, int frameEnd)
{
    if (!m_farmRunning || !m_config.is_coordinator) return;
    m_dispatchManager.reassignChunk(jobId, frameStart, frameEnd);
}

void MonitorApp::retryFailedChunk(const std::string& jobId, int frameStart, int frameEnd)
{
    if (!m_farmRunning || !m_config.is_coordinator) return;
    m_dispatchManager.retryFailedChunk(jobId, frameStart, frameEnd);
}

// ─── Node state ─────────────────────────────────────────────────────────────

void MonitorApp::setNodeState(NodeState state)
{
    m_nodeState = state;

    switch (state)
    {
    case NodeState::Active:
        m_renderCoordinator.setStopped(false);
        if (m_config.is_coordinator)
            m_dispatchManager.setNodeActive(true);
        m_heartbeatManager.setNodeState("active");
        MonitorLog::instance().info("farm", "Node state: Active");
        break;

    case NodeState::Stopped:
        if (m_renderCoordinator.isRendering())
            m_renderCoordinator.abortCurrentRender("Node stopped");
        m_renderCoordinator.setStopped(true);
        if (m_config.is_coordinator)
            m_dispatchManager.setNodeActive(false);
        m_heartbeatManager.setNodeState("stopped");
        MonitorLog::instance().info("farm", "Node state: Stopped");
        break;
    }
}

// ─── Tray state ─────────────────────────────────────────────────────────────

TrayIconState MonitorApp::trayState() const
{
    if (!m_farmRunning || m_nodeState == NodeState::Stopped)
        return TrayIconState::Gray;

    if (!m_agentSupervisor.isAgentConnected() && m_agentSupervisor.agentPid() != 0)
        return TrayIconState::Red;

    if (m_renderCoordinator.isRendering())
        return TrayIconState::Blue;

    return TrayIconState::Green;
}

std::string MonitorApp::trayTooltip() const
{
    std::string prefix = std::string("SmallRender v") + APP_VERSION;

    if (!m_farmRunning)
        return prefix + " — Farm not running";

    if (m_nodeState == NodeState::Stopped)
        return prefix + " — Stopped";

    if (!m_agentSupervisor.isAgentConnected() && m_agentSupervisor.agentPid() != 0)
        return prefix + " — Agent disconnected";

    if (m_renderCoordinator.isRendering())
    {
        return prefix + " — Rendering " + m_renderCoordinator.currentChunkLabel()
               + " of " + m_renderCoordinator.currentJobId();
    }

    return prefix + " — Idle";
}

std::string MonitorApp::trayStatusText() const
{
    if (!m_farmRunning)
        return "Farm not running";

    if (m_renderCoordinator.isRendering())
        return "Rendering " + m_renderCoordinator.currentChunkLabel();

    if (m_nodeState == NodeState::Stopped)
        return "Stopped";

    return "Idle";
}

// ─── Exit flow ──────────────────────────────────────────────────────────────

void MonitorApp::requestExit()
{
    m_exitRequested = true;

    if (!m_renderCoordinator.isRendering())
        m_shouldExit = true;
    // else: dashboard will show confirmation dialog
}

void MonitorApp::beginForceExit()
{
    m_renderCoordinator.abortCurrentRender("Force exit");
    m_shouldExit = true;
    MonitorLog::instance().info("farm", "Exit: kill and exit");
}

void MonitorApp::cancelExit()
{
    m_exitRequested = false;
    MonitorLog::instance().info("farm", "Exit cancelled");
}

// ─── Config ─────────────────────────────────────────────────────────────────

void MonitorApp::loadConfig()
{
    auto data = AtomicFileIO::safeReadJson(m_configPath);
    if (data.has_value())
    {
        try
        {
            m_config = data.value().get<Config>();
            MonitorLog::instance().info("farm", "Config loaded from: " + m_configPath.string());
        }
        catch (const std::exception& e)
        {
            MonitorLog::instance().error("farm", std::string("Config parse error, using defaults: ") + e.what());
            m_config = Config{};
        }
    }
    else
    {
        MonitorLog::instance().info("farm", "No config found, using defaults");
        m_config = Config{};
    }
}

void MonitorApp::saveConfig()
{
    ensureDir(m_appDataDir);
    nlohmann::json j = m_config;
    if (AtomicFileIO::writeJson(m_configPath, j))
    {
        MonitorLog::instance().info("farm", "Config saved to: " + m_configPath.string());
    }
    else
    {
        MonitorLog::instance().error("farm", "Failed to save config!");
    }
}

} // namespace SR
