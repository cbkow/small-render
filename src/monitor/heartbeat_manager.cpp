#include "monitor/heartbeat_manager.h"
#include "core/atomic_file_io.h"
#include "core/platform.h"

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
    m_seq      = 0;
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

    m_seq++;
    auto hb = buildHeartbeat();
    nlohmann::json j = hb;

    auto path = m_nodesDir / m_nodeId / "heartbeat.json";
    if (!AtomicFileIO::writeJson(path, j))
    {
        MonitorLog::instance().error("health", "Failed to write heartbeat (seq=" + std::to_string(m_seq) + ")");
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

    m_seq++;
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
    hb.seq = m_seq;
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
            auto& info = m_nodes[peerId];
            info.heartbeat = hb;
            info.isLocal = (peerId == m_nodeId);
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

        // Check if node explicitly stopped
        if (info.heartbeat.node_state == "stopped")
        {
            info.isDead = true;
            info.reclaimEligible = true;
            continue;
        }

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
