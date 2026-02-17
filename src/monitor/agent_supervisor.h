#pragma once

#include "core/ipc_server.h"

#include <nlohmann/json.hpp>

#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace SR {

/// Manages the agent process lifecycle and IPC communication.
/// Owns the IPC server, runs a background thread for receiving messages,
/// and provides main-thread-safe methods for processing incoming messages.
class AgentSupervisor
{
public:
    AgentSupervisor();
    ~AgentSupervisor();

    AgentSupervisor(const AgentSupervisor&) = delete;
    AgentSupervisor& operator=(const AgentSupervisor&) = delete;

    /// Start the IPC server and background thread.
    /// Call once during MonitorApp::init().
    void start(const std::string& nodeId);

    /// Stop everything: signal agent to shutdown, join threads, close pipe.
    void stop();

    /// Spawn the agent process (sr-agent.exe --node-id <id>).
    /// Looks for sr-agent.exe next to the monitor executable.
    bool spawnAgent();

    /// Send shutdown message to agent, wait for exit, then force-kill if needed.
    void shutdownAgent();

    /// Immediately terminate the agent process.
    void killAgent();

    /// Send a ping to the agent.
    void sendPing();

    /// Send a task JSON string to the agent.
    bool sendTask(const std::string& taskJson);

    /// Send abort command to agent (stop current render).
    void sendAbort(const std::string& reason);

    /// Set handler for agent messages (ack, progress, stdout, completed, failed).
    void setMessageHandler(std::function<void(const std::string&, const nlohmann::json&)> handler);

    /// Process received messages on the main thread. Call each frame.
    void processMessages();

    /// Is the agent process running?
    bool isAgentRunning() const;

    /// Is the agent connected via IPC?
    bool isAgentConnected() const { return m_ipc.isConnected(); }

    /// Agent PID (0 if not running)
    uint32_t agentPid() const { return m_agentPid; }

    /// Agent reported state ("idle", "rendering", or empty)
    const std::string& agentState() const { return m_agentState; }

private:
    void ipcThreadFunc();
    bool sendJson(const std::string& json);

    IpcServer m_ipc;
    std::string m_nodeId;

    // IPC thread
    std::thread m_ipcThread;
    std::atomic<bool> m_running{false};

    // Thread-safe message queue (IPC thread produces, main thread consumes)
    std::queue<std::string> m_messageQueue;
    std::mutex m_queueMutex;

    // Agent process
#ifdef _WIN32
    HANDLE m_processHandle = nullptr;
    HANDLE m_threadHandle = nullptr;
#endif
    uint32_t m_agentPid = 0;
    std::string m_agentState;

    // Ping tracking
    std::chrono::steady_clock::time_point m_lastPingTime;
    static constexpr int PING_INTERVAL_SECONDS = 30;

    // Message handler callback
    std::function<void(const std::string&, const nlohmann::json&)> m_messageHandler;
};

} // namespace SR
