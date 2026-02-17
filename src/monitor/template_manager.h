#pragma once

#include "core/job_types.h"

#include <filesystem>
#include <vector>
#include <string>
#include <optional>
#include <chrono>

namespace SR {

class TemplateManager
{
public:
    void scan(const std::filesystem::path& farmPath);
    const std::vector<JobTemplate>& templates() const { return m_templates; }

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
    std::vector<JobTemplate> m_templates;
    std::chrono::steady_clock::time_point m_lastScan{};
    static constexpr int SCAN_COOLDOWN_MS = 5000;

    void loadTemplatesFromDir(const std::filesystem::path& dir, bool isExample);
};

} // namespace SR
