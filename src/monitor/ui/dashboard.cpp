#include "monitor/ui/dashboard.h"
#include "monitor/monitor_app.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace SR {

void Dashboard::init(MonitorApp* app)
{
    m_app = app;
    m_settingsPanel.init(app);
    m_nodePanel.init(app);
    m_jobListPanel.init(app);
    m_jobDetailPanel.init(app);
    m_logPanel.init(app);
    m_farmCleanupDialog.init(app);
}

void Dashboard::render()
{
    // Fullscreen dockspace host window
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_MenuBar |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("##DockspaceHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    // Menu bar
    renderMenuBar();

    // Dockspace
    ImGuiID dockspaceId = ImGui::GetID("SmallRenderDockspace");
    ImGuiDockNodeFlags dockFlags =
        ImGuiDockNodeFlags_NoTabBar |
        ImGuiDockNodeFlags_NoUndocking |
        ImGuiDockNodeFlags_NoDockingSplit;
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), dockFlags);

    // Build default layout on first frame
    if (m_firstFrame)
    {
        buildDefaultLayout();
        m_firstFrame = false;
    }

    ImGui::End(); // DockspaceHost

    // Render panels
    m_nodePanel.render();
    m_jobDetailPanel.render();
    m_jobListPanel.render();
    m_logPanel.render();

    // Farm Cleanup dialog (modal)
    m_farmCleanupDialog.render();

    // Settings modal
    if (m_showSettings)
    {
        ImGui::OpenPopup("Settings");
        m_showSettings = false; // OpenPopup is a one-shot call
    }
    m_settingsPanel.render();

    // Exit confirmation dialog
    if (m_app->isExitPending() && m_app->renderCoordinator().isRendering())
    {
        ImGui::OpenPopup("Confirm Exit");
    }

    if (ImGui::BeginPopupModal("Confirm Exit", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Node is currently rendering %s of %s. Kill render and exit?",
                     m_app->renderCoordinator().currentChunkLabel().c_str(),
                     m_app->renderCoordinator().currentJobId().c_str());
        ImGui::Spacing();

        if (ImGui::Button("Kill && Exit"))
        {
            m_app->beginForceExit();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            m_app->cancelExit();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void Dashboard::renderMenuBar()
{
    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Settings"))
                m_showSettings = true;
            if (ImGui::MenuItem("Farm Cleanup...", nullptr, false, m_app && m_app->isFarmRunning()))
                m_farmCleanupDialog.open();
            ImGui::Separator();
            if (ImGui::MenuItem("Exit"))
            {
                m_app->requestExit();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Jobs"))
        {
            if (ImGui::MenuItem("New Job", nullptr, false, m_app && m_app->isFarmRunning()))
                m_app->requestSubmissionMode();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem("Node Overview", nullptr, &m_nodePanel.visible);
            ImGui::MenuItem("Job Detail",    nullptr, &m_jobDetailPanel.visible);
            ImGui::MenuItem("Job List",      nullptr, &m_jobListPanel.visible);
            ImGui::MenuItem("Log",           nullptr, &m_logPanel.visible);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

void Dashboard::buildDefaultLayout()
{
    ImGuiID dockspaceId = ImGui::GetID("SmallRenderDockspace");

    // Clear any existing layout
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    // Left: Nodes (20%, full height)
    ImGuiID leftId, remainId;
    ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.20f, &leftId, &remainId);

    // Remaining: top Job List (33%) | bottom content (67%)
    ImGuiID topId, bottomId;
    ImGui::DockBuilderSplitNode(remainId, ImGuiDir_Up, 0.33f, &topId, &bottomId);

    // Bottom content: Job Detail (left, 50%) | Log (right, 50%)
    ImGuiID bottomLeftId, bottomRightId;
    ImGui::DockBuilderSplitNode(bottomId, ImGuiDir_Left, 0.50f, &bottomLeftId, &bottomRightId);

    // Dock windows
    ImGui::DockBuilderDockWindow("Node Overview", leftId);
    ImGui::DockBuilderDockWindow("Job List",      topId);
    ImGui::DockBuilderDockWindow("Job Detail",    bottomLeftId);
    ImGui::DockBuilderDockWindow("Log",           bottomRightId);

    ImGui::DockBuilderFinish(dockspaceId);
}

} // namespace SR
