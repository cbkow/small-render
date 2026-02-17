#pragma once

#include "core/job_types.h"

#include <filesystem>
#include <vector>
#include <string>
#include <optional>
#include <chrono>
#include <mutex>
#include <thread>
#include <atomic>

namespace SR {

class TemplateManager
{
public:
    ~TemplateManager();

    TemplateManager() = default;
    TemplateManager(const TemplateManager&) = delete;
    TemplateManager& operator=(const TemplateManager&) = delete;

    // Start background scanning thread. First scan is synchronous.
    void start(const std::filesystem::path& farmPath);

    // Stop background thread.
    void stop();

    // Thread-safe snapshot for UI
    std::vector<JobTemplate> getTemplateSnapshot() const;

    JobManifest bakeManifest(const JobTemplate& tmpl,
                             const std::vector<std::string>& flagValues,
                             const std::string& cmdForMyOS,
                             const std::string& jobSlug,
                             int frameStart, int frameEnd, int chunkSize,
                             int maxRetries, std::optional<int> timeout,
                             const std::string& nodeId, const std::string& os) const;

    // Static version for use by SubmissionManager (same logic, no instance needed)
    static JobManifest bakeManifestStatic(const JobTemplate& tmpl,
                             const std::vector<std::string>& flagValues,
                             const std::string& cmdForMyOS,
                             const std::string& jobSlug,
                             int frameStart, int frameEnd, int chunkSize,
                             int maxRetries, std::optional<int> timeout,
                             const std::string& nodeId, const std::string& os);

    std::string buildCommandPreview(const JobTemplate& tmpl,
                                    const std::vector<std::string>& flagValues,
                                    const std::string& cmdPath) const;

    static std::string generateSlug(const std::string& jobName,
                                    const std::filesystem::path& jobsDir);

    static std::string resolvePattern(
        const std::string& pattern,
        const JobTemplate& tmpl,
        const std::vector<std::string>& flagValues,
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    static bool validateTemplate(JobTemplate& tmpl);

    static std::vector<std::string> validateSubmission(
        const JobTemplate& tmpl, const std::vector<std::string>& flagValues,
        const std::string& cmdPath, const std::string& jobName,
        int frameStart, int frameEnd, int chunkSize,
        const std::filesystem::path& jobsDir);

private:
    void threadFunc();
    std::vector<JobTemplate> doScan();
    void loadTemplatesFromDir(const std::filesystem::path& dir, bool isExample,
                              std::vector<JobTemplate>& out);

    std::filesystem::path m_farmPath;
    std::vector<JobTemplate> m_templates;
    mutable std::mutex m_mutex;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    static constexpr int SCAN_COOLDOWN_MS = 5000;
};

} // namespace SR
