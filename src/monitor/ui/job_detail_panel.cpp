#include "monitor/ui/job_detail_panel.h"
#include "monitor/monitor_app.h"
#include "monitor/template_manager.h"
#include "core/platform.h"
#include "core/job_types.h"
#include "core/atomic_file_io.h"

#include "monitor/ui/style.h"

#include <imgui.h>
#include <nfd.h>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace SR {

void JobDetailPanel::init(MonitorApp* app)
{
    m_app = app;
}

void JobDetailPanel::render()
{
    if (!visible) return;

    // Check for mode transitions
    if (m_app && m_app->shouldEnterSubmission())
    {
        m_mode = Mode::Submission;
        m_selectedTemplateIdx = -1;
        std::memset(m_jobNameBuf, 0, sizeof(m_jobNameBuf));
        std::memset(m_cmdBuf, 0, sizeof(m_cmdBuf));
        m_flagBufs.clear();
        m_outputBufs.clear();
        m_frameStart = 1; m_frameEnd = 250; m_chunkSize = 1;
        m_priority = 50; m_maxRetries = 3; m_timeout = 0;
        m_hasTimeout = false;
        m_errors.clear();
        m_detailJobId.clear();

        // Pre-fill from CLI submit request if present
        if (m_app->hasPendingSubmitRequest())
        {
            auto req = m_app->consumeSubmitRequest();

            // Find and select the matching template
            const auto& templates = m_app->templateManager().templates();
            for (int i = 0; i < (int)templates.size(); ++i)
            {
                if (!req.templateId.empty() && templates[i].template_id == req.templateId && templates[i].valid)
                {
                    onTemplateSelected(i);
                    break;
                }
            }

            // If no template match but we have a file, select first valid template
            if (m_selectedTemplateIdx < 0 && !req.file.empty())
            {
                for (int i = 0; i < (int)templates.size(); ++i)
                {
                    if (templates[i].valid)
                    {
                        onTemplateSelected(i);
                        break;
                    }
                }
            }

            // Pre-fill file path into first type:"file" flag
            if (!req.file.empty() && m_selectedTemplateIdx >= 0)
            {
                const auto& tmpl = templates[m_selectedTemplateIdx];
                for (int i = 0; i < (int)tmpl.flags.size(); ++i)
                {
                    if (tmpl.flags[i].type == "file" && i < (int)m_flagBufs.size())
                    {
                        std::memset(m_flagBufs[i].data(), 0, m_flagBufs[i].size());
                        #ifdef _WIN32
                        strncpy_s(m_flagBufs[i].data(), m_flagBufs[i].size(), req.file.c_str(), _TRUNCATE);
                        #else
                        std::strncpy(m_flagBufs[i].data(), req.file.c_str(), m_flagBufs[i].size() - 1);
                        #endif

                        // Auto-fill job name from file stem
                        namespace fs = std::filesystem;
                        std::string stem = fs::path(req.file).stem().string();
                        std::memset(m_jobNameBuf, 0, sizeof(m_jobNameBuf));
                        #ifdef _WIN32
                        strncpy_s(m_jobNameBuf, sizeof(m_jobNameBuf), stem.c_str(), _TRUNCATE);
                        #else
                        std::strncpy(m_jobNameBuf, stem.c_str(), sizeof(m_jobNameBuf) - 1);
                        #endif

                        // Trigger output pattern resolution
                        resolveOutputPatterns(tmpl);
                        break;
                    }
                }
            }
        }
    }

    if (m_app && !m_app->selectedJobId().empty() && m_app->selectedJobId() != m_detailJobId)
    {
        m_mode = Mode::Detail;
        m_detailJobId = m_app->selectedJobId();
        m_frameStatesJobId.clear(); // force rescan
    }

    if (ImGui::Begin("Job Detail", nullptr, ImGuiWindowFlags_NoTitleBar))
    {
        panelHeader("Job Detail", visible);
        if (!m_app || !m_app->isFarmRunning())
        {
            ImGui::TextDisabled("Farm not connected");
            ImGui::End();
            return;
        }

        switch (m_mode)
        {
            case Mode::Empty:      renderEmpty(); break;
            case Mode::Submission: renderSubmission(); break;
            case Mode::Detail:     renderDetail(); break;
        }
    }
    ImGui::End();
}

void JobDetailPanel::renderEmpty()
{
    ImGui::TextDisabled("Select a job from the list, or click 'New Job' to submit one.");
}

void JobDetailPanel::renderSubmission()
{
    if (Fonts::bold) ImGui::PushFont(Fonts::bold);
    ImGui::TextUnformatted("Submit New Job");
    if (Fonts::bold) ImGui::PopFont();
    ImGui::Separator();

    const auto& templates = m_app->templateManager().templates();

    // --- Template ---
    if (Fonts::bold) ImGui::PushFont(Fonts::bold);
    ImGui::TextUnformatted("Template");
    if (Fonts::bold) ImGui::PopFont();

    {
        const char* previewLabel = (m_selectedTemplateIdx >= 0 && m_selectedTemplateIdx < (int)templates.size())
            ? templates[m_selectedTemplateIdx].name.c_str()
            : "Select template...";

        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##template", previewLabel))
        {
            for (int i = 0; i < (int)templates.size(); ++i)
            {
                const auto& t = templates[i];
                ImGui::PushID(i);

                if (!t.valid)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                    ImGui::Selectable(t.name.c_str(), false, ImGuiSelectableFlags_Disabled);
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("Invalid: %s", t.validation_error.c_str());
                }
                else
                {
                    bool selected = (i == m_selectedTemplateIdx);
                    std::string label = t.name;
                    if (t.isExample)
                        label += " (example)";
                    if (ImGui::Selectable(label.c_str(), selected))
                        onTemplateSelected(i);
                }

                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }

    if (m_selectedTemplateIdx < 0 || m_selectedTemplateIdx >= (int)templates.size())
    {
        ImGui::Separator();
        ImGui::TextDisabled("Select a template to continue.");
        if (ImGui::Button("Cancel"))
            m_mode = Mode::Empty;
        return;
    }

    const auto& tmpl = templates[m_selectedTemplateIdx];

    ImGui::Separator();

    // --- Command (only if editable) ---
    if (tmpl.cmd.editable)
    {
        std::string cmdLabel = tmpl.cmd.label.empty() ? "Executable" : tmpl.cmd.label;

        if (Fonts::bold) ImGui::PushFont(Fonts::bold);
        ImGui::TextUnformatted(cmdLabel.c_str());
        if (Fonts::bold) ImGui::PopFont();

        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##cmd", m_cmdBuf, sizeof(m_cmdBuf));

        ImGui::Separator();
    }

    // --- File flags (with browse button, above Job Name) ---
    for (int i = 0; i < (int)tmpl.flags.size(); ++i)
    {
        const auto& f = tmpl.flags[i];
        if (!f.editable || f.type != "file") continue;

        ImGui::PushID(i + 1000);

        std::string label = f.info;
        if (f.required)
            label += " *";

        if (Fonts::bold) ImGui::PushFont(Fonts::bold);
        ImGui::TextUnformatted(label.c_str());
        if (Fonts::bold) ImGui::PopFont();

        // Input + Browse button
        float browseWidth = ImGui::CalcTextSize("Browse").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetNextItemWidth(-(browseWidth + ImGui::GetStyle().ItemSpacing.x));
        if (i < (int)m_flagBufs.size())
            ImGui::InputText("##flag", m_flagBufs[i].data(), m_flagBufs[i].size());

        ImGui::SameLine();
        if (ImGui::Button("Browse"))
        {
            nfdchar_t* outPath = nullptr;
            nfdfilteritem_t filterItem[1] = {};
            nfdfiltersize_t filterCount = 0;

            std::string filterName = f.info;
            std::string filterSpec = f.filter;

            if (!filterSpec.empty())
            {
                filterItem[0] = { filterName.c_str(), filterSpec.c_str() };
                filterCount = 1;
            }

            nfdresult_t result = NFD_OpenDialog(&outPath, filterCount > 0 ? filterItem : nullptr, filterCount, nullptr);
            if (result == NFD_OKAY && outPath)
            {
                // Fill the flag buffer with picked path
                if (i < (int)m_flagBufs.size())
                {
                    std::memset(m_flagBufs[i].data(), 0, m_flagBufs[i].size());
                    #ifdef _WIN32
                    strncpy_s(m_flagBufs[i].data(), m_flagBufs[i].size(), outPath, _TRUNCATE);
                    #else
                    std::strncpy(m_flagBufs[i].data(), outPath, m_flagBufs[i].size() - 1);
                    #endif
                }

                // Auto-fill Job Name if empty
                if (m_jobNameBuf[0] == '\0')
                {
                    namespace fs = std::filesystem;
                    std::string stem = fs::path(outPath).stem().string();
                    std::memset(m_jobNameBuf, 0, sizeof(m_jobNameBuf));
                    #ifdef _WIN32
                    strncpy_s(m_jobNameBuf, sizeof(m_jobNameBuf), stem.c_str(), _TRUNCATE);
                    #else
                    std::strncpy(m_jobNameBuf, stem.c_str(), sizeof(m_jobNameBuf) - 1);
                    #endif
                }

                NFD_FreePath(outPath);

                // Resolve output patterns now that file path is available
                resolveOutputPatterns(tmpl);
            }
        }

        ImGui::Separator();
        ImGui::PopID();
    }

    // --- Output path flags (dir + filename split) ---
    {
        // Resolve patterns each frame for non-overridden outputs (live update)
        resolveOutputPatterns(tmpl);

        for (auto& ob : m_outputBufs)
        {
            if (ob.flagIndex < 0 || ob.flagIndex >= (int)tmpl.flags.size()) continue;
            const auto& f = tmpl.flags[ob.flagIndex];

            ImGui::PushID(ob.flagIndex + 3000);

            std::string label = f.info;
            if (f.required) label += " *";

            if (Fonts::bold) ImGui::PushFont(Fonts::bold);
            ImGui::TextUnformatted(label.c_str());
            if (Fonts::bold) ImGui::PopFont();

            // Directory sub-label + input + Browse
            ImGui::TextDisabled("Directory");
            float browseWidth = ImGui::CalcTextSize("Browse").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetNextItemWidth(-(browseWidth + ImGui::GetStyle().ItemSpacing.x));
            if (ImGui::InputText("##outdir", ob.dirBuf.data(), ob.dirBuf.size()))
                ob.patternOverridden = true;

            ImGui::SameLine();
            if (ImGui::Button("Browse"))
            {
                nfdchar_t* outPath = nullptr;
                nfdresult_t result = NFD_PickFolder(&outPath, nullptr);
                if (result == NFD_OKAY && outPath)
                {
                    std::memset(ob.dirBuf.data(), 0, ob.dirBuf.size());
                    #ifdef _WIN32
                    strncpy_s(ob.dirBuf.data(), ob.dirBuf.size(), outPath, _TRUNCATE);
                    #else
                    std::strncpy(ob.dirBuf.data(), outPath, ob.dirBuf.size() - 1);
                    #endif
                    ob.patternOverridden = true;
                    NFD_FreePath(outPath);
                }
            }

            // Filename sub-label + input
            ImGui::TextDisabled("Filename");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##outfile", ob.filenameBuf.data(), ob.filenameBuf.size()))
                ob.patternOverridden = true;

            // Sync concatenated dir/filename back to m_flagBufs
            {
                std::string dir(ob.dirBuf.data());
                std::string filename(ob.filenameBuf.data());
                std::string fullPath = dir;
                if (!dir.empty() && !filename.empty())
                {
                    #ifdef _WIN32
                    if (dir.back() != '\\' && dir.back() != '/')
                        fullPath += "\\";
                    #else
                    if (dir.back() != '/')
                        fullPath += "/";
                    #endif
                }
                fullPath += filename;

                if (ob.flagIndex < (int)m_flagBufs.size())
                {
                    std::memset(m_flagBufs[ob.flagIndex].data(), 0, m_flagBufs[ob.flagIndex].size());
                    #ifdef _WIN32
                    strncpy_s(m_flagBufs[ob.flagIndex].data(), m_flagBufs[ob.flagIndex].size(), fullPath.c_str(), _TRUNCATE);
                    #else
                    std::strncpy(m_flagBufs[ob.flagIndex].data(), fullPath.c_str(), m_flagBufs[ob.flagIndex].size() - 1);
                    #endif
                }
            }

            // Full path preview
            if (Fonts::mono) ImGui::PushFont(Fonts::mono);
            ImGui::TextDisabled("%s", m_flagBufs[ob.flagIndex].data());
            if (Fonts::mono) ImGui::PopFont();

            ImGui::Separator();
            ImGui::PopID();
        }
    }

    // --- Job Name ---
    if (Fonts::bold) ImGui::PushFont(Fonts::bold);
    ImGui::TextUnformatted("Job Name");
    if (Fonts::bold) ImGui::PopFont();

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##jobname", m_jobNameBuf, sizeof(m_jobNameBuf));

    ImGui::Separator();

    // --- Non-file editable flags ---
    for (int i = 0; i < (int)tmpl.flags.size(); ++i)
    {
        const auto& f = tmpl.flags[i];
        if (!f.editable || f.type == "file" || f.type == "output") continue;

        ImGui::PushID(i + 2000);

        std::string label = f.info;
        if (f.required)
            label += " *";

        if (Fonts::bold) ImGui::PushFont(Fonts::bold);
        ImGui::TextUnformatted(label.c_str());
        if (Fonts::bold) ImGui::PopFont();

        ImGui::SetNextItemWidth(-FLT_MIN);
        if (i < (int)m_flagBufs.size())
            ImGui::InputText("##flag", m_flagBufs[i].data(), m_flagBufs[i].size());

        if (!f.flag.empty())
            ImGui::TextDisabled("%s", f.flag.c_str());

        ImGui::Separator();
        ImGui::PopID();
    }

    // --- Frame Start ---
    if (Fonts::bold) ImGui::PushFont(Fonts::bold);
    ImGui::TextUnformatted("Frame Start");
    if (Fonts::bold) ImGui::PopFont();

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputInt("##fstart", &m_frameStart);

    ImGui::Separator();

    // --- Frame End ---
    if (Fonts::bold) ImGui::PushFont(Fonts::bold);
    ImGui::TextUnformatted("Frame End");
    if (Fonts::bold) ImGui::PopFont();

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputInt("##fend", &m_frameEnd);

    ImGui::Separator();

    // --- Chunk Size ---
    if (Fonts::bold) ImGui::PushFont(Fonts::bold);
    ImGui::TextUnformatted("Chunk Size");
    if (Fonts::bold) ImGui::PopFont();

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputInt("##chunk", &m_chunkSize);
    if (m_chunkSize < 1) m_chunkSize = 1;
    ImGui::TextDisabled("Frames per task sent to a node");

    ImGui::Separator();

    // --- Priority ---
    if (Fonts::bold) ImGui::PushFont(Fonts::bold);
    ImGui::TextUnformatted("Priority");
    if (Fonts::bold) ImGui::PopFont();

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##priority", &m_priority, 1, 100);
    ImGui::TextDisabled("Higher priority jobs are picked first");

    ImGui::Separator();

    // --- Max Retries ---
    if (Fonts::bold) ImGui::PushFont(Fonts::bold);
    ImGui::TextUnformatted("Max Retries");
    if (Fonts::bold) ImGui::PopFont();

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputInt("##retries", &m_maxRetries);
    if (m_maxRetries < 0) m_maxRetries = 0;

    ImGui::Separator();

    // --- Timeout ---
    if (Fonts::bold) ImGui::PushFont(Fonts::bold);
    ImGui::TextUnformatted("Timeout");
    if (Fonts::bold) ImGui::PopFont();

    ImGui::Checkbox("Enable##timeout_check", &m_hasTimeout);
    if (m_hasTimeout)
    {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputInt("##timeout_sec", &m_timeout);
        if (m_timeout < 0) m_timeout = 0;
        ImGui::TextDisabled("Seconds before a frame is killed");
    }

    ImGui::Separator();

    // --- Preview ---
    if (Fonts::bold) ImGui::PushFont(Fonts::bold);
    ImGui::TextUnformatted("Command Preview");
    if (Fonts::bold) ImGui::PopFont();

    {
        std::vector<std::string> flagVals;
        for (const auto& buf : m_flagBufs)
            flagVals.push_back(std::string(buf.data()));

        std::string preview = m_app->templateManager().buildCommandPreview(
            tmpl, flagVals, std::string(m_cmdBuf));

        if (Fonts::mono) ImGui::PushFont(Fonts::mono);
        ImGui::TextWrapped("%s", preview.c_str());
        if (Fonts::mono) ImGui::PopFont();
    }

    // --- Errors ---
    if (!m_errors.empty())
    {
        ImGui::Spacing();
        for (const auto& err : m_errors)
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", err.c_str());
    }

    // --- Buttons ---
    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::Button("Cancel"))
        m_mode = Mode::Empty;

    ImGui::SameLine();

    if (ImGui::Button("Submit"))
        doSubmit();
}

void JobDetailPanel::renderDetail()
{
    // Find job by id
    const auto& jobs = m_app->jobManager().jobs();
    const JobInfo* found = nullptr;
    for (const auto& j : jobs)
    {
        if (j.manifest.job_id == m_detailJobId)
        {
            found = &j;
            break;
        }
    }

    if (!found)
    {
        ImGui::TextDisabled("Job not found: %s", m_detailJobId.c_str());
        if (ImGui::Button("Clear"))
        {
            m_mode = Mode::Empty;
            m_detailJobId.clear();
        }
        return;
    }

    const auto& manifest = found->manifest;

    // Scan frame states (with cooldown)
    scanFrameStates(m_detailJobId, manifest);

    // Header
    ImGui::TextUnformatted(manifest.job_id.c_str());

    // State badge
    ImGui::SameLine();
    ImVec4 stateColor(1, 1, 1, 1);
    if (found->current_state == "active")
        stateColor = ImVec4(0.3f, 0.5f, 0.9f, 1.0f);
    else if (found->current_state == "paused")
        stateColor = ImVec4(1.0f, 0.85f, 0.0f, 1.0f);
    else if (found->current_state == "cancelled")
        stateColor = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    else if (found->current_state == "completed")
        stateColor = ImVec4(0.3f, 0.8f, 0.3f, 1.0f);
    else if (found->current_state == "failed")
        stateColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
    ImGui::TextColored(stateColor, "[%s]", found->current_state.c_str());

    ImGui::Separator();

    // Info section
    ImGui::Text("Template: %s", manifest.template_id.c_str());
    ImGui::Text("Priority: %d", found->current_priority);
    ImGui::Text("Submitted by: %s", manifest.submitted_by.c_str());

    if (manifest.submitted_at_ms > 0)
    {
        time_t secs = static_cast<time_t>(manifest.submitted_at_ms / 1000);
        struct tm tmBuf;
        #ifdef _WIN32
        localtime_s(&tmBuf, &secs);
        #else
        localtime_r(&secs, &tmBuf);
        #endif
        char timeBuf[32];
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tmBuf);
        ImGui::Text("Submitted at: %s", timeBuf);
    }

    // --- Controls ---
    ImGui::Spacing();
    ImGui::SeparatorText("Controls");

    const auto& state = found->current_state;

    if (state == "active")
    {
        if (ImGui::Button("Pause"))
            m_app->pauseJob(m_detailJobId);
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            m_pendingCancel = true;
        ImGui::SameLine();
        if (ImGui::Button("Requeue"))
            m_pendingRequeue = true;
    }
    else if (state == "paused")
    {
        if (ImGui::Button("Resume"))
            m_app->resumeJob(m_detailJobId);
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            m_pendingCancel = true;
        ImGui::SameLine();
        if (ImGui::Button("Requeue"))
            m_pendingRequeue = true;
    }
    else if (state == "cancelled" || state == "completed" || state == "failed")
    {
        if (ImGui::Button("Requeue"))
            m_pendingRequeue = true;
        ImGui::SameLine();
        if (ImGui::Button("Delete"))
            m_pendingDelete = true;
    }

    // Open Output folder button
    if (manifest.output_dir.has_value() && !manifest.output_dir.value().empty())
    {
        ImGui::SameLine();
        if (ImGui::Button("Open Output"))
            openFolderInExplorer(std::filesystem::path(manifest.output_dir.value()));
    }

    // --- Job Progress ---
    ImGui::Spacing();
    ImGui::SeparatorText("Progress");
    renderJobProgress(manifest);

    // --- Frame Grid ---
    ImGui::Spacing();
    ImGui::SeparatorText("Frames");
    renderFrameGrid(manifest);

    // --- Chunk Table ---
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Chunks"))
        renderChunkTable(manifest);

    // --- Collapsible details ---
    if (ImGui::CollapsingHeader("Command & Flags"))
    {
        std::string os = currentOS();
        auto cmdIt = manifest.cmd.find(os);
        if (cmdIt != manifest.cmd.end())
            ImGui::Text("Executable (%s): %s", os.c_str(), cmdIt->second.c_str());
        else
            ImGui::TextDisabled("No executable for %s", os.c_str());

        for (const auto& [osKey, path] : manifest.cmd)
        {
            if (osKey != os)
                ImGui::TextDisabled("  %s: %s", osKey.c_str(), path.c_str());
        }

        ImGui::Spacing();
        for (const auto& f : manifest.flags)
        {
            std::string line;
            if (!f.flag.empty())
                line += f.flag;
            if (f.value.has_value())
            {
                if (!line.empty()) line += " ";
                line += f.value.value();
            }
            if (!line.empty())
                ImGui::BulletText("%s", line.c_str());
        }
    }

    if (ImGui::CollapsingHeader("Job Settings"))
    {
        ImGui::Text("Frame range: %d - %d", manifest.frame_start, manifest.frame_end);
        ImGui::Text("Chunk size: %d", manifest.chunk_size);
        ImGui::Text("Max retries: %d", manifest.max_retries);
        if (manifest.timeout_seconds.has_value())
            ImGui::Text("Timeout: %d seconds", manifest.timeout_seconds.value());
        else
            ImGui::TextDisabled("Timeout: none");

        if (!manifest.tags_required.empty())
        {
            std::string tags;
            for (const auto& t : manifest.tags_required)
            {
                if (!tags.empty()) tags += ", ";
                tags += t;
            }
            ImGui::Text("Required tags: %s", tags.c_str());
        }
    }

    // Confirmation popups
    if (m_pendingCancel)
    {
        ImGui::OpenPopup("Confirm Cancel");
        m_pendingCancel = false;
    }
    if (ImGui::BeginPopupModal("Confirm Cancel", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Cancel this job? Running frames will be aborted.");
        if (ImGui::Button("Yes, Cancel"))
        {
            m_app->cancelJob(m_detailJobId);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (m_pendingRequeue)
    {
        ImGui::OpenPopup("Confirm Requeue");
        m_pendingRequeue = false;
    }
    if (ImGui::BeginPopupModal("Confirm Requeue", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Requeue this job? A new copy will be submitted.");
        if (ImGui::Button("Yes, Requeue"))
        {
            m_app->requeueJob(m_detailJobId);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (m_pendingDelete)
    {
        ImGui::OpenPopup("Confirm Delete");
        m_pendingDelete = false;
    }
    if (ImGui::BeginPopupModal("Confirm Delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Delete this job permanently? This cannot be undone.");
        if (ImGui::Button("Yes, Delete"))
        {
            m_app->deleteJob(m_detailJobId);
            m_mode = Mode::Empty;
            m_detailJobId.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ─── Frame state scanning ────────────────────────────────────────────────────

void JobDetailPanel::scanFrameStates(const std::string& jobId, const JobManifest& manifest)
{
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastFrameScan).count();
    if (m_frameStatesJobId == jobId && elapsed < FRAME_SCAN_COOLDOWN_MS)
        return;

    m_lastFrameScan = now;
    m_frameStatesJobId = jobId;

    int totalFrames = manifest.frame_end - manifest.frame_start + 1;
    m_frameStates.assign(totalFrames, FrameState{});

    // Read dispatch.json
    auto dispatchPath = m_app->farmPath() / "jobs" / jobId / "dispatch.json";
    auto data = AtomicFileIO::safeReadJson(dispatchPath);
    if (!data.has_value())
    {
        m_dispatchChunks.clear();
        return;
    }

    try
    {
        DispatchTable dt = data.value().get<DispatchTable>();

        for (const auto& dc : dt.chunks)
        {
            ChunkState cs = ChunkState::Unclaimed;
            if (dc.state == "assigned")        cs = ChunkState::Rendering;
            else if (dc.state == "completed")  cs = ChunkState::Completed;
            else if (dc.state == "failed")     cs = ChunkState::Failed;

            for (int f = dc.frame_start; f <= dc.frame_end; ++f)
            {
                int idx = f - manifest.frame_start;
                if (idx >= 0 && idx < totalFrames)
                {
                    m_frameStates[idx].state = cs;
                    m_frameStates[idx].ownerNodeId = dc.assigned_to;
                }
            }
        }

        m_dispatchChunks = std::move(dt.chunks);

        // Scan event files for per-frame completions within "assigned" chunks
        auto eventsBaseDir = m_app->farmPath() / "jobs" / jobId / "events";
        std::error_code ec2;
        if (std::filesystem::is_directory(eventsBaseDir, ec2))
        {
            for (const auto& nodeDir : std::filesystem::directory_iterator(eventsBaseDir, ec2))
            {
                if (!nodeDir.is_directory(ec2)) continue;
                for (const auto& entry : std::filesystem::directory_iterator(nodeDir.path(), ec2))
                {
                    if (entry.path().extension() != ".json") continue;
                    std::string stem = entry.path().stem().string();
                    if (stem.find("_frame_finished_") == std::string::npos) continue;

                    // Parse frame from filename: {seq}_frame_finished_{NNNNNN}-{NNNNNN}
                    auto pos = stem.find("_frame_finished_") + 16;
                    auto dash = stem.find('-', pos);
                    if (dash == std::string::npos) continue;
                    try
                    {
                        int frameNum = std::stoi(stem.substr(pos, dash - pos));
                        int idx = frameNum - manifest.frame_start;
                        if (idx >= 0 && idx < totalFrames &&
                            m_frameStates[idx].state == ChunkState::Rendering)
                        {
                            m_frameStates[idx].state = ChunkState::Completed;
                        }
                    }
                    catch (...) {}
                }
            }
        }
    }
    catch (...)
    {
        m_dispatchChunks.clear();
    }
}

// ─── Job progress ────────────────────────────────────────────────────────────

void JobDetailPanel::renderJobProgress(const JobManifest& /*manifest*/)
{
    if (m_frameStates.empty())
    {
        ImGui::TextDisabled("No frame data");
        return;
    }

    int total = (int)m_frameStates.size();
    int completed = 0, rendering = 0, failed = 0;
    for (const auto& fs : m_frameStates)
    {
        if (fs.state == ChunkState::Completed) ++completed;
        else if (fs.state == ChunkState::Rendering) ++rendering;
        else if (fs.state == ChunkState::Failed) ++failed;
    }

    // Summary text
    std::string summary = std::to_string(completed) + "/" + std::to_string(total) + " frames completed";
    if (rendering > 0)
        summary += "  |  " + std::to_string(rendering) + " rendering";
    if (failed > 0)
        summary += "  |  " + std::to_string(failed) + " failed";
    ImGui::TextUnformatted(summary.c_str());

    // Progress bar
    float fraction = total > 0 ? (float)completed / (float)total : 0.0f;
    ImGui::ProgressBar(fraction, ImVec2(-1, 0));
}

// ─── Frame grid ──────────────────────────────────────────────────────────────

void JobDetailPanel::renderFrameGrid(const JobManifest& manifest)
{
    if (m_frameStates.empty())
        return;

    int totalFrames = (int)m_frameStates.size();
    float cellSize = 14.0f;
    float gap = 2.0f;
    float availWidth = ImGui::GetContentRegionAvail().x;
    int cols = (std::max)(1, (int)(availWidth / (cellSize + gap)));

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();

    for (int i = 0; i < totalFrames; ++i)
    {
        int col = i % cols;
        int row = i / cols;
        float x = origin.x + col * (cellSize + gap);
        float y = origin.y + row * (cellSize + gap);

        // Cell color based on state
        ImU32 color;
        switch (m_frameStates[i].state)
        {
            case ChunkState::Rendering:  color = IM_COL32(77, 128, 230, 255); break;
            case ChunkState::Completed:  color = IM_COL32(77, 204, 77, 255);  break;
            case ChunkState::Failed:     color = IM_COL32(230, 77, 77, 255);  break;
            case ChunkState::Abandoned:  color = IM_COL32(153, 128, 51, 255); break;
            default:                     color = IM_COL32(64, 64, 64, 255);   break;
        }

        drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + cellSize, y + cellSize), color);
    }

    // Invisible buttons for click detection + tooltips
    int totalRows = (totalFrames + cols - 1) / cols;
    ImVec2 gridSize(availWidth, totalRows * (cellSize + gap));

    ImGui::PushID("##framegrid");
    for (int i = 0; i < totalFrames; ++i)
    {
        int col = i % cols;
        int row = i / cols;
        float x = origin.x + col * (cellSize + gap);
        float y = origin.y + row * (cellSize + gap);

        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID(i);
        ImGui::InvisibleButton("##cell", ImVec2(cellSize, cellSize));

        int frameNum = manifest.frame_start + i;

        if (ImGui::IsItemHovered())
        {
            const char* stateStr = "unclaimed";
            switch (m_frameStates[i].state)
            {
                case ChunkState::Rendering: stateStr = "rendering"; break;
                case ChunkState::Completed: stateStr = "completed"; break;
                case ChunkState::Failed:    stateStr = "failed";    break;
                case ChunkState::Abandoned: stateStr = "abandoned"; break;
                default: break;
            }
            if (!m_frameStates[i].ownerNodeId.empty())
                ImGui::SetTooltip("Frame %d: %s (%s)", frameNum, stateStr,
                                  m_frameStates[i].ownerNodeId.c_str());
            else
                ImGui::SetTooltip("Frame %d: %s", frameNum, stateStr);
        }

        ImGui::PopID();
    }
    ImGui::PopID();

    // Advance cursor past the grid
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + totalRows * (cellSize + gap) + 4.0f));
}

// ─── Chunk table ─────────────────────────────────────────────────────────────

std::string JobDetailPanel::hostnameForNodeId(const std::string& nodeId) const
{
    if (nodeId.empty()) return "";
    auto nodes = m_app->heartbeatManager().getNodeSnapshot();
    for (const auto& n : nodes)
    {
        if (n.heartbeat.node_id == nodeId)
            return n.heartbeat.hostname;
    }
    return nodeId.substr(0, 8); // fallback: truncated node ID
}

void JobDetailPanel::renderChunkTable(const JobManifest& /*manifest*/)
{
    if (m_dispatchChunks.empty())
    {
        ImGui::TextDisabled("No dispatch data");
        return;
    }

    bool isCoordinator = m_app->isCoordinator();

    int numCols = isCoordinator ? 5 : 4;
    if (!ImGui::BeginTable("##chunks", numCols,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
            ImVec2(0, (std::min)(200.0f, m_dispatchChunks.size() * 24.0f + 28.0f))))
        return;

    ImGui::TableSetupColumn("Range", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Worker", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Elapsed", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    if (isCoordinator)
        ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableHeadersRow();

    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    for (int i = 0; i < (int)m_dispatchChunks.size(); ++i)
    {
        const auto& dc = m_dispatchChunks[i];
        ImGui::PushID(i);
        ImGui::TableNextRow();

        // Range
        ImGui::TableNextColumn();
        char rangeBuf[32];
        if (dc.frame_start == dc.frame_end)
            snprintf(rangeBuf, sizeof(rangeBuf), "%d", dc.frame_start);
        else
            snprintf(rangeBuf, sizeof(rangeBuf), "%d-%d", dc.frame_start, dc.frame_end);
        ImGui::TextUnformatted(rangeBuf);

        // State (color-coded)
        ImGui::TableNextColumn();
        {
            ImVec4 stateColor(0.5f, 0.5f, 0.5f, 1.0f); // pending = gray
            std::string stateLabel = "Pending";
            if (dc.state == "assigned")
            {
                stateColor = ImVec4(0.3f, 0.5f, 0.9f, 1.0f);
                stateLabel = "Rendering";
            }
            else if (dc.state == "completed")
            {
                stateColor = ImVec4(0.3f, 0.8f, 0.3f, 1.0f);
                stateLabel = "Completed";
            }
            else if (dc.state == "failed")
            {
                stateColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                stateLabel = "Failed (" + std::to_string(dc.retry_count) + ")";
            }
            ImGui::TextColored(stateColor, "%s", stateLabel.c_str());
        }

        // Worker (hostname)
        ImGui::TableNextColumn();
        if (!dc.assigned_to.empty())
        {
            std::string hostname = hostnameForNodeId(dc.assigned_to);
            ImGui::TextUnformatted(hostname.c_str());
        }
        else
        {
            ImGui::TextDisabled("--");
        }

        // Elapsed
        ImGui::TableNextColumn();
        if (dc.state == "assigned" && dc.assigned_at_ms > 0)
        {
            int64_t elapsedMs = nowMs - dc.assigned_at_ms;
            int secs = (int)(elapsedMs / 1000);
            if (secs >= 3600)
                ImGui::Text("%dh%02dm", secs / 3600, (secs % 3600) / 60);
            else if (secs >= 60)
                ImGui::Text("%dm%02ds", secs / 60, secs % 60);
            else
                ImGui::Text("%ds", secs);
        }
        else if (dc.state == "completed" && dc.completed_at_ms > 0 && dc.assigned_at_ms > 0)
        {
            int64_t elapsedMs = dc.completed_at_ms - dc.assigned_at_ms;
            int secs = (int)(elapsedMs / 1000);
            if (secs >= 3600)
                ImGui::Text("%dh%02dm", secs / 3600, (secs % 3600) / 60);
            else if (secs >= 60)
                ImGui::Text("%dm%02ds", secs / 60, secs % 60);
            else
                ImGui::Text("%ds", secs);
        }
        else
        {
            ImGui::TextDisabled("--");
        }

        // Action buttons (coordinator only)
        if (isCoordinator)
        {
            ImGui::TableNextColumn();
            if (dc.state == "assigned")
            {
                if (ImGui::SmallButton("Reassign"))
                    m_app->reassignChunk(m_detailJobId, dc.frame_start, dc.frame_end);
            }
            else if (dc.state == "failed")
            {
                if (ImGui::SmallButton("Retry"))
                    m_app->retryFailedChunk(m_detailJobId, dc.frame_start, dc.frame_end);
            }
        }

        ImGui::PopID();
    }

    ImGui::EndTable();
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

void JobDetailPanel::onTemplateSelected(int idx)
{
    m_selectedTemplateIdx = idx;
    m_errors.clear();

    const auto& templates = m_app->templateManager().templates();
    if (idx < 0 || idx >= (int)templates.size())
        return;

    const auto& tmpl = templates[idx];

    // Populate cmd buffer for current OS
    std::string os = currentOS();
    std::string cmdStr = getCmdForOS(tmpl.cmd, os);
    std::memset(m_cmdBuf, 0, sizeof(m_cmdBuf));
    #ifdef _WIN32
    strncpy_s(m_cmdBuf, sizeof(m_cmdBuf), cmdStr.c_str(), _TRUNCATE);
    #else
    std::strncpy(m_cmdBuf, cmdStr.c_str(), sizeof(m_cmdBuf) - 1);
    #endif

    // Allocate flag buffers
    m_flagBufs.clear();
    m_flagBufs.resize(tmpl.flags.size());
    for (int i = 0; i < (int)tmpl.flags.size(); ++i)
    {
        m_flagBufs[i].resize(512, '\0');
        if (tmpl.flags[i].value.has_value())
        {
            const auto& val = tmpl.flags[i].value.value();
            #ifdef _WIN32
            strncpy_s(m_flagBufs[i].data(), m_flagBufs[i].size(), val.c_str(), _TRUNCATE);
            #else
            std::strncpy(m_flagBufs[i].data(), val.c_str(), m_flagBufs[i].size() - 1);
            #endif
        }
    }

    // Build output buffers for type:"output" flags
    m_outputBufs.clear();
    for (int i = 0; i < (int)tmpl.flags.size(); ++i)
    {
        if (tmpl.flags[i].type == "output")
        {
            OutputBuf ob;
            ob.flagIndex = i;
            ob.dirBuf.resize(512, '\0');
            ob.filenameBuf.resize(256, '\0');
            ob.patternOverridden = false;
            m_outputBufs.push_back(std::move(ob));
        }
    }

    // Initial pattern resolution (date/time tokens resolve immediately)
    resolveOutputPatterns(tmpl);

    // Populate job settings from defaults
    m_frameStart = tmpl.job_defaults.frame_start;
    m_frameEnd = tmpl.job_defaults.frame_end;
    m_chunkSize = tmpl.job_defaults.chunk_size;
    m_priority = tmpl.job_defaults.priority;
    m_maxRetries = tmpl.job_defaults.max_retries;
    if (tmpl.job_defaults.timeout_seconds.has_value())
    {
        m_hasTimeout = true;
        m_timeout = tmpl.job_defaults.timeout_seconds.value();
    }
    else
    {
        m_hasTimeout = false;
        m_timeout = 0;
    }
}

void JobDetailPanel::resolveOutputPatterns(const JobTemplate& tmpl)
{
    // Collect current flag values
    std::vector<std::string> flagVals;
    for (const auto& buf : m_flagBufs)
        flagVals.push_back(std::string(buf.data()));

    for (auto& ob : m_outputBufs)
    {
        if (ob.patternOverridden) continue;
        if (ob.flagIndex < 0 || ob.flagIndex >= (int)tmpl.flags.size()) continue;

        const auto& f = tmpl.flags[ob.flagIndex];
        if (!f.default_pattern.has_value()) continue;

        std::string resolved = TemplateManager::resolvePattern(
            f.default_pattern.value(), tmpl, flagVals);

        // Split into directory and filename
        namespace fs = std::filesystem;
        fs::path p(resolved);
        std::string dir = p.parent_path().string();
        std::string filename = p.filename().string();

        std::memset(ob.dirBuf.data(), 0, ob.dirBuf.size());
        std::memset(ob.filenameBuf.data(), 0, ob.filenameBuf.size());

        #ifdef _WIN32
        strncpy_s(ob.dirBuf.data(), ob.dirBuf.size(), dir.c_str(), _TRUNCATE);
        strncpy_s(ob.filenameBuf.data(), ob.filenameBuf.size(), filename.c_str(), _TRUNCATE);
        #else
        std::strncpy(ob.dirBuf.data(), dir.c_str(), ob.dirBuf.size() - 1);
        std::strncpy(ob.filenameBuf.data(), filename.c_str(), ob.filenameBuf.size() - 1);
        #endif

        // Sync concatenated path back to m_flagBufs
        std::string fullPath = dir;
        if (!dir.empty() && !filename.empty())
        {
            #ifdef _WIN32
            fullPath += "\\";
            #else
            fullPath += "/";
            #endif
        }
        fullPath += filename;

        if (ob.flagIndex < (int)m_flagBufs.size())
        {
            std::memset(m_flagBufs[ob.flagIndex].data(), 0, m_flagBufs[ob.flagIndex].size());
            #ifdef _WIN32
            strncpy_s(m_flagBufs[ob.flagIndex].data(), m_flagBufs[ob.flagIndex].size(), fullPath.c_str(), _TRUNCATE);
            #else
            std::strncpy(m_flagBufs[ob.flagIndex].data(), fullPath.c_str(), m_flagBufs[ob.flagIndex].size() - 1);
            #endif
        }
    }
}

void JobDetailPanel::doSubmit()
{
    m_errors.clear();

    const auto& templates = m_app->templateManager().templates();
    if (m_selectedTemplateIdx < 0 || m_selectedTemplateIdx >= (int)templates.size())
    {
        m_errors.push_back("No template selected");
        return;
    }

    const auto& tmpl = templates[m_selectedTemplateIdx];

    // Final resolution pass for output patterns with current timestamp
    {
        std::vector<std::string> currentVals;
        for (const auto& buf : m_flagBufs)
            currentVals.push_back(std::string(buf.data()));

        auto now = std::chrono::system_clock::now();
        for (auto& ob : m_outputBufs)
        {
            if (ob.flagIndex < 0 || ob.flagIndex >= (int)tmpl.flags.size()) continue;
            const auto& f = tmpl.flags[ob.flagIndex];
            if (!f.default_pattern.has_value() || ob.patternOverridden) continue;

            std::string resolved = TemplateManager::resolvePattern(
                f.default_pattern.value(), tmpl, currentVals, now);

            namespace fs = std::filesystem;
            fs::path p(resolved);
            std::string dir = p.parent_path().string();
            std::string filename = p.filename().string();

            std::string fullPath = dir;
            if (!dir.empty() && !filename.empty())
            {
                #ifdef _WIN32
                fullPath += "\\";
                #else
                fullPath += "/";
                #endif
            }
            fullPath += filename;

            if (ob.flagIndex < (int)m_flagBufs.size())
            {
                std::memset(m_flagBufs[ob.flagIndex].data(), 0, m_flagBufs[ob.flagIndex].size());
                #ifdef _WIN32
                strncpy_s(m_flagBufs[ob.flagIndex].data(), m_flagBufs[ob.flagIndex].size(), fullPath.c_str(), _TRUNCATE);
                #else
                std::strncpy(m_flagBufs[ob.flagIndex].data(), fullPath.c_str(), m_flagBufs[ob.flagIndex].size() - 1);
                #endif
            }
        }
    }

    // Collect flag values from buffers
    std::vector<std::string> flagValues;
    for (const auto& buf : m_flagBufs)
        flagValues.push_back(std::string(buf.data()));

    std::string jobName(m_jobNameBuf);
    std::string cmdPath(m_cmdBuf);
    auto jobsDir = m_app->farmPath() / "jobs";

    // Validate
    auto errors = TemplateManager::validateSubmission(
        tmpl, flagValues, cmdPath, jobName,
        m_frameStart, m_frameEnd, m_chunkSize, jobsDir);

    if (!errors.empty())
    {
        m_errors = std::move(errors);
        return;
    }

    // Generate slug
    auto slug = TemplateManager::generateSlug(jobName, jobsDir);
    if (slug.empty())
    {
        m_errors.push_back("Failed to generate a unique job slug");
        return;
    }

    // Bake manifest
    std::string os = currentOS();
    std::optional<int> timeout = m_hasTimeout ? std::optional<int>(m_timeout) : std::nullopt;

    auto manifest = m_app->templateManager().bakeManifest(
        tmpl, flagValues, cmdPath, slug,
        m_frameStart, m_frameEnd, m_chunkSize,
        m_maxRetries, timeout,
        m_app->identity().nodeId(), os);

    // Submit
    auto result = m_app->jobManager().submitJob(m_app->farmPath(), manifest, m_priority);
    if (result.empty())
    {
        m_errors.push_back("Failed to write job to filesystem");
        return;
    }

    // Success — switch to detail view
    m_app->selectJob(result);
    m_mode = Mode::Detail;
    m_detailJobId = result;
}

std::string JobDetailPanel::currentOS() const
{
    return getOS();
}

} // namespace SR
