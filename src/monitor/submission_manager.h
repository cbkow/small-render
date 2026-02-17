#pragma once

#include "core/job_types.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>

namespace SR {

class SubmissionManager
{
public:
    using TemplateLoader = std::function<std::optional<JobTemplate>(const std::string&)>;
    using JobSubmitter = std::function<std::string(const JobManifest&, int)>;

    ~SubmissionManager();

    SubmissionManager() = default;
    SubmissionManager(const SubmissionManager&) = delete;
    SubmissionManager& operator=(const SubmissionManager&) = delete;

    void start(const std::filesystem::path& farmPath,
               const std::string& nodeId,
               const std::string& os,
               TemplateLoader templateLoader,
               JobSubmitter jobSubmitter);
    void stop();
    void update();  // Called from MonitorApp::update() on coordinator only
    void wakeUp();  // Reset poll timer for immediate check (called on UDP notification)

private:
    void pollInbox();
    void processSubmission(const std::filesystem::path& file);
    void purgeProcessed();

    std::filesystem::path m_farmPath;
    std::string m_nodeId;
    std::string m_os;
    bool m_running = false;

    TemplateLoader m_templateLoader;
    JobSubmitter m_jobSubmitter;

    std::chrono::steady_clock::time_point m_lastPoll{};
    std::chrono::steady_clock::time_point m_lastPurge{};
    static constexpr int POLL_INTERVAL_MS = 5000;
    static constexpr int PURGE_INTERVAL_MS = 3600000; // 1 hour

    // Track unreadable files for retry (cloud FS propagation delay)
    std::map<std::string, int> m_readFailCounts;  // filename → retry count
    static constexpr int MAX_READ_RETRIES = 6;    // 6 * 5s = 30s max wait

    // Background thread
    std::thread m_thread;
    std::atomic<bool> m_threadRunning{false};
    std::atomic<bool> m_wakeFlag{false};

    void threadFunc();
};

} // namespace SR
