#include "monitor/ui/farm_cleanup_dialog.h"
#include "monitor/monitor_app.h"
#include "core/monitor_log.h"

#include <imgui.h>
#include <filesystem>

namespace SR {

namespace fs = std::filesystem;

void FarmCleanupDialog::init(MonitorApp* app)
{
    m_app = app;
}

void FarmCleanupDialog::open()
{
    scanItems();
    m_shouldOpen = true;
}

void FarmCleanupDialog::render()
{
    if (m_shouldOpen)
    {
        ImGui::OpenPopup("Farm Cleanup");
        m_shouldOpen = false;
    }

    // Size to 90% of main viewport, centered (matches Settings panel style)
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 modalSize(viewport->WorkSize.x * 0.9f, viewport->WorkSize.y * 0.9f);
    ImGui::SetNextWindowSize(modalSize, ImGuiCond_Always);
    ImVec2 center(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                  viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.09f, 0.09f, 0.09f, 1.0f));
    if (!ImGui::BeginPopupModal("Farm Cleanup", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
    {
        ImGui::PopStyleColor();
        return;
    }
    ImGui::PopStyleColor();

    // Scrollable content area, leaving room for buttons at the bottom
    float buttonRowHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("CleanupContent", ImVec2(0, -buttonRowHeight), ImGuiChildFlags_None);

    bool hasAnyItems = !m_completedJobs.empty() || !m_deadNodes.empty() || !m_orphanedDirs.empty();

    if (!hasAnyItems)
    {
        ImGui::TextDisabled("Nothing to clean up.");
        ImGui::Spacing();
    }

    // --- Completed/Cancelled Jobs ---
    if (!m_completedJobs.empty())
    {
        ImGui::SeparatorText("Completed / Cancelled Jobs");
        for (auto& item : m_completedJobs)
        {
            ImGui::Checkbox(item.label.c_str(), &item.selected);
            ImGui::SameLine(0, 8);
            ImGui::TextDisabled("%s", item.detail.c_str());
        }
        ImGui::Spacing();
    }

    // --- Dead Nodes ---
    if (!m_deadNodes.empty())
    {
        ImGui::SeparatorText("Dead Nodes");
        ImGui::TextDisabled("Removes heartbeat + command inbox directories.");
        for (auto& item : m_deadNodes)
        {
            ImGui::Checkbox(item.label.c_str(), &item.selected);
            ImGui::SameLine(0, 8);
            ImGui::TextDisabled("%s", item.detail.c_str());
        }
        ImGui::Spacing();
    }

    // --- Orphaned Directories ---
    if (!m_orphanedDirs.empty())
    {
        ImGui::SeparatorText("Orphaned Directories");
        ImGui::TextDisabled("Job directories missing manifest.json.");
        for (auto& item : m_orphanedDirs)
        {
            ImGui::Checkbox(item.label.c_str(), &item.selected);
        }
        ImGui::Spacing();
    }

    ImGui::EndChild(); // CleanupContent

    // --- Buttons (pinned to bottom) ---
    ImGui::Separator();
    if (ImGui::Button("Select All"))
    {
        for (auto& i : m_completedJobs) i.selected = true;
        for (auto& i : m_deadNodes) i.selected = true;
        for (auto& i : m_orphanedDirs) i.selected = true;
    }
    ImGui::SameLine();

    // Count selected
    int selectedCount = 0;
    for (auto& i : m_completedJobs) if (i.selected) selectedCount++;
    for (auto& i : m_deadNodes) if (i.selected) selectedCount++;
    for (auto& i : m_orphanedDirs) if (i.selected) selectedCount++;

    if (selectedCount > 0)
    {
        char btnLabel[64];
        snprintf(btnLabel, sizeof(btnLabel), "Clean Selected (%d)", selectedCount);
        if (ImGui::Button(btnLabel))
        {
            cleanSelected();
            ImGui::CloseCurrentPopup();
        }
    }
    else
    {
        ImGui::BeginDisabled();
        ImGui::Button("Clean Selected");
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

void FarmCleanupDialog::scanItems()
{
    m_completedJobs.clear();
    m_deadNodes.clear();
    m_orphanedDirs.clear();

    if (!m_app || !m_app->isFarmRunning())
        return;

    const auto& farmPath = m_app->farmPath();
    std::error_code ec;

    // --- Completed/Cancelled Jobs ---
    for (const auto& job : m_app->jobManager().jobs())
    {
        if (job.current_state == "completed" || job.current_state == "cancelled")
        {
            CleanupItem item;
            item.id = job.manifest.job_id;
            item.label = job.manifest.job_id;
            item.detail = job.current_state + " | " +
                          std::to_string(job.manifest.frame_end - job.manifest.frame_start + 1) + " frames";
            m_completedJobs.push_back(std::move(item));
        }
    }

    // --- Dead Nodes ---
    auto nodes = m_app->heartbeatManager().getNodeSnapshot();
    for (const auto& n : nodes)
    {
        if (n.isDead && !n.isLocal)
        {
            CleanupItem item;
            item.id = n.heartbeat.node_id;
            item.label = n.heartbeat.hostname + " (" + n.heartbeat.node_id + ")";
            item.detail = "dead";
            m_deadNodes.push_back(std::move(item));
        }
    }

    // --- Orphaned Job Directories ---
    auto jobsDir = farmPath / "jobs";
    if (fs::is_directory(jobsDir, ec))
    {
        for (auto& entry : fs::directory_iterator(jobsDir, ec))
        {
            if (!entry.is_directory(ec)) continue;

            auto manifestPath = entry.path() / "manifest.json";
            if (!fs::exists(manifestPath, ec))
            {
                CleanupItem item;
                item.id = entry.path().string();
                item.label = "jobs/" + entry.path().filename().string();
                item.detail = "no manifest.json";
                m_orphanedDirs.push_back(std::move(item));
            }
        }
    }
}

void FarmCleanupDialog::cleanSelected()
{
    if (!m_app || !m_app->isFarmRunning())
        return;

    const auto& farmPath = m_app->farmPath();
    std::error_code ec;
    int cleaned = 0;

    // --- Clean completed/cancelled jobs ---
    for (const auto& item : m_completedJobs)
    {
        if (!item.selected) continue;
        auto jobDir = farmPath / "jobs" / item.id;
        fs::remove_all(jobDir, ec);
        if (!ec)
        {
            MonitorLog::instance().info("farm", "Cleaned job: " + item.id);
            cleaned++;
        }
        else
        {
            MonitorLog::instance().error("farm", "Failed to clean job " + item.id + ": " + ec.message());
        }
    }

    // --- Clean dead nodes ---
    for (const auto& item : m_deadNodes)
    {
        if (!item.selected) continue;
        fs::remove_all(farmPath / "nodes" / item.id, ec);
        fs::remove_all(farmPath / "commands" / item.id, ec);
        MonitorLog::instance().info("farm", "Cleaned dead node: " + item.id);
        cleaned++;
    }

    // --- Clean orphaned dirs ---
    for (const auto& item : m_orphanedDirs)
    {
        if (!item.selected) continue;
        fs::remove_all(fs::path(item.id), ec);
        if (!ec)
        {
            MonitorLog::instance().info("farm", "Cleaned orphaned dir: " + item.label);
            cleaned++;
        }
    }

    if (cleaned > 0)
    {
        m_app->jobManager().invalidate();
        MonitorLog::instance().info("farm", "Farm cleanup: " + std::to_string(cleaned) + " items removed");
    }
}

} // namespace SR
