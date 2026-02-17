#include "monitor/ui/settings_panel.h"
#include "monitor/monitor_app.h"
#include "monitor/ui/style.h"
#include "core/config.h"
#include "core/platform.h"

#include <imgui.h>
#include <nfd.h>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable: 4996) // strncpy deprecation
#endif
#include <filesystem>
#include <algorithm>
#include <sstream>

namespace SR {

void SettingsPanel::init(MonitorApp* app)
{
    m_app = app;
    loadFromConfig();
}

void SettingsPanel::loadFromConfig()
{
    const auto& cfg = m_app->config();

    std::strncpy(m_syncRootBuf, cfg.sync_root.c_str(), sizeof(m_syncRootBuf) - 1);
    m_syncRootBuf[sizeof(m_syncRootBuf) - 1] = '\0';

    m_timingPreset = static_cast<int>(cfg.timing_preset);

    // Join tags with comma
    std::string tagsStr;
    for (size_t i = 0; i < cfg.tags.size(); i++)
    {
        if (i > 0) tagsStr += ", ";
        tagsStr += cfg.tags[i];
    }
    std::strncpy(m_tagsBuf, tagsStr.c_str(), sizeof(m_tagsBuf) - 1);
    m_tagsBuf[sizeof(m_tagsBuf) - 1] = '\0';

    m_isCoordinator = cfg.is_coordinator;
    m_autoStartAgent = cfg.auto_start_agent;
    m_showNotifications = cfg.show_notifications;
    m_fontScale = cfg.font_scale;

    m_heartbeatMs = static_cast<int>(cfg.timing.heartbeat_interval_ms);
    m_scanMs = static_cast<int>(cfg.timing.scan_interval_ms);
    m_claimSettleMs = static_cast<int>(cfg.timing.claim_settle_ms);
    m_deadThresholdScans = static_cast<int>(cfg.timing.dead_threshold_scans);
    m_savedSyncRoot = cfg.sync_root;
}

void SettingsPanel::applyToConfig()
{
    auto& cfg = m_app->config();

    cfg.sync_root = m_syncRootBuf;
    cfg.timing_preset = static_cast<TimingPreset>(m_timingPreset);

    // Parse tags from comma-separated string
    cfg.tags.clear();
    std::istringstream ss(m_tagsBuf);
    std::string tag;
    while (std::getline(ss, tag, ','))
    {
        // Trim whitespace
        auto start = tag.find_first_not_of(" \t");
        auto end = tag.find_last_not_of(" \t");
        if (start != std::string::npos)
            cfg.tags.push_back(tag.substr(start, end - start + 1));
    }

    cfg.is_coordinator = m_isCoordinator;
    cfg.auto_start_agent = m_autoStartAgent;
    cfg.show_notifications = m_showNotifications;
    cfg.font_scale = m_fontScale;

    if (cfg.timing_preset == TimingPreset::Custom)
    {
        cfg.timing.heartbeat_interval_ms = static_cast<uint32_t>(m_heartbeatMs);
        cfg.timing.scan_interval_ms = static_cast<uint32_t>(m_scanMs);
        cfg.timing.claim_settle_ms = static_cast<uint32_t>(m_claimSettleMs);
        cfg.timing.dead_threshold_scans = static_cast<uint32_t>(m_deadThresholdScans);
    }
    else
    {
        cfg.timing = timingForPreset(cfg.timing_preset);
    }

    // Apply font scale immediately
    ImGui::GetIO().FontGlobalScale = m_fontScale;
}

void SettingsPanel::drawFontSizeSection()
{
    ImGui::TextUnformatted("Font Size");
    ImGui::Spacing();

    // Preset buttons
    ImGui::Text("Presets:");
    ImGui::SameLine();

    if (ImGui::Button("Small"))
        m_fontScale = FONT_SCALE_SMALL;
    ImGui::SameLine();

    if (ImGui::Button("Medium"))
        m_fontScale = FONT_SCALE_MEDIUM;
    ImGui::SameLine();

    if (ImGui::Button("Large"))
        m_fontScale = FONT_SCALE_LARGE;
    ImGui::SameLine();

    if (ImGui::Button("X-Large"))
        m_fontScale = FONT_SCALE_XLARGE;

    ImGui::Spacing();

    // Slider for fine control
    ImGui::Text("Custom Scale:");
    ImGui::SetNextItemWidth(-1);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.09f, 0.09f, 0.09f, 1.0f));
    ImGui::SliderFloat("##fontscale", &m_fontScale, 0.5f, 2.0f, "%.2fx");
    ImGui::PopStyleColor();
}

void SettingsPanel::drawFontPreview()
{
    ImGui::TextUnformatted("Font Preview");
    ImGui::Spacing();

    // Save current scale, apply preview scale inside child window
    float originalScale = ImGui::GetIO().FontGlobalScale;

    // Scale preview height with dampened scaling
    float heightScale = 1.0f + (m_fontScale - 1.0f) * 0.65f;
    ImGui::BeginChild("FontPreview", ImVec2(-1, 120 * heightScale), ImGuiChildFlags_Borders);

    ImGui::GetIO().FontGlobalScale = m_fontScale;

    // Regular font preview
    if (Fonts::regular)
    {
        ImGui::PushFont(Fonts::regular);
        ImGui::Text("Regular: The quick brown fox jumps over the lazy dog");
        ImGui::PopFont();
    }
    else
    {
        ImGui::Text("Regular: The quick brown fox jumps over the lazy dog");
    }

    ImGui::Spacing();

    // Mono font preview
    if (Fonts::mono)
    {
        ImGui::PushFont(Fonts::mono);
        ImGui::Text("Mono: function main() { return 0; }");
        ImGui::PopFont();
    }
    else
    {
        ImGui::Text("Mono: function main() { return 0; }");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Scale: %.2fx", m_fontScale);

    // Restore original scale before ending child
    ImGui::GetIO().FontGlobalScale = originalScale;

    ImGui::EndChild();
}

void SettingsPanel::render()
{
    // Size to 90% of main viewport, centered
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 modalSize(viewport->WorkSize.x * 0.9f, viewport->WorkSize.y * 0.9f);
    ImGui::SetNextWindowSize(modalSize, ImGuiCond_Always);
    ImVec2 center(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                  viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.09f, 0.09f, 0.09f, 1.0f));
    if (!ImGui::BeginPopupModal("Settings", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
    {
        ImGui::PopStyleColor();
        return;
    }
    ImGui::PopStyleColor();

    // Reload config values when modal first opens
    if (m_needsReload)
    {
        loadFromConfig();
        m_needsReload = false;
    }

    // Scrollable content area, leaving room for buttons at the bottom
    float buttonRowHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("SettingsContent", ImVec2(0, -buttonRowHeight), ImGuiChildFlags_None);

    // --- Node Info (read-only) ---
    if (ImGui::CollapsingHeader("Node Info", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const auto& id = m_app->identity();
        ImGui::Text("Node ID:  %s", id.nodeId().c_str());
        ImGui::Text("Hostname: %s", id.systemInfo().hostname.c_str());
        ImGui::Text("CPU:      %d cores", id.systemInfo().cpuCores);
        ImGui::Text("RAM:      %llu MB", static_cast<unsigned long long>(id.systemInfo().ramMB));
        ImGui::Text("GPU:      %s", id.systemInfo().gpuName.c_str());
        ImGui::Separator();
    }

    // --- Appearance ---
    if (ImGui::CollapsingHeader("Appearance", ImGuiTreeNodeFlags_DefaultOpen))
    {
        drawFontSizeSection();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        drawFontPreview();

        ImGui::Separator();
    }

    // --- Sync Root ---
    if (ImGui::CollapsingHeader("Sync Root", ImGuiTreeNodeFlags_DefaultOpen))
    {
        float browseWidth = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browseWidth - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputText("##syncroot", m_syncRootBuf, sizeof(m_syncRootBuf));
        ImGui::SameLine();
        if (ImGui::Button("Browse..."))
        {
            nfdchar_t* outPath = nullptr;
            nfdresult_t result = NFD_PickFolder(&outPath, m_syncRootBuf[0] != '\0' ? m_syncRootBuf : nullptr);
            if (result == NFD_OKAY && outPath)
            {
                std::strncpy(m_syncRootBuf, outPath, sizeof(m_syncRootBuf) - 1);
                m_syncRootBuf[sizeof(m_syncRootBuf) - 1] = '\0';
                NFD_FreePath(outPath);
            }
        }

        // Validation + Templates button
        bool syncRootValid = false;
        if (m_syncRootBuf[0] != '\0')
        {
            syncRootValid = std::filesystem::is_directory(m_syncRootBuf);
            if (syncRootValid)
            {
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "Directory exists");
                // Show Templates button when sync root is valid and farm is running
                auto templatesDir = std::filesystem::path(m_syncRootBuf) / "SmallRender-v1" / "templates";
                if (std::filesystem::is_directory(templatesDir))
                {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Templates"))
                        openFolderInExplorer(templatesDir);
                }
            }
            else
            {
                ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "Directory not found");
            }
        }
        ImGui::Separator();
    }

    // --- Coordinator ---
    if (ImGui::CollapsingHeader("Coordinator", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("This node is the coordinator", &m_isCoordinator);
        ImGui::TextDisabled("The coordinator dispatches work to all nodes.");
        ImGui::TextDisabled("Only one node on the farm should be coordinator.");
        ImGui::Separator();
    }

    // --- Timing ---
    if (ImGui::CollapsingHeader("Timing", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const char* presets[] = { "Local / NAS", "Cloud FS", "Custom" };
        ImGui::Combo("Preset", &m_timingPreset, presets, IM_ARRAYSIZE(presets));

        if (static_cast<TimingPreset>(m_timingPreset) == TimingPreset::Custom)
        {
            ImGui::InputInt("Heartbeat (ms)", &m_heartbeatMs, 1000);
            ImGui::InputInt("Scan interval (ms)", &m_scanMs, 1000);
            ImGui::InputInt("Claim settle (ms)", &m_claimSettleMs, 1000);
            ImGui::InputInt("Dead threshold (scans)", &m_deadThresholdScans, 1);
            if (m_deadThresholdScans < 1) m_deadThresholdScans = 1;
            int deathMs = m_scanMs * m_deadThresholdScans;
            ImGui::TextDisabled("Death detection: ~%d s", deathMs / 1000);
        }
        else
        {
            auto preset = timingForPreset(static_cast<TimingPreset>(m_timingPreset));
            ImGui::Text("Heartbeat: %d ms  |  Scan: %d ms", preset.heartbeat_interval_ms, preset.scan_interval_ms);
            int deathMs = preset.scan_interval_ms * preset.dead_threshold_scans;
            ImGui::Text("Claim settle: %d ms  |  Dead threshold: %u scans (~%ds)",
                        preset.claim_settle_ms, preset.dead_threshold_scans, deathMs / 1000);
        }
        ImGui::Separator();
    }

    // --- Tags ---
    if (ImGui::CollapsingHeader("Node Tags"))
    {
        ImGui::InputText("Tags (comma-separated)", m_tagsBuf, sizeof(m_tagsBuf));
        ImGui::Separator();
    }

    // --- Agent ---
    if (ImGui::CollapsingHeader("Agent", ImGuiTreeNodeFlags_DefaultOpen))
    {
        auto& supervisor = m_app->agentSupervisor();
        bool connected = supervisor.isAgentConnected();
        bool running = supervisor.isAgentRunning();

        // Status
        if (connected)
        {
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "Connected");
            ImGui::SameLine();
            ImGui::TextDisabled("(PID %u, %s)", supervisor.agentPid(),
                supervisor.agentState().empty() ? "unknown" : supervisor.agentState().c_str());
        }
        else if (running)
        {
            ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.3f, 1.0f), "Starting...");
            ImGui::SameLine();
            ImGui::TextDisabled("(PID %u)", supervisor.agentPid());
        }
        else
        {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Disconnected");
        }

        ImGui::Spacing();

        // Controls
        if (!running)
        {
            if (ImGui::Button("Start Agent"))
            {
                supervisor.spawnAgent();
            }
        }
        else
        {
            if (ImGui::Button("Stop Agent"))
            {
                supervisor.shutdownAgent();
            }
            ImGui::SameLine();
            if (ImGui::Button("Restart Agent"))
            {
                supervisor.shutdownAgent();
                supervisor.spawnAgent();
            }
        }

        ImGui::Spacing();
        ImGui::Checkbox("Auto-start agent", &m_autoStartAgent);
        ImGui::Separator();
    }

    // --- Notifications ---
    ImGui::Checkbox("Show notifications", &m_showNotifications);

    ImGui::EndChild(); // SettingsContent

    // --- Save / Cancel (pinned to bottom) ---
    ImGui::Separator();
    if (ImGui::Button("Save", ImVec2(120, 0)))
    {
        std::string oldSyncRoot = m_savedSyncRoot;
        bool wasCoordinator = m_app->config().is_coordinator;
        applyToConfig();
        m_app->saveConfig();

        auto& cfg = m_app->config();
        bool needsRestart = (cfg.sync_root != oldSyncRoot) ||
                            (cfg.is_coordinator != wasCoordinator);

        if (needsRestart)
        {
            m_app->stopFarm();
            if (!cfg.sync_root.empty() && std::filesystem::is_directory(cfg.sync_root))
                m_app->startFarm();
        }
        else if (m_app->isFarmRunning())
        {
            // Update live timing/tags
            m_app->heartbeatManager().updateTiming(cfg.timing);
            m_app->heartbeatManager().updateTags(cfg.tags);
            if (cfg.is_coordinator)
            {
                m_app->dispatchManager().updateTiming(cfg.timing);
                m_app->dispatchManager().updateTags(cfg.tags);
            }
        }

        m_needsReload = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0)))
    {
        loadFromConfig(); // Revert
        m_needsReload = true;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

} // namespace SR
