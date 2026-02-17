#include "monitor/ui/node_panel.h"
#include "monitor/ui/style.h"
#include "monitor/monitor_app.h"

#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace SR {

void NodePanel::init(MonitorApp* app)
{
    m_app = app;
}

void NodePanel::render()
{
    if (!visible) return;

    if (ImGui::Begin("Node Overview", nullptr, ImGuiWindowFlags_NoTitleBar))
    {
        panelHeader("Nodes", visible);
        if (!m_app || !m_app->isFarmRunning())
        {
            ImGui::TextDisabled("Farm not connected.");
            ImGui::TextDisabled("Configure Sync Root in Settings.");

            if (m_app && m_app->hasFarmError())
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "Error: %s",
                                   m_app->farmError().c_str());
            }
        }
        else
        {
            renderLocalNode();
            ImGui::Separator();
            ImGui::Spacing();
            renderPeerList();
        }
    }
    ImGui::End();
}

void NodePanel::renderLocalNode()
{
    auto nodes = m_app->heartbeatManager().getNodeSnapshot();

    // Find local node
    const NodeInfo* local = nullptr;
    for (const auto& n : nodes)
    {
        if (n.isLocal)
        {
            local = &n;
            break;
        }
    }

    ImGui::PushTextWrapPos(0.0f);

    ImGui::TextUnformatted("This Node");
    ImGui::Spacing();

    if (!local)
    {
        ImGui::TextDisabled("Waiting for first heartbeat...");
        ImGui::PopTextWrapPos();
        return;
    }

    const auto& hb = local->heartbeat;

    ImGui::Text("ID: %s", hb.node_id.c_str());
    ImGui::Text("Host: %s", hb.hostname.c_str());

    // State badges
    ImGui::Text("State: ");
    ImGui::SameLine(0, 0);
    if (hb.render_state == "rendering")
        ImGui::TextColored(ImVec4(0.3f, 0.5f, 0.9f, 1.0f), "[Rendering]");
    else
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "[Idle]");

    if (hb.is_coordinator)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.0f, 1.0f), "[Coordinator]");
    }

    if (hb.render_state == "rendering" && !hb.active_job.empty())
    {
        ImGui::Text("Job: %s  Chunk: %s", hb.active_job.c_str(),
                     hb.active_frames.empty() ? "?" : hb.active_frames.c_str());
    }

    if (!hb.gpu_name.empty())
        ImGui::Text("GPU: %s", hb.gpu_name.c_str());
    if (hb.cpu_cores > 0)
        ImGui::Text("CPU: %d cores  |  RAM: %llu GB", hb.cpu_cores,
                     static_cast<unsigned long long>(hb.ram_gb));

    // Clock skew warning
    if (m_app->heartbeatManager().hasLocalClockSkew())
    {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f),
                           "WARNING: Clock skew detected vs majority of peers!");
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f),
                           "Check this machine's system clock.");
    }

    // Node control
    ImGui::Spacing();
    ImGui::SeparatorText("Node Control");

    auto nodeState = m_app->nodeState();
    if (nodeState == NodeState::Active)
    {
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "Active");
        ImGui::SameLine();
        if (ImGui::Button("Stop Node"))
            m_app->setNodeState(NodeState::Stopped);
    }
    else // Stopped
    {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Stopped");
        ImGui::SameLine();
        if (ImGui::Button("Start Node"))
            m_app->setNodeState(NodeState::Active);
    }

    ImGui::PopTextWrapPos();
}

void NodePanel::renderPeerList()
{
    auto nodes = m_app->heartbeatManager().getNodeSnapshot();

    // Separate peers from local
    std::vector<const NodeInfo*> peers;
    for (const auto& n : nodes)
    {
        if (!n.isLocal)
            peers.push_back(&n);
    }

    ImGui::PushTextWrapPos(0.0f);

    ImGui::TextUnformatted("Peers");
    ImGui::Spacing();

    if (peers.empty())
    {
        ImGui::TextDisabled("No peers detected yet.");
        ImGui::PopTextWrapPos();
        return;
    }

    // Sort: active first, stopped second, dead last; alphabetical within each group
    auto sortKey = [](const NodeInfo* n) -> int {
        if (n->isDead) return 2;
        if (n->heartbeat.node_state == "stopped") return 1;
        return 0;
    };
    std::sort(peers.begin(), peers.end(), [&sortKey](const NodeInfo* a, const NodeInfo* b)
    {
        int ka = sortKey(a), kb = sortKey(b);
        if (ka != kb) return ka < kb;
        return a->heartbeat.hostname < b->heartbeat.hostname;
    });

    for (int peerIdx = 0; peerIdx < (int)peers.size(); ++peerIdx)
    {
        if (peerIdx > 0)
            ImGui::Separator();

        const auto* peer = peers[peerIdx];
        const auto& hb = peer->heartbeat;
        ImGui::PushID(hb.node_id.c_str());

        // Status indicator
        if (peer->isDead)
        {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "[Dead]");
        }
        else if (hb.node_state == "stopped")
        {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "[Stopped]");
        }
        else if (hb.render_state == "rendering")
        {
            ImGui::TextColored(ImVec4(0.3f, 0.5f, 0.9f, 1.0f), "[Rendering]");
        }
        else
        {
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "[Idle]");
        }

        ImGui::SameLine();

        // Hostname
        if (peer->isDead)
            ImGui::TextDisabled("%s", hb.hostname.c_str());
        else
            ImGui::TextUnformatted(hb.hostname.c_str());

        if (hb.is_coordinator)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.0f, 1.0f), "[Coordinator]");
        }

        if (peer->hasUdpContact)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "[UDP]");
        }

        // Hardware summary + version
        if (!hb.app_version.empty())
        {
            ImGui::TextDisabled("v%s | %s | %d cores | %llu GB | %s",
                                hb.app_version.c_str(), hb.os.c_str(), hb.cpu_cores,
                                static_cast<unsigned long long>(hb.ram_gb),
                                hb.gpu_name.c_str());
        }
        else
        {
            ImGui::TextDisabled("%s | %d cores | %llu GB | %s",
                                hb.os.c_str(), hb.cpu_cores,
                                static_cast<unsigned long long>(hb.ram_gb),
                                hb.gpu_name.c_str());
        }

        // Active job
        if (!peer->isDead && hb.render_state == "rendering" && !hb.active_job.empty())
        {
            ImGui::TextDisabled("Job: %s  Chunk: %s", hb.active_job.c_str(),
                                hb.active_frames.empty() ? "?" : hb.active_frames.c_str());
        }

        // Remote control buttons (alive peers only)
        if (!peer->isDead)
        {
            if (hb.node_state == "stopped")
            {
                if (ImGui::SmallButton("Start"))
                    m_app->commandManager().sendCommand(hb.node_id, "resume_all", "", "remote_request");
            }
            else
            {
                if (ImGui::SmallButton("Stop"))
                    m_app->commandManager().sendCommand(hb.node_id, "stop_all", "", "remote_request");
            }
        }

        // Clock skew warning (only meaningful for alive nodes)
        if (!peer->isDead && peer->clockSkewWarning)
        {
            double skewSec = std::abs(peer->skewAmountMs) / 1000.0;
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f),
                               "Clock skew: %.1fs", skewSec);
        }

        ImGui::Spacing();
        ImGui::PopID();
    }

    ImGui::PopTextWrapPos();
}

} // namespace SR
