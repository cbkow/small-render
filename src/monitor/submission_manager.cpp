#include "monitor/submission_manager.h"
#include "core/atomic_file_io.h"
#include "core/monitor_log.h"
#include "monitor/template_manager.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <chrono>

namespace SR {

namespace fs = std::filesystem;

SubmissionManager::~SubmissionManager()
{
    stop();
}

void SubmissionManager::start(const fs::path& farmPath,
                              const std::string& nodeId,
                              const std::string& os,
                              TemplateLoader templateLoader,
                              JobSubmitter jobSubmitter)
{
    m_farmPath = farmPath;
    m_nodeId = nodeId;
    m_os = os;
    m_templateLoader = std::move(templateLoader);
    m_jobSubmitter = std::move(jobSubmitter);
    m_running = true;

    // Ensure submissions directories exist
    std::error_code ec;
    fs::create_directories(m_farmPath / "submissions" / "processed", ec);

    // Start background thread
    m_threadRunning.store(true);
    m_thread = std::thread(&SubmissionManager::threadFunc, this);

    MonitorLog::instance().info("farm", "SubmissionManager started");
}

void SubmissionManager::stop()
{
    m_running = false;
    m_threadRunning.store(false);
    if (m_thread.joinable())
        m_thread.join();
}

void SubmissionManager::wakeUp()
{
    m_wakeFlag.store(true);
}

void SubmissionManager::update()
{
    // No-op: all work done on bg thread now
}

void SubmissionManager::threadFunc()
{
    while (m_threadRunning.load())
    {
        for (int i = 0; i < 10 && m_threadRunning.load(); ++i)
        {
            if (m_wakeFlag.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (!m_threadRunning.load()) break;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastPoll).count();
        bool woken = m_wakeFlag.exchange(false);

        if (elapsed < POLL_INTERVAL_MS && !woken)
            continue;

        m_lastPoll = now;

        try
        {
            pollInbox();
        }
        catch (const std::exception& e)
        {
            MonitorLog::instance().error("farm", std::string("SubmissionManager exception: ") + e.what());
        }
        catch (...)
        {
            MonitorLog::instance().error("farm", "SubmissionManager unknown exception");
        }

        // Periodic purge of old processed files
        auto purgeElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastPurge).count();
        if (purgeElapsed >= PURGE_INTERVAL_MS)
        {
            m_lastPurge = now;
            purgeProcessed();
        }
    }
}

void SubmissionManager::pollInbox()
{
    auto inboxDir = m_farmPath / "submissions";
    std::error_code ec;

    if (!fs::is_directory(inboxDir, ec)) return;

    // Collect and sort JSON files
    std::vector<fs::path> files;
    for (auto& entry : fs::directory_iterator(inboxDir, ec))
    {
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != ".json") continue;
        files.push_back(entry.path());
    }

    // Sort by filename (timestamp-based = chronological order)
    std::sort(files.begin(), files.end());

    for (const auto& file : files)
    {
        processSubmission(file);
    }
}

void SubmissionManager::processSubmission(const fs::path& file)
{
    auto data = AtomicFileIO::safeReadJson(file);
    if (!data.has_value())
    {
        std::string fname = file.filename().string();
        int& count = m_readFailCounts[fname];
        ++count;
        if (count >= MAX_READ_RETRIES)
        {
            MonitorLog::instance().error("farm", "Giving up on unreadable submission after " +
                                         std::to_string(count) + " retries: " + fname);
            std::error_code ec;
            fs::rename(file, m_farmPath / "submissions" / "processed" / file.filename(), ec);
            m_readFailCounts.erase(fname);
        }
        else
        {
            MonitorLog::instance().info("farm", "Submission not yet readable (retry " +
                                        std::to_string(count) + "): " + fname);
        }
        return;
    }

    // Clear retry counter on successful read
    m_readFailCounts.erase(file.filename().string());

    try
    {
        const auto& j = data.value();

        std::string templateId = j.value("template_id", "");
        std::string jobName = j.value("job_name", "");
        std::string submittedByHost = j.value("submitted_by_host", "");

        if (templateId.empty())
        {
            MonitorLog::instance().error("farm", "Submission missing template_id: " + file.filename().string());
            std::error_code ec;
            fs::rename(file, m_farmPath / "submissions" / "processed" / file.filename(), ec);
            return;
        }

        // Load template
        auto tmplOpt = m_templateLoader(templateId);
        if (!tmplOpt.has_value())
        {
            MonitorLog::instance().error("farm", "Template not found for submission: " + templateId);
            std::error_code ec;
            fs::rename(file, m_farmPath / "submissions" / "processed" / file.filename(), ec);
            return;
        }

        auto tmpl = tmplOpt.value();

        // Apply overrides from submission
        if (j.contains("overrides") && j["overrides"].is_object())
        {
            for (auto& [key, val] : j["overrides"].items())
            {
                std::string overrideVal = val.get<std::string>();
                bool found = false;
                for (auto& flag : tmpl.flags)
                {
                    if (!flag.id.empty() && flag.id == key)
                    {
                        flag.value = overrideVal;
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    MonitorLog::instance().warn("farm", "Submission override key not found in template: " + key);
                }
            }
        }

        // Override job settings
        int frameStart = j.value("frame_start", tmpl.job_defaults.frame_start);
        int frameEnd = j.value("frame_end", tmpl.job_defaults.frame_end);
        int chunkSize = j.value("chunk_size", tmpl.job_defaults.chunk_size);
        int priority = j.value("priority", tmpl.job_defaults.priority);
        int maxRetries = j.value("max_retries", tmpl.job_defaults.max_retries);
        std::optional<int> timeout = tmpl.job_defaults.timeout_seconds;
        if (j.contains("timeout_seconds"))
        {
            if (j["timeout_seconds"].is_null())
                timeout = std::nullopt;
            else
                timeout = j["timeout_seconds"].get<int>();
        }

        // Build flag values
        std::vector<std::string> flagValues;
        flagValues.reserve(tmpl.flags.size());
        for (const auto& flag : tmpl.flags)
        {
            if (flag.value.has_value())
                flagValues.push_back(flag.value.value());
            else
                flagValues.push_back("");
        }

        // Generate slug
        auto jobsDir = m_farmPath / "jobs";
        if (jobName.empty())
            jobName = templateId + "-batch";
        auto slug = TemplateManager::generateSlug(jobName, jobsDir);
        if (slug.empty())
        {
            MonitorLog::instance().error("farm", "Failed to generate slug for submission: " + jobName);
            std::error_code ec;
            fs::rename(file, m_farmPath / "submissions" / "processed" / file.filename(), ec);
            return;
        }

        // Get cmd for coordinator's OS (used as fallback)
        std::string cmdPath = getCmdForOS(tmpl.cmd, m_os);

        // Bake manifest
        auto manifest = TemplateManager::bakeManifestStatic(
            tmpl, flagValues, cmdPath, slug,
            frameStart, frameEnd, chunkSize,
            maxRetries, timeout, m_nodeId, m_os);

        // Submit
        auto result = m_jobSubmitter(manifest, priority);
        if (result.empty())
        {
            MonitorLog::instance().error("farm", "Failed to submit job from submission: " + jobName);
        }
        else
        {
            MonitorLog::instance().info("farm", "Auto-submitted job '" + result + "' from " +
                                        submittedByHost + " (template: " + templateId + ")");
        }
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("farm", "Exception processing submission " +
                                     file.filename().string() + ": " + e.what());
    }

    // Always move to processed (even on error, to prevent retry loop)
    std::error_code ec;
    fs::rename(file, m_farmPath / "submissions" / "processed" / file.filename(), ec);
}

void SubmissionManager::purgeProcessed()
{
    auto processedDir = m_farmPath / "submissions" / "processed";
    std::error_code ec;

    if (!fs::is_directory(processedDir, ec)) return;

    auto now = std::chrono::system_clock::now();
    auto cutoff = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() - 86400000; // 24 hours

    for (auto& entry : fs::directory_iterator(processedDir, ec))
    {
        if (!entry.is_regular_file(ec)) continue;

        // Extract timestamp from filename (first 13 digits before '.')
        std::string name = entry.path().stem().string();
        auto dotPos = name.find('.');
        if (dotPos == std::string::npos) continue;

        try
        {
            int64_t ts = std::stoll(name.substr(0, dotPos));
            if (ts < cutoff)
            {
                fs::remove(entry.path(), ec);
            }
        }
        catch (...) {}
    }
}

} // namespace SR
