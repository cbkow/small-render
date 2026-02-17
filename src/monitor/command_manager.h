#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>

namespace SR {

class CommandManager
{
public:
    CommandManager() = default;
    ~CommandManager();

    CommandManager(const CommandManager&) = delete;
    CommandManager& operator=(const CommandManager&) = delete;

    void start(const std::filesystem::path& farmPath,
               const std::string& nodeId);
    void stop();

    // Send a command to a target node's inbox (thread-safe).
    void sendCommand(const std::string& targetNodeId,
                     const std::string& type,
                     const std::string& jobId = {},
                     const std::string& reason = "user_request",
                     int frameStart = 0,
                     int frameEnd = 0);

    // Action queued from inbox polling, consumed by main thread.
    struct Action
    {
        std::string type;
        std::string jobId;
        std::string reason;
        int frameStart = 0;
        int frameEnd = 0;
        std::string fromNodeId;
    };

    // Pop all pending actions (main thread).
    std::vector<Action> popActions();

private:
    void threadFunc();
    void pollInbox();
    void purgeProcessed();

    std::filesystem::path m_farmPath;
    std::string m_nodeId;
    std::thread m_thread;
    std::atomic<bool> m_running{false};

    // Action queue (bg thread -> main thread)
    std::queue<Action> m_actionQueue;
    std::mutex m_mutex;
};

} // namespace SR
