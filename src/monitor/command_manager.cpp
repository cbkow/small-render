#include "monitor/command_manager.h"
#include "core/atomic_file_io.h"
#include "core/platform.h"
#include "core/udp_notify.h"

#include <algorithm>
#include <chrono>
#include "core/monitor_log.h"

namespace SR {

namespace fs = std::filesystem;

CommandManager::~CommandManager()
{
    if (m_running.load())
        stop();
}

void CommandManager::start(const fs::path& farmPath, const std::string& nodeId)
{
    if (m_running.load())
        return;

    m_farmPath = farmPath;
    m_nodeId = nodeId;

    // Ensure inbox directory exists
    std::error_code ec;
    fs::create_directories(farmPath / "commands" / nodeId / "processed", ec);

    m_running.store(true);
    m_thread = std::thread(&CommandManager::threadFunc, this);

    MonitorLog::instance().info("command", "Started for node " + nodeId);
}

void CommandManager::stop()
{
    if (!m_running.load())
        return;

    m_running.store(false);
    if (m_thread.joinable())
        m_thread.join();

    MonitorLog::instance().info("command", "Stopped");
}

void CommandManager::sendCommand(const std::string& targetNodeId,
                                  const std::string& type,
                                  const std::string& jobId,
                                  const std::string& reason,
                                  int frameStart,
                                  int frameEnd)
{
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    nlohmann::json j = {
        {"_version", 1},
        {"from", m_nodeId},
        {"timestamp_ms", now},
        {"type", type},
        {"job_id", jobId},
        {"reason", reason},
    };

    if (frameStart != 0 || frameEnd != 0)
    {
        j["frame_start"] = frameStart;
        j["frame_end"] = frameEnd;
    }

    auto targetDir = m_farmPath / "commands" / targetNodeId;
    std::error_code ec;
    fs::create_directories(targetDir, ec);

    std::string msgId = std::to_string(now) + "." + m_nodeId;
    j["msg_id"] = msgId;
    j["target"] = targetNodeId;

    std::string filename = msgId + ".json";
    AtomicFileIO::writeJson(targetDir / filename, j);

    if (m_udpNotify)
        m_udpNotify->send(j);

    std::string msg = "Sent " + type + " to " + targetNodeId;
    if (!jobId.empty())
        msg += " job=" + jobId;
    MonitorLog::instance().info("command", msg);
}

std::vector<CommandManager::Action> CommandManager::popActions()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Action> result;
    while (!m_actionQueue.empty())
    {
        result.push_back(std::move(m_actionQueue.front()));
        m_actionQueue.pop();
    }
    return result;
}

// ─── Background thread ─────────────────────────────────────────────────────

void CommandManager::threadFunc()
{
    using clock = std::chrono::steady_clock;

    auto lastPoll = clock::time_point{};
    auto lastPurge = clock::now();

    while (m_running.load())
    {
        try
        {
            auto now = clock::now();

            // Poll inbox every 3 seconds
            auto pollElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPoll).count();
            if (pollElapsed >= 3000)
            {
                pollInbox();
                lastPoll = clock::now();
            }

            // Purge processed every 60 seconds
            auto purgeElapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastPurge).count();
            if (purgeElapsed >= 60)
            {
                purgeProcessed();
                lastPurge = clock::now();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        catch (const std::exception& e)
        {
            MonitorLog::instance().error("command", std::string("Thread exception: ") + e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
        catch (...)
        {
            MonitorLog::instance().error("command", "Thread unknown exception");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    }
}

void CommandManager::pollInbox()
{
    std::error_code ec;
    auto inboxDir = m_farmPath / "commands" / m_nodeId;
    if (!fs::is_directory(inboxDir, ec))
        return;

    // Collect command files (not in processed/)
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(inboxDir, ec))
    {
        if (!entry.is_regular_file(ec))
            continue;
        if (entry.path().extension() != ".json")
            continue;
        files.push_back(entry.path());
    }

    // Sort by filename (timestamp order)
    std::sort(files.begin(), files.end());

    for (const auto& file : files)
    {
        auto data = AtomicFileIO::safeReadJson(file);
        if (!data.has_value())
            continue;

        try
        {
            const auto& j = data.value();
            Action action;
            action.type = j.value("type", "");
            action.jobId = j.value("job_id", "");
            action.reason = j.value("reason", "");
            action.frameStart = j.value("frame_start", 0);
            action.frameEnd = j.value("frame_end", 0);
            action.fromNodeId = j.value("from", "");
            action.msgId = j.value("msg_id", "");
            if (action.msgId.empty())
                action.msgId = file.stem().string(); // fallback for old files

            if (!action.type.empty())
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_actionQueue.push(std::move(action));
            }

            // Move to processed
            auto processedDir = inboxDir / "processed";
            fs::rename(file, processedDir / file.filename(), ec);
            if (ec)
            {
                // If rename fails, try to delete to prevent re-processing
                fs::remove(file, ec);
            }
        }
        catch (const std::exception& e)
        {
            MonitorLog::instance().error("command", "Failed to parse command: " + file.string() + " - " + std::string(e.what()));
            // Move bad file to processed to avoid loop
            auto processedDir = inboxDir / "processed";
            fs::rename(file, processedDir / file.filename(), ec);
        }
    }
}

void CommandManager::purgeProcessed()
{
    std::error_code ec;
    auto processedDir = m_farmPath / "commands" / m_nodeId / "processed";
    if (!fs::is_directory(processedDir, ec))
        return;

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    constexpr int64_t PURGE_AGE_MS = 24 * 60 * 60 * 1000; // 24 hours

    for (const auto& entry : fs::directory_iterator(processedDir, ec))
    {
        if (!entry.is_regular_file(ec))
            continue;
        if (entry.path().extension() != ".json")
            continue;

        // Parse timestamp from filename: "1234567890.nodeId.json"
        std::string stem = entry.path().stem().string(); // "1234567890.nodeId"
        auto dotPos = stem.find('.');
        if (dotPos == std::string::npos)
            continue;

        try
        {
            int64_t ts = std::stoll(stem.substr(0, dotPos));
            if (now - ts > PURGE_AGE_MS)
            {
                fs::remove(entry.path(), ec);
            }
        }
        catch (...) {}
    }
}

} // namespace SR
