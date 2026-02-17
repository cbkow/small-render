#include "monitor/monitor_app.h"
#include "monitor/ui/style.h"
#include "core/system_tray.h"
#include "core/single_instance.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <nfd.h>

#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <cstdlib>
#include <crtdbg.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

static void invalidParameterHandler(
    const wchar_t* expression, const wchar_t* function,
    const wchar_t* file, unsigned int line, uintptr_t /*reserved*/)
{
    // Log to stderr since MonitorLog may not be initialized
    std::wcerr << L"[CRT] Invalid parameter: " << (expression ? expression : L"(null)")
               << L" in " << (function ? function : L"(null)")
               << L" at " << (file ? file : L"(null)")
               << L":" << line << std::endl;
}
#endif

static void glfwErrorCallback(int error, const char* description)
{
    std::cerr << "[GLFW] Error " << error << ": " << description << std::endl;
}

int main(int argc, char* argv[])
{
#ifdef _WIN32
    // Redirect CRT invalid parameter errors to our handler instead of abort()
    _set_invalid_parameter_handler(invalidParameterHandler);
    // In debug mode, also disable the CRT assertion popup for invalid params
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
#endif

    // Parse CLI flags
    bool startMinimized = false;
    bool cliSubmit = false;
    std::string cliFile;
    std::string cliTemplate;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--minimized") == 0)
            startMinimized = true;
        else if (std::strcmp(argv[i], "--submit") == 0)
            cliSubmit = true;
        else if (std::strcmp(argv[i], "--file") == 0 && i + 1 < argc)
            cliFile = argv[++i];
        else if (std::strcmp(argv[i], "--template") == 0 && i + 1 < argc)
            cliTemplate = argv[++i];
    }

    // --- Single Instance Check (before GLFW init) ---
    SR::SingleInstance singleInstance("SmallRenderMonitor");
    if (!singleInstance.isFirst())
    {
        if (cliSubmit && !cliFile.empty())
        {
            singleInstance.sendSubmitRequest(cliFile, cliTemplate);
            return 0; // Delivered to existing instance
        }
        else
        {
            singleInstance.signalExisting();
            return 0; // Brought existing window to front
        }
    }

    // --- GLFW init ---
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit())
    {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return 1;
    }

    // GL 3.3 Core
    const char* glslVersion = "#version 330";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // Start hidden if --minimized, show after setup
    if (startMinimized)
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    std::string windowTitle = std::string("SmallRender Monitor v") + APP_VERSION;
    GLFWwindow* window = glfwCreateWindow(1280, 720, windowTitle.c_str(), nullptr, nullptr);
    if (!window)
    {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // VSync

    // --- GLAD ---
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress)))
    {
        std::cerr << "Failed to initialize OpenGL loader" << std::endl;
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // --- ImGui ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.IniFilename = nullptr; // We manage layout ourselves

    // Fonts and theme
    SR::loadFonts();
    SR::setupStyle();

    // When viewports are enabled, tweak style so platform windows match
    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    // Dark title bar on Windows
    SR::enableDarkTitleBar(window);

    // Set window icon from embedded resource
#ifdef _WIN32
    {
        HICON iconBig = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        if (iconBig)
        {
            HWND hwnd = glfwGetWin32Window(window);
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(iconBig));
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(iconBig));
        }
    }
#endif

    // --- NFD ---
    NFD_Init();

    // --- App ---
    SR::MonitorApp app;
    if (!app.init())
    {
        std::cerr << "Failed to initialize MonitorApp" << std::endl;
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // If launched with --submit as first instance, store the request for
    // the app to process once the farm is running
    if (cliSubmit && !cliFile.empty())
    {
        app.setPendingSubmitRequest(cliFile, cliTemplate);
    }

    // --- X button hides window, doesn't close process ---
    glfwSetWindowCloseCallback(window, [](GLFWwindow* w) {
        glfwSetWindowShouldClose(w, GLFW_FALSE);
        glfwHideWindow(w);
    });

    // --- System tray ---
    SR::SystemTray tray;
    tray.init();

    tray.onShowWindow = [&]() {
        glfwShowWindow(window);
        glfwFocusWindow(window);
    };

    tray.onStopResume = [&]() {
        if (app.nodeState() == SR::NodeState::Active)
            app.setNodeState(SR::NodeState::Stopped);
        else
            app.setNodeState(SR::NodeState::Active);
    };

    tray.onExit = [&]() {
        app.requestExit();
    };

    // --- Main loop — exit controlled by app, not GLFW ---
    while (!app.shouldExit())
    {
        glfwPollEvents();
        app.update();

        bool visible = glfwGetWindowAttrib(window, GLFW_VISIBLE) != 0;

        // Auto-show window when exit dialog needs to display
        if (app.isExitPending() && !visible)
        {
            glfwShowWindow(window);
            glfwFocusWindow(window);
            visible = true;
        }

        // Update tray icon
        tray.setIcon(app.trayState());
        tray.setTooltip(app.trayTooltip());
        tray.setStatusText(app.trayStatusText());
        tray.setNodeActive(app.nodeState() == SR::NodeState::Active);

        if (visible)
        {
            // Full ImGui frame + render + swap
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            app.renderUI();

            ImGui::Render();
            int displayW, displayH;
            glfwGetFramebufferSize(window, &displayW, &displayH);
            glViewport(0, 0, displayW, displayH);
            glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            // Update additional platform windows
            if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
            {
                GLFWwindow* backupCtx = glfwGetCurrentContext();
                ImGui::UpdatePlatformWindows();
                ImGui::RenderPlatformWindowsDefault();
                glfwMakeContextCurrent(backupCtx);
            }

            glfwSwapBuffers(window);
        }
        else
        {
            // Window hidden — sleep to save CPU, background threads keep running
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // --- Cleanup ---
    tray.shutdown();
    app.shutdown();
    NFD_Quit();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
