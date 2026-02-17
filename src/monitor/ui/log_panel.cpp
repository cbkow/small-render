#include "monitor/ui/log_panel.h"
#include "monitor/ui/style.h"
#include "monitor/monitor_app.h"
#include "monitor/ui_data_cache.h"
#include "core/monitor_log.h"
#include "core/platform.h"

#include <imgui.h>
#include <algorithm>
#include <filesystem>

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
            }
            if (ImGui::Selectable("All Nodes", m_mode == Mode::MonitorLog && m_filterIdx == 1))
            {
                m_mode = Mode::MonitorLog;
                m_filterIdx = 1;
            }

            for (int i = 0; i < (int)m_peerHostnames.size(); ++i)
            {
                bool selected = (m_mode == Mode::MonitorLog && m_filterIdx == i + 2);
                if (ImGui::Selectable(m_peerHostnames[i].c_str(), selected))
                {
                    m_mode = Mode::MonitorLog;
                    m_filterIdx = i + 2;
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

                // Read + show peer log files (from UIDataCache bg thread)
                if (m_app->isFarmRunning())
                {
                    // Push log request to UIDataCache
                    m_app->uiDataCache().setLogRequest("all", m_peerNodeIds);
                    auto remoteSnap = m_app->uiDataCache().getRemoteLogSnapshot();

                    ImGui::Separator();
                    for (const auto& line : remoteSnap.lines)
                    {
                        ImVec4 color(0.5f, 0.6f, 0.7f, 1.0f);
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
                    // Push log request to UIDataCache
                    m_app->uiDataCache().setLogRequest(
                        "peer:" + m_peerNodeIds[peerIdx],
                        {m_peerNodeIds[peerIdx]});
                    auto remoteSnap = m_app->uiDataCache().getRemoteLogSnapshot();

                    for (const auto& line : remoteSnap.lines)
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

// ─── Task output (DCC stdout, from UIDataCache bg thread) ────────────────────

void LogPanel::renderTaskOutput()
{
    std::string jobId = m_app->selectedJobId();
    if (jobId.empty())
    {
        ImGui::TextDisabled("No job selected");
        return;
    }

    auto snap = m_app->uiDataCache().getTaskOutputSnapshot();

    if (snap.lines.empty())
    {
        ImGui::TextDisabled("No task output available");
        return;
    }

    for (const auto& tol : snap.lines)
    {
        if (tol.isHeader)
            ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "%s", tol.text.c_str());
        else
            ImGui::TextUnformatted(tol.text.c_str());
    }
}

} // namespace SR
