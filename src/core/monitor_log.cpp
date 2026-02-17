#include "core/monitor_log.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace SR {

namespace fs = std::filesystem;

MonitorLog& MonitorLog::instance()
{
    static MonitorLog s_instance;
    return s_instance;
}

void MonitorLog::startFileLogging(const fs::path& farmPath, const std::string& nodeId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_farmPath = farmPath;
    m_nodeId = nodeId;
    m_currentDate = currentDateStr();
    m_fileEnabled = true;

    // Ensure our node's directory exists
    std::error_code ec;
    fs::create_directories(farmPath / "nodes" / nodeId, ec);
}

void MonitorLog::stopFileLogging()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_fileEnabled = false;
}

void MonitorLog::info(const std::string& category, const std::string& message)
{
    append("INFO", category, message);
}

void MonitorLog::warn(const std::string& category, const std::string& message)
{
    append("WARN", category, message);
}

void MonitorLog::error(const std::string& category, const std::string& message)
{
    append("ERROR", category, message);
}

void MonitorLog::append(const std::string& level, const std::string& category,
                        const std::string& message)
{
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    // Format time string: HH:MM:SS.mmm
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
    auto remainder = ms - std::chrono::duration_cast<std::chrono::milliseconds>(secs).count();
    time_t t = static_cast<time_t>(secs.count());
    struct tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif

    char timeBuf[16];
    std::snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d.%03d",
                  tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec, (int)remainder);

    std::string fileLine = std::string(timeBuf) + " " + level + "  [" + category + "] " + message;

    std::lock_guard<std::mutex> lock(m_mutex);

    // Add to ring buffer
    Entry entry;
    entry.timestamp_ms = ms;
    entry.level = level;
    entry.category = category;
    entry.message = message;

    if (m_buffer.size() < MAX_ENTRIES)
    {
        m_buffer.push_back(std::move(entry));
    }
    else
    {
        m_buffer[m_writePos] = std::move(entry);
        m_wrapped = true;
    }
    m_writePos = (m_writePos + 1) % MAX_ENTRIES;

    // Write to file
    if (m_fileEnabled)
        writeToFile(fileLine);

    // Also write to stdout in debug builds
#ifndef NDEBUG
    std::cout << fileLine << std::endl;
#endif
}

void MonitorLog::writeToFile(const std::string& line)
{
    // Check for date rollover
    std::string today = currentDateStr();
    if (today != m_currentDate)
    {
        m_currentDate = today;
        purgeOldFiles();
    }

    auto logPath = m_farmPath / "nodes" / m_nodeId /
        ("monitor-" + m_currentDate + ".log");

    std::ofstream ofs(logPath, std::ios::app);
    if (ofs.is_open())
    {
        ofs << line << "\n";
    }
}

void MonitorLog::purgeOldFiles()
{
    auto nodeDir = m_farmPath / "nodes" / m_nodeId;
    std::error_code ec;

    if (!fs::is_directory(nodeDir, ec))
        return;

    // Parse current date to compute cutoff
    auto now = std::chrono::system_clock::now();
    auto cutoff = now - std::chrono::hours(7 * 24); // 7 days
    auto cutoffMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        cutoff.time_since_epoch()).count();

    for (auto& entry : fs::directory_iterator(nodeDir, ec))
    {
        if (!entry.is_regular_file(ec)) continue;
        auto filename = entry.path().filename().string();

        // Match pattern: monitor-YYYY-MM-DD.log
        if (filename.size() == 24 &&
            filename.substr(0, 8) == "monitor-" &&
            filename.substr(18) == ".log")
        {
            std::string dateStr = filename.substr(8, 10); // YYYY-MM-DD

            // Parse date
            struct tm tmDate = {};
#ifdef _WIN32
            if (sscanf_s(dateStr.c_str(), "%d-%d-%d",
                         &tmDate.tm_year, &tmDate.tm_mon, &tmDate.tm_mday) == 3)
#else
            if (std::sscanf(dateStr.c_str(), "%d-%d-%d",
                            &tmDate.tm_year, &tmDate.tm_mon, &tmDate.tm_mday) == 3)
#endif
            {
                tmDate.tm_year -= 1900;
                tmDate.tm_mon -= 1;
                time_t fileTime = std::mktime(&tmDate);
                auto fileMs = static_cast<int64_t>(fileTime) * 1000;

                if (fileMs < cutoffMs)
                {
                    fs::remove(entry.path(), ec);
                }
            }
        }
    }
}

std::string MonitorLog::currentDateStr() const
{
    auto now = std::chrono::system_clock::now();
    time_t t = std::chrono::system_clock::to_time_t(now);
    struct tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmBuf);
    return std::string(buf);
}

void MonitorLog::clearEntries()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_buffer.clear();
    m_writePos = 0;
    m_wrapped = false;
}

std::vector<MonitorLog::Entry> MonitorLog::getEntries() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_wrapped)
    {
        // Buffer hasn't wrapped yet — just return as-is
        return m_buffer;
    }

    // Return in chronological order: [writePos..end, 0..writePos)
    std::vector<Entry> result;
    result.reserve(MAX_ENTRIES);
    for (size_t i = m_writePos; i < m_buffer.size(); ++i)
        result.push_back(m_buffer[i]);
    for (size_t i = 0; i < m_writePos; ++i)
        result.push_back(m_buffer[i]);
    return result;
}

std::vector<std::string> MonitorLog::readNodeLog(
    const fs::path& farmPath, const std::string& nodeId, int maxLines)
{
    std::vector<std::string> result;

    // Get today's date
    auto now = std::chrono::system_clock::now();
    time_t t = std::chrono::system_clock::to_time_t(now);
    struct tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    char dateBuf[16];
    std::strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", &tmBuf);
    std::string today(dateBuf);

    // Also get yesterday's date
    auto yesterday = now - std::chrono::hours(24);
    time_t yt = std::chrono::system_clock::to_time_t(yesterday);
#ifdef _WIN32
    localtime_s(&tmBuf, &yt);
#else
    localtime_r(&yt, &tmBuf);
#endif
    std::strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", &tmBuf);
    std::string yesterdayStr(dateBuf);

    auto nodeDir = farmPath / "nodes" / nodeId;

    // Read yesterday's file first (if today's is short, we'll have more context)
    auto yesterdayPath = nodeDir / ("monitor-" + yesterdayStr + ".log");
    std::error_code ec;
    if (fs::exists(yesterdayPath, ec))
    {
        std::ifstream ifs(yesterdayPath);
        std::string line;
        while (std::getline(ifs, line))
            result.push_back(std::move(line));
    }

    // Read today's file
    auto todayPath = nodeDir / ("monitor-" + today + ".log");
    if (fs::exists(todayPath, ec))
    {
        std::ifstream ifs(todayPath);
        std::string line;
        while (std::getline(ifs, line))
            result.push_back(std::move(line));
    }

    // Trim to last maxLines
    if ((int)result.size() > maxLines)
    {
        result.erase(result.begin(), result.begin() + ((int)result.size() - maxLines));
    }

    return result;
}

} // namespace SR
