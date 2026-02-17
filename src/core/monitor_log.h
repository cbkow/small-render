#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <filesystem>
#include <cstdint>

namespace SR {

class MonitorLog
{
public:
    static MonitorLog& instance();

    // Call from MonitorApp::startFarm / stopFarm
    void startFileLogging(const std::filesystem::path& farmPath,
                          const std::string& nodeId);
    void stopFileLogging();

    // Logging methods (thread-safe)
    void info(const std::string& category, const std::string& message);
    void warn(const std::string& category, const std::string& message);
    void error(const std::string& category, const std::string& message);

    // UI access — returns copy of ring buffer (thread-safe)
    struct Entry
    {
        int64_t timestamp_ms = 0;
        std::string level;      // "INFO", "WARN", "ERROR"
        std::string category;   // "claims", "render", "agent", "command", "health", "job", "farm"
        std::string message;
    };
    std::vector<Entry> getEntries() const;
    void clearEntries();

    // Read another node's log file (for remote troubleshooting)
    static std::vector<std::string> readNodeLog(
        const std::filesystem::path& farmPath,
        const std::string& nodeId,
        int maxLines = 500);

private:
    MonitorLog() = default;
    void append(const std::string& level, const std::string& category,
                const std::string& message);
    void writeToFile(const std::string& line);
    void purgeOldFiles();
    std::string currentDateStr() const;

    // Ring buffer
    static constexpr size_t MAX_ENTRIES = 1000;
    std::vector<Entry> m_buffer;
    size_t m_writePos = 0;
    bool m_wrapped = false;

    // File logging
    std::filesystem::path m_farmPath;
    std::string m_nodeId;
    bool m_fileEnabled = false;
    std::string m_currentDate;

    mutable std::mutex m_mutex;
};

} // namespace SR
