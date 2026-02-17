#include "monitor/ui/job_list_panel.h"
#include "monitor/ui/style.h"
#include "monitor/monitor_app.h"
#include "core/atomic_file_io.h"

#include <imgui.h>
#include <ctime>
#include <cstring>
#include <filesystem>

namespace SR {

void JobListPanel::init(MonitorApp* app)
{
    m_app = app;
}

void JobListPanel::scanJobProgress()
{
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastProgressScan).count();
    if (elapsed < PROGRESS_SCAN_COOLDOWN_MS)
        return;

    m_lastProgressScan = now;

    const auto& jobs = m_app->jobManager().jobs();
    for (const auto& job : jobs)
    {
        const auto& manifest = job.manifest;
        int totalFrames = manifest.frame_end - manifest.frame_start + 1;

        // Read dispatch.json — skip on failed read to keep last good value
        auto dispatchPath = m_app->farmPath() / "jobs" / manifest.job_id / "dispatch.json";
        auto data = AtomicFileIO::safeReadJson(dispatchPath);
        if (!data.has_value())
            continue;

        JobProgress prog;
        prog.total = totalFrames;
        prog.completed = 0;

        try
        {
            DispatchTable dt = data.value().get<DispatchTable>();
            for (const auto& dc : dt.chunks)
            {
                if (dc.state == "completed")
                    prog.completed += (dc.frame_end - dc.frame_start + 1);
            }
        }
        catch (...) {}

        m_progressCache[manifest.job_id] = prog;
    }
}

static bool isDeletableState(const std::string& state)
{
    return state == "completed" || state == "cancelled" || state == "failed";
}

void JobListPanel::render()
{
    if (!visible) return;

    if (ImGui::Begin("Job List", nullptr, ImGuiWindowFlags_NoTitleBar))
    {
        panelHeader("Jobs", visible);
        if (!m_app || !m_app->isFarmRunning())
        {
            ImGui::TextDisabled("Farm not connected");
            ImGui::End();
            return;
        }

        // Scan progress
        scanJobProgress();

        // New Job button
        if (ImGui::Button("New Job"))
            m_app->requestSubmissionMode();

        // Toolbar: bulk delete button
        const auto& jobs = m_app->jobManager().jobs();
        int deletableCount = 0;
        for (const auto& id : m_selectedJobIds)
        {
            for (const auto& j : jobs)
            {
                if (j.manifest.job_id == id && isDeletableState(j.current_state))
                {
                    ++deletableCount;
                    break;
                }
            }
        }
        if (deletableCount > 0)
        {
            ImGui::SameLine();
            char delLabel[32];
            snprintf(delLabel, sizeof(delLabel), "Delete (%d)", deletableCount);
            if (ImGui::Button(delLabel))
                m_pendingBulkDelete = true;
        }

        ImGui::SameLine();
        ImGui::TextDisabled("(%d jobs)", (int)jobs.size());

        ImGui::Separator();

        // Job table
        if (jobs.empty())
        {
            ImGui::TextDisabled("No jobs submitted yet.");
        }
        else
        {
            ImGuiTableFlags tableFlags =
                ImGuiTableFlags_Resizable |
                ImGuiTableFlags_RowBg |
                ImGuiTableFlags_BordersOuter |
                ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY;

            if (ImGui::BeginTable("##JobTable", 7, tableFlags))
            {
                ImGui::TableSetupColumn("Name",      ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn("Template",   ImGuiTableColumnFlags_WidthStretch, 1.5f);
                ImGui::TableSetupColumn("State",      ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn("Progress",   ImGuiTableColumnFlags_WidthStretch, 3.0f);
                ImGui::TableSetupColumn("Priority",   ImGuiTableColumnFlags_WidthFixed, 55.0f);
                ImGui::TableSetupColumn("Frames",     ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Submitted",  ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                const ImU32 highlightColor = ImGui::GetColorU32(ImVec4(0.3f, 0.5f, 0.8f, 0.35f));

                for (int i = 0; i < (int)jobs.size(); ++i)
                {
                    const auto& job = jobs[i];
                    const auto& jobId = job.manifest.job_id;
                    bool inMultiSelect = m_selectedJobIds.count(jobId) > 0;

                    ImGui::PushID(i);
                    ImGui::TableNextRow();

                    // Row highlight for multi-selected rows
                    if (inMultiSelect)
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, highlightColor);

                    // Name (selectable full row)
                    ImGui::TableNextColumn();
                    bool isDetailSelected = (m_app->selectedJobId() == jobId);
                    if (ImGui::Selectable(jobId.c_str(), isDetailSelected || inMultiSelect,
                                          ImGuiSelectableFlags_SpanAllColumns))
                    {
                        bool ctrl = ImGui::GetIO().KeyCtrl;
                        bool shift = ImGui::GetIO().KeyShift;

                        if (shift && m_lastClickedIndex >= 0 && m_lastClickedIndex < (int)jobs.size())
                        {
                            // Shift+click: select range
                            int lo = (std::min)(m_lastClickedIndex, i);
                            int hi = (std::max)(m_lastClickedIndex, i);
                            if (!ctrl) m_selectedJobIds.clear();
                            for (int r = lo; r <= hi; ++r)
                                m_selectedJobIds.insert(jobs[r].manifest.job_id);
                        }
                        else if (ctrl)
                        {
                            // Ctrl+click: toggle
                            if (inMultiSelect)
                                m_selectedJobIds.erase(jobId);
                            else
                                m_selectedJobIds.insert(jobId);
                        }
                        else
                        {
                            // Plain click: single select
                            m_selectedJobIds.clear();
                            m_selectedJobIds.insert(jobId);
                        }

                        m_lastClickedIndex = i;
                        m_app->selectJob(jobId);
                    }

                    // Right-click context menu (active/paused jobs only)
                    if (job.current_state == "active" || job.current_state == "paused")
                    {
                        if (ImGui::BeginPopupContextItem("##JobCtx"))
                        {
                            if (job.current_state == "active")
                            {
                                if (ImGui::MenuItem("Pause"))
                                    m_app->pauseJob(jobId);
                                if (ImGui::MenuItem("Cancel"))
                                    m_app->cancelJob(jobId);
                            }
                            else
                            {
                                if (ImGui::MenuItem("Resume"))
                                    m_app->resumeJob(jobId);
                                if (ImGui::MenuItem("Cancel"))
                                    m_app->cancelJob(jobId);
                            }
                            ImGui::EndPopup();
                        }
                    }

                    // Template
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(job.manifest.template_id.c_str());

                    // State (colored)
                    ImGui::TableNextColumn();
                    ImVec4 stateColor(1, 1, 1, 1);
                    if (job.current_state == "active")
                        stateColor = ImVec4(0.3f, 0.5f, 0.9f, 1.0f);
                    else if (job.current_state == "paused")
                        stateColor = ImVec4(1.0f, 0.85f, 0.0f, 1.0f);
                    else if (job.current_state == "cancelled")
                        stateColor = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
                    else if (job.current_state == "completed")
                        stateColor = ImVec4(0.3f, 0.8f, 0.3f, 1.0f);
                    else if (job.current_state == "failed")
                        stateColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                    ImGui::TextColored(stateColor, "%s", job.current_state.c_str());

                    // Progress
                    ImGui::TableNextColumn();
                    auto progIt = m_progressCache.find(jobId);
                    if (progIt != m_progressCache.end() && progIt->second.total > 0)
                    {
                        float frac = (float)progIt->second.completed / (float)progIt->second.total;
                        float avail = ImGui::GetContentRegionAvail().x;
                        char label[32];
                        snprintf(label, sizeof(label), "%d/%d", progIt->second.completed, progIt->second.total);
                        float labelW = ImGui::CalcTextSize(label).x + ImGui::GetStyle().ItemSpacing.x;
                        float barW = avail - labelW;
                        if (barW < 40.0f) barW = 40.0f;
                        float barH = ImGui::GetTextLineHeight() - 2.0f;
                        float cellY = ImGui::GetCursorPosY();
                        float barOffset = (ImGui::GetTextLineHeight() - barH) * 0.5f;
                        ImGui::SetCursorPosY(cellY + barOffset);
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
                        ImGui::ProgressBar(frac, ImVec2(barW, barH), "");
                        ImGui::PopStyleVar();
                        ImGui::SameLine();
                        ImGui::SetCursorPosY(cellY);
                        ImGui::TextUnformatted(label);
                    }
                    else
                    {
                        ImGui::TextDisabled("--");
                    }

                    // Priority
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", job.current_priority);

                    // Frames
                    ImGui::TableNextColumn();
                    ImGui::Text("%d-%d", job.manifest.frame_start, job.manifest.frame_end);

                    // Submitted (formatted timestamp)
                    ImGui::TableNextColumn();
                    if (job.manifest.submitted_at_ms > 0)
                    {
                        time_t secs = static_cast<time_t>(job.manifest.submitted_at_ms / 1000);
                        struct tm tmBuf;
                        #ifdef _WIN32
                        localtime_s(&tmBuf, &secs);
                        #else
                        localtime_r(&secs, &tmBuf);
                        #endif
                        char timeBuf[32];
                        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", &tmBuf);
                        ImGui::TextUnformatted(timeBuf);
                    }

                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }

        // Bulk delete confirmation popup
        if (m_pendingBulkDelete)
        {
            ImGui::OpenPopup("Confirm Bulk Delete");
            m_pendingBulkDelete = false;
        }

        if (ImGui::BeginPopupModal("Confirm Bulk Delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            // Count deletable vs skipped
            int bulkDeletable = 0, bulkSkipped = 0;
            for (const auto& id : m_selectedJobIds)
            {
                for (const auto& j : jobs)
                {
                    if (j.manifest.job_id == id)
                    {
                        if (isDeletableState(j.current_state))
                            ++bulkDeletable;
                        else
                            ++bulkSkipped;
                        break;
                    }
                }
            }

            ImGui::Text("Delete %d job%s permanently? This cannot be undone.",
                         bulkDeletable, bulkDeletable == 1 ? "" : "s");
            if (bulkSkipped > 0)
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f),
                    "%d active/paused job%s will be skipped.",
                    bulkSkipped, bulkSkipped == 1 ? "" : "s");

            ImGui::Spacing();
            if (ImGui::Button("Delete"))
            {
                // Delete all deletable jobs, collect IDs to remove from selection
                std::vector<std::string> toRemove;
                for (const auto& id : m_selectedJobIds)
                {
                    for (const auto& j : jobs)
                    {
                        if (j.manifest.job_id == id && isDeletableState(j.current_state))
                        {
                            m_app->deleteJob(id);
                            toRemove.push_back(id);
                            break;
                        }
                    }
                }
                for (const auto& id : toRemove)
                    m_selectedJobIds.erase(id);

                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                ImGui::CloseCurrentPopup();

            ImGui::EndPopup();
        }
    }
    ImGui::End();
}

} // namespace SR
