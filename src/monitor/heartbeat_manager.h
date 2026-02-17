#pragma once

#include "core/heartbeat.h"
#include "core/config.h"
#include "core/node_identity.h"

#include <filesystem>
#include <vector>
#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>

namespace SR {

class HeartbeatManager
{
public:
    HeartbeatManager() = default;
    ~HeartbeatManager();

    HeartbeatManager(const HeartbeatManager&) = delete;
    HeartbeatManager& operator=(const HeartbeatManager&) = delete;

    // Start background thread: writes heartbeats, scans peers, detects staleness.
    void start(const std::filesystem::path& farmPath,
               const NodeIdentity& identity,
               const TimingConfig& timing,
               const std::vector<std::string>& tags);

    // Stop background thread. Writes final "stopped" heartbeat before returning.
    void stop();

    // Thread-safe snapshot of all known nodes (local + peers).
    std::vector<NodeInfo> getNodeSnapshot() const;

    // True if this node's clock is skewed vs majority of alive peers.
    bool hasLocalClockSkew() const { return m_localClockSkew.load(); }

    // Live config updates (thread-safe).
    void updateTiming(const TimingConfig& timing);
    void updateTags(const std::vector<std::string>& tags);
    void setIsCoordinator(bool coordinator);

    // Live render state updates (thread-safe, called from main thread).
    void setRenderState(const std::string& state,
                        const std::string& activeJob,
                        const std::string& activeFrames);
    void setNodeState(const std::string& state);

private:
    void threadFunc();
    void writeHeartbeat();
    void writeFinalHeartbeat();
    void scanPeers();
    void detectStaleness();
    void detectClockSkew();

    Heartbeat buildHeartbeat() const;
    int64_t nowMs() const;

    // Config (protected by m_mutex)
    std::filesystem::path m_farmPath;
    std::filesystem::path m_nodesDir;
    std::string m_nodeId;
    std::string m_hostname;
    std::string m_os;
    std::string m_gpuName;
    int m_cpuCores = 0;
    uint64_t m_ramGb = 0;
    TimingConfig m_timing;
    std::vector<std::string> m_tags;
    bool m_isCoordinator = false;

    // Dynamic state (updated from main thread via setters)
    std::string m_nodeState = "active";
    std::string m_renderState = "idle";
    std::string m_activeJob;
    std::string m_activeFrames;

    // State
    uint64_t m_seq = 0;
    std::map<std::string, NodeInfo> m_nodes;  // node_id -> info

    // Thread
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    mutable std::mutex m_mutex;
    std::atomic<bool> m_localClockSkew{false};
};

} // namespace SR
