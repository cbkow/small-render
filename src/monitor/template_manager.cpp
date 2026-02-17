#include "monitor/template_manager.h"
#include "core/atomic_file_io.h"
#include "core/platform.h"
#include "core/monitor_log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iostream>
#include <set>

namespace SR {

TemplateManager::~TemplateManager()
{
    stop();
}

void TemplateManager::start(const std::filesystem::path& farmPath)
{
    if (m_running.load()) return;

    m_farmPath = farmPath;

    // First scan synchronous — data available immediately
    auto result = doScan();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_templates = std::move(result);
    }

    m_running.store(true);
    m_thread = std::thread(&TemplateManager::threadFunc, this);
}

void TemplateManager::stop()
{
    m_running.store(false);
    if (m_thread.joinable())
        m_thread.join();
}

void TemplateManager::threadFunc()
{
    auto lastScan = std::chrono::steady_clock::now();

    while (m_running.load())
    {
        for (int i = 0; i < 10 && m_running.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

        if (!m_running.load()) break;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastScan).count();
        if (elapsed < SCAN_COOLDOWN_MS)
            continue;

        lastScan = now;

        try
        {
            auto result = doScan();
            std::lock_guard<std::mutex> lock(m_mutex);
            m_templates = std::move(result);
        }
        catch (const std::exception& e)
        {
            MonitorLog::instance().error("farm", std::string("Template scan exception: ") + e.what());
        }
        catch (...)
        {
            MonitorLog::instance().error("farm", "Template scan unknown exception");
        }
    }
}

std::vector<JobTemplate> TemplateManager::doScan()
{
    std::vector<JobTemplate> templates;

    // Load examples first, then user templates
    loadTemplatesFromDir(m_farmPath / "templates" / "examples", true, templates);
    loadTemplatesFromDir(m_farmPath / "templates", false, templates);

    // User templates with same template_id override examples
    std::set<std::string> userIds;
    for (const auto& t : templates)
    {
        if (!t.isExample)
            userIds.insert(t.template_id);
    }

    templates.erase(
        std::remove_if(templates.begin(), templates.end(),
            [&](const JobTemplate& t) {
                return t.isExample && userIds.count(t.template_id) > 0;
            }),
        templates.end());

    return templates;
}

std::vector<JobTemplate> TemplateManager::getTemplateSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_templates;
}

void TemplateManager::loadTemplatesFromDir(const std::filesystem::path& dir, bool isExample,
                                            std::vector<JobTemplate>& out)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return;

    for (const auto& entry : fs::directory_iterator(dir, ec))
    {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json")
            continue;

        // Skip farm.json
        if (entry.path().filename() == "farm.json")
            continue;

        auto data = AtomicFileIO::safeReadJson(entry.path());
        if (!data.has_value())
        {
            // Create an invalid template entry so user sees the error
            JobTemplate invalid;
            invalid.template_id = entry.path().stem().string();
            invalid.name = invalid.template_id;
            invalid.valid = false;
            invalid.validation_error = "Failed to parse JSON";
            invalid.isExample = isExample;
            out.push_back(std::move(invalid));
            continue;
        }

        try
        {
            auto tmpl = data.value().get<JobTemplate>();
            tmpl.isExample = isExample;
            validateTemplate(tmpl);
            out.push_back(std::move(tmpl));
        }
        catch (const std::exception& e)
        {
            JobTemplate invalid;
            invalid.template_id = entry.path().stem().string();
            invalid.name = invalid.template_id;
            invalid.valid = false;
            invalid.validation_error = std::string("Parse error: ") + e.what();
            invalid.isExample = isExample;
            out.push_back(std::move(invalid));
        }
    }
}

// ─── Pattern resolution ──────────────────────────────────────────────────────

static void replaceAll(std::string& str, const std::string& from, const std::string& to)
{
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos)
    {
        str.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string TemplateManager::resolvePattern(
    const std::string& pattern,
    const JobTemplate& tmpl,
    const std::vector<std::string>& flagValues,
    std::chrono::system_clock::time_point now)
{
    std::string result = pattern;

    // 1. {frame_pad}
    replaceAll(result, "{frame_pad}", tmpl.frame_padding);

    // 2. {project_dir} and {file_name} — derived from first type:"file" flag
    for (size_t i = 0; i < tmpl.flags.size(); ++i)
    {
        if (tmpl.flags[i].type == "file")
        {
            std::string filePath = (i < flagValues.size()) ? flagValues[i] : "";
            if (!filePath.empty())
            {
                namespace fs = std::filesystem;
                fs::path p(filePath);
                std::string projectDir = p.parent_path().string();
                std::string fileName = p.stem().string();
                replaceAll(result, "{project_dir}", projectDir);
                replaceAll(result, "{file_name}", fileName);
            }
            else
            {
                replaceAll(result, "{project_dir}", "");
                replaceAll(result, "{file_name}", "");
            }
            break;
        }
    }

    // 3. {flag:id} — scan for flags with matching id
    for (size_t i = 0; i < tmpl.flags.size(); ++i)
    {
        if (!tmpl.flags[i].id.empty())
        {
            std::string token = "{flag:" + tmpl.flags[i].id + "}";
            std::string val = (i < flagValues.size()) ? flagValues[i] : "";
            replaceAll(result, token, val);
        }
    }

    // 4. Date/time tokens
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm tmBuf;
    #ifdef _WIN32
    localtime_s(&tmBuf, &tt);
    #else
    localtime_r(&tt, &tmBuf);
    #endif

    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tmBuf);
    replaceAll(result, "{date:YYYYMMDD}", buf);

    std::strftime(buf, sizeof(buf), "%Y", &tmBuf);
    replaceAll(result, "{date:YYYY}", buf);

    std::strftime(buf, sizeof(buf), "%m", &tmBuf);
    replaceAll(result, "{date:MM}", buf);

    std::strftime(buf, sizeof(buf), "%d", &tmBuf);
    replaceAll(result, "{date:DD}", buf);

    std::strftime(buf, sizeof(buf), "%H%M", &tmBuf);
    replaceAll(result, "{time:HHmm}", buf);

    std::strftime(buf, sizeof(buf), "%H", &tmBuf);
    replaceAll(result, "{time:HH}", buf);

    std::strftime(buf, sizeof(buf), "%M", &tmBuf);
    replaceAll(result, "{time:mm}", buf);

    // 5. Cleanup pass — remove separator artifacts from empty references
    replaceAll(result, "-/", "/");
    replaceAll(result, "-\\", "\\");
    replaceAll(result, "-_", "_");
    replaceAll(result, "_-", "_");
    replaceAll(result, "--", "-");

    return result;
}

// ─── Validation ──────────────────────────────────────────────────────────────

bool TemplateManager::validateTemplate(JobTemplate& tmpl)
{
    tmpl.valid = true;
    tmpl.validation_error.clear();

    if (tmpl.template_id.empty())
    {
        tmpl.valid = false;
        tmpl.validation_error = "Missing template_id";
        return false;
    }
    if (tmpl.name.empty())
    {
        tmpl.valid = false;
        tmpl.validation_error = "Missing name";
        return false;
    }

    // Must have at least one OS cmd path
    if (tmpl.cmd.os_windows.empty() && tmpl.cmd.os_linux.empty() && tmpl.cmd.os_macos.empty())
    {
        tmpl.valid = false;
        tmpl.validation_error = "No executable path for any OS";
        return false;
    }

    return true;
}

std::vector<std::string> TemplateManager::validateSubmission(
    const JobTemplate& tmpl, const std::vector<std::string>& flagValues,
    const std::string& cmdPath, const std::string& jobName,
    int frameStart, int frameEnd, int chunkSize,
    const std::filesystem::path& jobsDir)
{
    std::vector<std::string> errors;

    if (cmdPath.empty())
        errors.push_back("Executable path is empty");

    if (jobName.empty())
        errors.push_back("Job name is empty");
    else
    {
        auto slug = generateSlug(jobName, jobsDir);
        if (slug.empty())
            errors.push_back("Job name produces an empty slug");
    }

    if (frameStart > frameEnd)
        errors.push_back("Frame start must be <= frame end");

    if (chunkSize < 1)
        errors.push_back("Chunk size must be >= 1");

    // Check required editable flags
    for (size_t i = 0; i < tmpl.flags.size(); ++i)
    {
        const auto& f = tmpl.flags[i];
        if (f.editable && f.required && i < flagValues.size())
        {
            if (flagValues[i].empty())
                errors.push_back("Required field is empty: " + f.info);
        }
    }

    return errors;
}

std::string TemplateManager::generateSlug(const std::string& jobName,
                                          const std::filesystem::path& jobsDir)
{
    // Lowercase
    std::string slug;
    slug.reserve(jobName.size());
    for (char ch : jobName)
    {
        char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (std::isalnum(static_cast<unsigned char>(lower)) || lower == '_')
            slug += lower;
        else
            slug += '-';
    }

    // Collapse consecutive dashes
    std::string collapsed;
    collapsed.reserve(slug.size());
    bool lastDash = false;
    for (char ch : slug)
    {
        if (ch == '-')
        {
            if (!lastDash)
                collapsed += ch;
            lastDash = true;
        }
        else
        {
            collapsed += ch;
            lastDash = false;
        }
    }
    slug = collapsed;

    // Trim leading/trailing dashes
    while (!slug.empty() && slug.front() == '-')
        slug.erase(slug.begin());
    while (!slug.empty() && slug.back() == '-')
        slug.pop_back();

    // Truncate to 64 chars
    if (slug.size() > 64)
        slug.resize(64);

    if (slug.empty())
        return {};

    // Dedup check
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(jobsDir / slug, ec))
        return slug;

    for (int i = 2; i <= 99; ++i)
    {
        std::string candidate = slug + "-" + std::to_string(i);
        if (!fs::exists(jobsDir / candidate, ec))
            return candidate;
    }

    // Extremely unlikely — give up
    return {};
}

JobManifest TemplateManager::bakeManifest(
    const JobTemplate& tmpl,
    const std::vector<std::string>& flagValues,
    const std::string& cmdForMyOS,
    const std::string& jobSlug,
    int frameStart, int frameEnd, int chunkSize,
    int maxRetries, std::optional<int> timeout,
    const std::string& nodeId, const std::string& os) const
{
    return bakeManifestStatic(tmpl, flagValues, cmdForMyOS, jobSlug,
                              frameStart, frameEnd, chunkSize,
                              maxRetries, timeout, nodeId, os);
}

JobManifest TemplateManager::bakeManifestStatic(
    const JobTemplate& tmpl,
    const std::vector<std::string>& flagValues,
    const std::string& cmdForMyOS,
    const std::string& jobSlug,
    int frameStart, int frameEnd, int chunkSize,
    int maxRetries, std::optional<int> timeout,
    const std::string& nodeId, const std::string& os)
{
    JobManifest m;
    m.job_id = jobSlug;
    m.template_id = tmpl.template_id;
    m.submitted_by = nodeId;
    m.submitted_os = os;

    auto now = std::chrono::system_clock::now();
    m.submitted_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    // Copy all OS paths from template, overwrite current OS with user-edited value
    if (!tmpl.cmd.os_windows.empty())
        m.cmd["windows"] = tmpl.cmd.os_windows;
    if (!tmpl.cmd.os_linux.empty())
        m.cmd["linux"] = tmpl.cmd.os_linux;
    if (!tmpl.cmd.os_macos.empty())
        m.cmd["macos"] = tmpl.cmd.os_macos;
    m.cmd[os] = cmdForMyOS;

    // Flags: for each template flag, emit manifest flag with user-edited value for editable flags
    // Skip empty non-required positional flags and their preceding standalone flag
    for (size_t i = 0; i < tmpl.flags.size(); ++i)
    {
        const auto& tf = tmpl.flags[i];

        // Check if this is a standalone flag whose next positional value is empty+non-required
        if (!tf.flag.empty() && !tf.value.has_value() && !tf.editable
            && (i + 1) < tmpl.flags.size())
        {
            const auto& nextTf = tmpl.flags[i + 1];
            if (nextTf.flag.empty() && nextTf.editable && !nextTf.required)
            {
                std::string nextVal = ((i + 1) < flagValues.size()) ? flagValues[i + 1] : "";
                if (nextVal.empty())
                {
                    ++i; // skip both this standalone flag and the next empty positional
                    continue;
                }
            }
        }

        // Check if this is an empty non-required positional value (standalone case without preceding flag)
        if (tf.flag.empty() && tf.editable && !tf.required)
        {
            std::string val = (i < flagValues.size()) ? flagValues[i] : "";
            if (val.empty())
                continue;
        }

        ManifestFlag mf;
        mf.flag = tf.flag;

        if (tf.editable && i < flagValues.size())
            mf.value = flagValues[i];
        else
            mf.value = tf.value;

        m.flags.push_back(std::move(mf));
    }

    // Extract output directory from the first output-type flag
    for (size_t i = 0; i < tmpl.flags.size(); ++i)
    {
        if (tmpl.flags[i].type == "output" && i < flagValues.size() && !flagValues[i].empty())
        {
            auto parentDir = std::filesystem::path(flagValues[i]).parent_path().string();
            if (!parentDir.empty())
                m.output_dir = parentDir;
            break;
        }
    }

    m.frame_start = frameStart;
    m.frame_end = frameEnd;
    m.chunk_size = chunkSize;
    m.max_retries = maxRetries;
    m.timeout_seconds = timeout;

    // Copy verbatim from template
    m.progress = tmpl.progress;
    m.output_detection = tmpl.output_detection;
    m.process = tmpl.process;
    m.environment = tmpl.environment;
    m.tags_required = tmpl.tags_required;

    return m;
}

std::string TemplateManager::buildCommandPreview(
    const JobTemplate& tmpl,
    const std::vector<std::string>& flagValues,
    const std::string& cmdPath) const
{
    std::string preview;

    // Quote if contains spaces
    auto maybeQuote = [](const std::string& s) -> std::string {
        if (s.find(' ') != std::string::npos)
            return "\"" + s + "\"";
        return s;
    };

    preview = maybeQuote(cmdPath);

    for (size_t i = 0; i < tmpl.flags.size(); ++i)
    {
        const auto& f = tmpl.flags[i];

        // Skip empty non-required positional flags and their preceding standalone flag
        if (!f.flag.empty() && !f.value.has_value() && !f.editable
            && (i + 1) < tmpl.flags.size())
        {
            const auto& nextF = tmpl.flags[i + 1];
            if (nextF.flag.empty() && nextF.editable && !nextF.required)
            {
                std::string nextVal = ((i + 1) < flagValues.size()) ? flagValues[i + 1] : "";
                if (nextVal.empty())
                {
                    ++i;
                    continue;
                }
            }
        }

        if (f.flag.empty() && f.editable && !f.required)
        {
            std::string val = (i < flagValues.size()) ? flagValues[i] : "";
            if (val.empty())
                continue;
        }

        // Emit flag name if non-empty
        if (!f.flag.empty())
        {
            preview += " ";
            preview += f.flag;
        }

        // Emit value if present
        if (f.value.has_value())
        {
            std::string displayed = (f.editable && i < flagValues.size()) ? flagValues[i] : f.value.value();
            if (!displayed.empty())
            {
                preview += " ";
                preview += maybeQuote(displayed);
            }
            else
            {
                preview += " <empty>";
            }
        }
    }

    return preview;
}

} // namespace SR
