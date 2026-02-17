#include "monitor/ui/log_panel.h"
#include "monitor/ui/style.h"
#include "monitor/monitor_app.h"
#include "core/monitor_log.h"
#include "core/platform.h"

#include <imgui.h>
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace SR {

void LogPanel::init(MonitorApp* app)
{
    m_app = app;
}

void LogPanel::render()
{
    if (!visible) return;

    if (ImGui::Begin("Log", nullptr, ImGuiWindowFlags_NoTitleBar))
    {
        panelHeader("Log", visible);
        if (!m_app)
        {
            ImGui::TextDisabled("Not initialized");
            ImGui::End();
            return;
        }

        // Update peer list from heartbeat manager
        if (m_app->isFarmRunning())
        {
            auto nodes = m_app->heartbeatManager().getNodeSnapshot();
            m_peerNodeIds.clear();
            m_peerHostnames.clear();
            for (const auto& n : nodes)
            {
                if (n.isLocal) continue;
                m_peerNodeIds.push_back(n.heartbeat.node_id);
                m_peerHostnames.push_back(
                    n.heartbeat.hostname.empty() ? n.heartbeat.node_id : n.heartbeat.hostname);
            }
        }

        // Fall back to monitor log if job deselected while in task output mode
        if (m_mode == Mode::TaskOutput && m_app->selectedJobId().empty())
        {
            m_mode = Mode::MonitorLog;
            m_filterIdx = 0;
        }

        // Build dropdown label
        std::string currentLabel;
        if (m_mode == Mode::TaskOutput)
        {
            currentLabel = "Task Output: " + m_app->selectedJobId();
        }
        else
        {
            if (m_filterIdx == 0)
                currentLabel = "This Node";
            else if (m_filterIdx == 1)
                currentLabel = "All Nodes";
            else
            {
                int peerIdx = m_filterIdx - 2;
                if (peerIdx < (int)m_peerHostnames.size())
                    currentLabel = m_peerHostnames[peerIdx];
                else
                {
                    m_filterIdx = 0;
                    currentLabel = "This Node";
                }
            }
        }

        // Filter dropdown
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::BeginCombo("##logfilter", currentLabel.c_str()))
        {
            if (ImGui::Selectable("This Node", m_mode == Mode::MonitorLog && m_filterIdx == 0))
            {
                m_mode = Mode::MonitorLog;
                m_filterIdx = 0;
                m_remoteCacheKey.clear();
            }
            if (ImGui::Selectable("All Nodes", m_mode == Mode::MonitorLog && m_filterIdx == 1))
            {
                m_mode = Mode::MonitorLog;
                m_filterIdx = 1;
                m_remoteCacheKey.clear();
            }

            for (int i = 0; i < (int)m_peerHostnames.size(); ++i)
            {
                bool selected = (m_mode == Mode::MonitorLog && m_filterIdx == i + 2);
                if (ImGui::Selectable(m_peerHostnames[i].c_str(), selected))
                {
                    m_mode = Mode::MonitorLog;
                    m_filterIdx = i + 2;
                    m_remoteCacheKey.clear();
                }
            }

            // Task output (only when a job is selected)
            if (!m_app->selectedJobId().empty())
            {
                ImGui::Separator();
                std::string taskLabel = "Task Output: " + m_app->selectedJobId();
                if (ImGui::Selectable(taskLabel.c_str(), m_mode == Mode::TaskOutput))
                {
                    m_mode = Mode::TaskOutput;
                    m_taskOutputJobId.clear(); // force reload
                }
            }

            ImGui::EndCombo();
        }

        ImGui::Separator();

        // Log content area
        float footerHeight = ImGui::GetFrameHeightWithSpacing();
        if (ImGui::BeginChild("##log_scroll", ImVec2(0, -footerHeight), ImGuiChildFlags_None))
        {
            if (Fonts::mono) ImGui::PushFont(Fonts::mono);

            if (m_mode == Mode::TaskOutput)
            {
                renderTaskOutput();
            }
            else if (m_filterIdx == 0)
            {
                // This Node — local in-memory buffer
                auto entries = MonitorLog::instance().getEntries();
                for (const auto& e : entries)
                {
                    ImVec4 color(0.7f, 0.7f, 0.7f, 1.0f); // default gray
                    if (e.level == "INFO")
                        color = ImVec4(0.7f, 0.9f, 0.7f, 1.0f);
                    else if (e.level == "WARN")
                        color = ImVec4(1.0f, 0.85f, 0.0f, 1.0f);
                    else if (e.level == "ERROR")
                        color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);

                    // Format: HH:MM:SS LEVEL [cat] message
                    // Extract time from timestamp
                    time_t secs = static_cast<time_t>(e.timestamp_ms / 1000);
                    struct tm tmBuf;
#ifdef _WIN32
                    localtime_s(&tmBuf, &secs);
#else
                    localtime_r(&secs, &tmBuf);
#endif
                    char timeBuf[16];
                    std::snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d",
                                  tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);

                    std::string line = std::string(timeBuf) + " " +
                        e.level + "  [" + e.category + "] " + e.message;

                    ImGui::TextColored(color, "%s", line.c_str());
                }
            }
            else if (m_filterIdx == 1)
            {
                // All Nodes — local + read peers' files
                auto entries = MonitorLog::instance().getEntries();

                // Show local entries
                for (const auto& e : entries)
                {
                    ImVec4 color(0.7f, 0.9f, 0.7f, 1.0f);
                    if (e.level == "WARN") color = ImVec4(1.0f, 0.85f, 0.0f, 1.0f);
                    else if (e.level == "ERROR") color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);

                    time_t secs = static_cast<time_t>(e.timestamp_ms / 1000);
                    struct tm tmBuf;
#ifdef _WIN32
                    localtime_s(&tmBuf, &secs);
#else
                    localtime_r(&secs, &tmBuf);
#endif
                    char timeBuf[16];
                    std::snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d",
                                  tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);

                    ImGui::TextColored(color, "[local] %s %s  [%s] %s",
                                       timeBuf, e.level.c_str(),
                                       e.category.c_str(), e.message.c_str());
                }

                // Read + show peer log files (with cache)
                if (m_app->isFarmRunning())
                {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - m_lastRemoteLoad).count();

                    if (m_remoteCacheKey != "all" || elapsed >= REMOTE_RELOAD_MS)
                    {
                        m_remoteLines.clear();
                        m_remoteCacheKey = "all";
                        m_lastRemoteLoad = now;

                        for (int i = 0; i < (int)m_peerNodeIds.size(); ++i)
                        {
                            auto lines = MonitorLog::readNodeLog(
                                m_app->farmPath(), m_peerNodeIds[i], 200);
                            for (auto& l : lines)
                            {
                                m_remoteLines.push_back(
                                    "[" + m_peerHostnames[i] + "] " + l);
                            }
                        }
                    }

                    ImGui::Separator();
                    for (const auto& line : m_remoteLines)
                    {
                        // Color remote lines by level if parseable
                        ImVec4 color(0.5f, 0.6f, 0.7f, 1.0f); // muted blue-gray
                        if (line.find("WARN") != std::string::npos)
                            color = ImVec4(0.8f, 0.7f, 0.0f, 1.0f);
                        else if (line.find("ERROR") != std::string::npos)
                            color = ImVec4(0.8f, 0.3f, 0.3f, 1.0f);

                        ImGui::TextColored(color, "%s", line.c_str());
                    }
                }
            }
            else
            {
                // Specific peer
                int peerIdx = m_filterIdx - 2;
                if (peerIdx >= 0 && peerIdx < (int)m_peerNodeIds.size() && m_app->isFarmRunning())
                {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - m_lastRemoteLoad).count();

                    std::string cacheKey = "peer:" + m_peerNodeIds[peerIdx];
                    if (m_remoteCacheKey != cacheKey || elapsed >= REMOTE_RELOAD_MS)
                    {
                        m_remoteCacheKey = cacheKey;
                        m_lastRemoteLoad = now;
                        m_remoteLines = MonitorLog::readNodeLog(
                            m_app->farmPath(), m_peerNodeIds[peerIdx], 500);
                    }

                    for (const auto& line : m_remoteLines)
                    {
                        ImVec4 color(0.7f, 0.7f, 0.7f, 1.0f);
                        if (line.find("WARN") != std::string::npos)
                            color = ImVec4(1.0f, 0.85f, 0.0f, 1.0f);
                        else if (line.find("ERROR") != std::string::npos)
                            color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                        else if (line.find("INFO") != std::string::npos)
                            color = ImVec4(0.7f, 0.9f, 0.7f, 1.0f);

                        ImGui::TextColored(color, "%s", line.c_str());
                    }
                }
                else
                {
                    ImGui::TextDisabled("Peer not available");
                }
            }

            // Auto-scroll: pin to bottom when enabled
            if (m_autoScroll)
                ImGui::SetScrollHereY(1.0f);

            if (Fonts::mono) ImGui::PopFont();
        }
        ImGui::EndChild();

        // Footer
        if (ImGui::Button("Clear"))
        {
            MonitorLog::instance().clearEntries();
            m_remoteLines.clear();
            m_remoteCacheKey.clear();
            m_taskOutputLines.clear();
            m_taskOutputJobId.clear();
        }
        ImGui::SameLine();
        if (m_app->isFarmRunning())
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            fs::path logFolder;

            if (m_mode == Mode::TaskOutput && !m_app->selectedJobId().empty())
            {
                logFolder = m_app->farmPath() / "jobs" / m_app->selectedJobId() / "stdout";
            }
            else if (m_filterIdx == 0)
            {
                logFolder = m_app->farmPath() / "nodes" / m_app->identity().nodeId();
            }
            else if (m_filterIdx == 1)
            {
                logFolder = m_app->farmPath() / "nodes";
            }
            else
            {
                int peerIdx = m_filterIdx - 2;
                if (peerIdx >= 0 && peerIdx < (int)m_peerNodeIds.size())
                    logFolder = m_app->farmPath() / "nodes" / m_peerNodeIds[peerIdx];
            }

            if (!logFolder.empty() && fs::is_directory(logFolder, ec))
            {
                if (ImGui::Button("Open Folder"))
                    openFolderInExplorer(logFolder);
                ImGui::SameLine();
            }
        }
        ImGui::Checkbox("Auto-scroll", &m_autoScroll);
    }
    ImGui::End();
}

// ─── Task output (DCC stdout) ────────────────────────────────────────────────

void LogPanel::renderTaskOutput()
{
    std::string jobId = m_app->selectedJobId();
    if (jobId.empty())
    {
        ImGui::TextDisabled("No job selected");
        return;
    }

    // Reload on job change or cooldown expiry
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastTaskOutputLoad).count();
    bool needsReload = (jobId != m_taskOutputJobId) || (elapsed >= TASK_OUTPUT_RELOAD_MS);

    if (needsReload)
    {
        m_taskOutputJobId = jobId;
        m_lastTaskOutputLoad = now;
        m_taskOutputLines.clear();

        namespace fs = std::filesystem;
        std::error_code ec;
        auto stdoutDir = m_app->farmPath() / "jobs" / jobId / "stdout";

        struct LogFile
        {
            std::string nodeId;
            std::string filename;
            std::string rangeStr;
            uint64_t timestamp_ms = 0;
            fs::path path;
        };
        std::vector<LogFile> logFiles;

        if (fs::is_directory(stdoutDir, ec))
        {
            for (auto& nodeEntry : fs::directory_iterator(stdoutDir, ec))
            {
                if (!nodeEntry.is_directory(ec)) continue;
                std::string nodeId = nodeEntry.path().filename().string();

                std::error_code ec2;
                for (auto& fileEntry : fs::directory_iterator(nodeEntry.path(), ec2))
                {
                    if (!fileEntry.is_regular_file(ec2)) continue;
                    if (fileEntry.path().extension() != ".log") continue;

                    std::string fname = fileEntry.path().filename().string();

                    // Parse: "{rangeStr}_{timestamp_ms}.log"
                    auto lastUnderscore = fname.rfind('_');
                    auto dotPos = fname.rfind('.');
                    if (lastUnderscore == std::string::npos || dotPos == std::string::npos)
                        continue;

                    std::string rangeStr = fname.substr(0, lastUnderscore);
                    std::string tsStr = fname.substr(lastUnderscore + 1,
                                                     dotPos - lastUnderscore - 1);

                    uint64_t ts = 0;
                    try { ts = std::stoull(tsStr); }
                    catch (...) { continue; }

                    logFiles.push_back({nodeId, fname, rangeStr, ts, fileEntry.path()});
                }
            }
        }

        // Sort by range then timestamp (groups chunks together, retries in order)
        std::sort(logFiles.begin(), logFiles.end(),
                  [](const LogFile& a, const LogFile& b) {
                      if (a.rangeStr != b.rangeStr) return a.rangeStr < b.rangeStr;
                      return a.timestamp_ms < b.timestamp_ms;
                  });

        // Load file contents with headers
        for (const auto& lf : logFiles)
        {
            // Format timestamp as HH:MM:SS
            time_t secs = static_cast<time_t>(lf.timestamp_ms / 1000);
            struct tm tmBuf;
#ifdef _WIN32
            localtime_s(&tmBuf, &secs);
#else
            localtime_r(&secs, &tmBuf);
#endif
            char timeBuf[16];
            std::snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d",
                          tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);

            std::string header = lf.nodeId + "  |  f" + lf.rangeStr + "  |  " + timeBuf;
            m_taskOutputLines.push_back({header, true});

            std::ifstream ifs(lf.path);
            std::string line;
            while (std::getline(ifs, line))
                m_taskOutputLines.push_back({std::move(line), false});

            m_taskOutputLines.push_back({"", false}); // blank separator
        }
    }

    // Display
    if (m_taskOutputLines.empty())
    {
        ImGui::TextDisabled("No task output available");
        return;
    }

    for (const auto& tol : m_taskOutputLines)
    {
        if (tol.isHeader)
            ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "%s", tol.text.c_str());
        else
            ImGui::TextUnformatted(tol.text.c_str());
    }
}

} // namespace SR
