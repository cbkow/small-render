#pragma once

#include "core/job_types.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <chrono>

namespace SR {

class SubmissionManager
{
public:
    using TemplateLoader = std::function<std::optional<JobTemplate>(const std::string&)>;
    using JobSubmitter = std::function<std::string(const JobManifest&, int)>;

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
};

} // namespace SR
