#include "monitor/heartbeat_manager.h"
#include "core/atomic_file_io.h"
#include "core/platform.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include "core/monitor_log.h"

namespace SR {

namespace fs = std::filesystem;

HeartbeatManager::~HeartbeatManager()
{
    if (m_running.load())
        stop();
}

void HeartbeatManager::start(const fs::path& farmPath,
                             const NodeIdentity& identity,
                             const TimingConfig& timing,
                             const std::vector<std::string>& tags)
{
    if (m_running.load())
        return;

    m_farmPath = farmPath;
    m_nodesDir = farmPath / "nodes";
    m_nodeId   = identity.nodeId();
    m_hostname = identity.systemInfo().hostname;
    m_os       = getOS();
    m_gpuName  = identity.systemInfo().gpuName;
    m_cpuCores = identity.systemInfo().cpuCores;
    m_ramGb    = identity.systemInfo().ramMB / 1024;
    m_timing   = timing;
    m_tags     = tags;
    m_seq.store(0);
    m_nodeState = "active";
    m_renderState = "idle";
    m_activeJob.clear();
    m_activeFrames.clear();

    m_running.store(true);

    // Write first heartbeat immediately (on caller thread for visibility)
    writeHeartbeat();

    m_thread = std::thread(&HeartbeatManager::threadFunc, this);

    MonitorLog::instance().info("health", "Started (heartbeat=" + std::to_string(timing.heartbeat_interval_ms) + "ms, scan=" + std::to_string(timing.scan_interval_ms) + "ms, dead_scans=" + std::to_string(timing.dead_threshold_scans) + ")");
}

void HeartbeatManager::stop()
{
    if (!m_running.load())
        return;

    m_running.store(false);
    if (m_thread.joinable())
        m_thread.join();

    writeFinalHeartbeat();

    MonitorLog::instance().info("health", "Stopped");
}

std::vector<NodeInfo> HeartbeatManager::getNodeSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<NodeInfo> result;
    result.reserve(m_nodes.size());
    for (const auto& [id, info] : m_nodes)
        result.push_back(info);
    return result;
}

void HeartbeatManager::updateTiming(const TimingConfig& timing)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_timing = timing;
}

void HeartbeatManager::updateTags(const std::vector<std::string>& tags)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_tags = tags;
}

void HeartbeatManager::setIsCoordinator(bool coordinator)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_isCoordinator = coordinator;
}

void HeartbeatManager::setRenderState(const std::string& state,
                                       const std::string& activeJob,
                                       const std::string& activeFrames)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_renderState = state;
    m_activeJob = activeJob;
    m_activeFrames = activeFrames;
}

void HeartbeatManager::setNodeState(const std::string& state)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_nodeState = state;
}

void HeartbeatManager::processUdpHeartbeat(const nlohmann::json& msg)
{
    std::string peerId = msg.value("n", "");
    if (peerId.empty() || peerId == m_nodeId)
        return;

    std::lock_guard<std::mutex> lock(m_mutex);

    auto& info = m_nodes[peerId];
    auto myNow = nowMs();

    uint64_t seq = msg.value("seq", uint64_t(0));
    if (seq > info.lastSeenSeq)
    {
        info.lastSeenSeq = seq;
        info.staleCount = 0;
        info.isDead = false;
    }

    // Update fast-changing fields from compact UDP heartbeat
    info.heartbeat.node_id = peerId;
    info.heartbeat.seq = seq;
    info.heartbeat.timestamp_ms = msg.value("ts", int64_t(0));
    info.heartbeat.node_state = msg.value("st", std::string("active"));
    info.heartbeat.render_state = msg.value("rs", std::string("idle"));
    info.heartbeat.is_coordinator = msg.value("coord", false);
    if (msg.contains("job") && !msg["job"].is_null())
        info.heartbeat.active_job = msg["job"].get<std::string>();
    else
        info.heartbeat.active_job.clear();

    info.isLocal = false;
    info.hasUdpContact = true;
    info.lastUdpContactMs = myNow;
}

// --- Background thread ---

void HeartbeatManager::threadFunc()
{
    using clock = std::chrono::steady_clock;

    auto lastHeartbeat = clock::now();
    auto lastScan = clock::time_point{}; // trigger immediate first scan

    while (m_running.load())
    {
        try
        {
            auto now = clock::now();

            TimingConfig timing;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                timing = m_timing;
            }

            auto hbElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHeartbeat).count();
            auto scanElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastScan).count();

            if (hbElapsed >= static_cast<int64_t>(timing.heartbeat_interval_ms))
            {
                writeHeartbeat();
                lastHeartbeat = clock::now();
            }

            if (scanElapsed >= static_cast<int64_t>(timing.scan_interval_ms))
            {
                scanPeers();
                detectStaleness();
                detectClockSkew();
                lastScan = clock::now();
            }

            // Sleep until next event, but no longer than 500ms (responsive to stop)
            auto timeToNextHb = static_cast<int64_t>(timing.heartbeat_interval_ms) - hbElapsed;
            auto timeToNextScan = static_cast<int64_t>(timing.scan_interval_ms) - scanElapsed;
            auto sleepMs = std::min({timeToNextHb, timeToNextScan, int64_t(500)});
            if (sleepMs < 10) sleepMs = 10;

            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }
        catch (const std::exception& e)
        {
            MonitorLog::instance().error("health", std::string("Thread exception: ") + e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
        catch (...)
        {
            MonitorLog::instance().error("health", "Thread unknown exception");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    }
}

void HeartbeatManager::writeHeartbeat()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_seq.fetch_add(1);
    auto hb = buildHeartbeat();
    nlohmann::json j = hb;

    auto path = m_nodesDir / m_nodeId / "heartbeat.json";
    if (!AtomicFileIO::writeJson(path, j))
    {
        MonitorLog::instance().error("health", "Failed to write heartbeat (seq=" + std::to_string(m_seq.load()) + ")");
    }

    // Update local node in map
    auto& local = m_nodes[m_nodeId];
    local.heartbeat = hb;
    local.isLocal = true;
    local.isDead = false;
    local.staleCount = 0;
    local.lastSeenSeq = hb.seq;
}

void HeartbeatManager::writeFinalHeartbeat()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_seq.fetch_add(1);
    auto hb = buildHeartbeat();
    hb.node_state = "stopped";
    nlohmann::json j = hb;

    auto path = m_nodesDir / m_nodeId / "heartbeat.json";
    AtomicFileIO::writeJson(path, j);
}

Heartbeat HeartbeatManager::buildHeartbeat() const
{
    // Must be called with m_mutex held
    Heartbeat hb;
    hb.node_id = m_nodeId;
    hb.hostname = m_hostname;
    hb.os = m_os;
    hb.app_version = APP_VERSION;
    hb.protocol_version = PROTOCOL_VERSION;
    hb.seq = m_seq.load();
    hb.timestamp_ms = nowMs();
    hb.node_state = m_nodeState;
    hb.render_state = m_renderState;
    hb.active_job = m_activeJob;
    hb.active_frames = m_activeFrames;
    hb.gpu_name = m_gpuName;
    hb.cpu_cores = m_cpuCores;
    hb.ram_gb = m_ramGb;
    hb.tags = m_tags;
    hb.is_coordinator = m_isCoordinator;
    return hb;
}

int64_t HeartbeatManager::nowMs() const
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void HeartbeatManager::scanPeers()
{
    std::error_code ec;
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& entry : fs::directory_iterator(m_nodesDir, ec))
    {
        if (!entry.is_directory(ec))
            continue;

        std::string peerId = entry.path().filename().string();
        auto hbPath = entry.path() / "heartbeat.json";

        auto data = AtomicFileIO::safeReadJson(hbPath);
        if (!data.has_value())
            continue;

        try
        {
            Heartbeat hb = data.value().get<Heartbeat>();
            bool isNew = (m_nodes.find(peerId) == m_nodes.end());
            auto& info = m_nodes[peerId];
            info.heartbeat = hb;
            info.isLocal = (peerId == m_nodeId);

            // Seed lastSeenSeq on first discovery so the node must
            // advance its seq to prove it's alive (not just stale on disk)
            if (isNew && !info.isLocal)
                info.lastSeenSeq = hb.seq;
        }
        catch (const std::exception& e)
        {
            MonitorLog::instance().error("health", "Failed to parse heartbeat for " + peerId + ": " + std::string(e.what()));
        }
    }
}

void HeartbeatManager::detectStaleness()
{
    // Must be called with m_mutex held (from scanPeers context)
    for (auto& [id, info] : m_nodes)
    {
        if (info.isLocal)
            continue;

        // Seq-based staleness detection (runs for all nodes including stopped)
        if (info.heartbeat.seq == info.lastSeenSeq)
        {
            info.staleCount++;
        }
        else
        {
            info.staleCount = 0;
            info.isDead = false;
            info.reclaimEligible = false;
        }

        info.lastSeenSeq = info.heartbeat.seq;

        // Stopped nodes are alive but their chunks can be reassigned
        if (!info.isDead && info.heartbeat.node_state == "stopped")
        {
            info.reclaimEligible = true;
        }

        if (info.staleCount >= m_timing.dead_threshold_scans)
        {
            if (!info.isDead)
            {
                info.isDead = true;
                info.reclaimEligible = false; // grace period: one more scan
                MonitorLog::instance().warn("health", "Node DEAD: " + id + " (stale for " + std::to_string(info.staleCount) + " scans)");
            }
            else
            {
                info.reclaimEligible = true;
            }
        }

        // UDP contact timeout: if no UDP heartbeat for 15s, fall back to filesystem detection
        if (info.hasUdpContact && (nowMs() - info.lastUdpContactMs > 15000))
            info.hasUdpContact = false;
    }
}

void HeartbeatManager::detectClockSkew()
{
    // Must be called with m_mutex held
    int64_t myNow = nowMs();
    int skewedCount = 0;
    int aliveCount = 0;

    for (auto& [id, info] : m_nodes)
    {
        if (info.isLocal || info.isDead)
            continue;

        aliveCount++;
        int64_t skew = std::abs(myNow - info.heartbeat.timestamp_ms);
        info.clockSkewWarning = (skew > CLOCK_SKEW_WARN_MS);
        info.skewAmountMs = myNow - info.heartbeat.timestamp_ms;

        if (info.clockSkewWarning)
            skewedCount++;
    }

    // If majority of alive peers show skew, we're the odd one out
    m_localClockSkew.store(aliveCount > 0 && skewedCount > aliveCount / 2);
}

} // namespace SR
