#include "monitor/ui/style.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

#include <imgui.h>
#include <GLFW/glfw3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include <filesystem>
#include <string>

namespace SR {

// ---------------------------------------------------------------------------
// Accent color helpers
// ---------------------------------------------------------------------------

static ImVec4 getAccentColor()
{
#ifdef _WIN32
    DWORD color = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque)))
    {
        float r = ((color >> 16) & 0xff) / 255.0f;
        float g = ((color >> 8) & 0xff) / 255.0f;
        float b = (color & 0xff) / 255.0f;
        return ImVec4(r, g, b, 1.0f);
    }
#endif
    return ImVec4(0.50f, 0.45f, 0.37f, 1.0f); // warm tan fallback
}

// ---------------------------------------------------------------------------
// Font loading
// ---------------------------------------------------------------------------

static std::string getExeDir()
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path().string();
#else
    return ".";
#endif
}

void loadFonts()
{
    ImGuiIO& io = ImGui::GetIO();

    std::string fontDir = getExeDir() + "/resources/fonts/";

    io.Fonts->AddFontDefault();

    std::string interRegular = fontDir + "Inter_18pt-Regular.ttf";
    if (std::filesystem::exists(interRegular))
    {
        Fonts::regular = io.Fonts->AddFontFromFileTTF(interRegular.c_str(), 17.0f);
        Fonts::bold    = io.Fonts->AddFontFromFileTTF((fontDir + "Inter_18pt-Bold.ttf").c_str(), 17.0f);
        Fonts::italic  = io.Fonts->AddFontFromFileTTF((fontDir + "Inter_18pt-Italic.ttf").c_str(), 17.0f);
        Fonts::mono    = io.Fonts->AddFontFromFileTTF((fontDir + "JetBrainsMono-Regular.ttf").c_str(), 15.0f);

        ImFontConfig iconsCfg;
        iconsCfg.MergeMode = false;
        iconsCfg.PixelSnapH = true;
        static const ImWchar iconRanges[] = { 0xE000, 0xF8FF, 0 };
        Fonts::icons = io.Fonts->AddFontFromFileTTF(
            (fontDir + "MaterialSymbolsSharp-Regular.ttf").c_str(), 18.0f, &iconsCfg, iconRanges);

        if (Fonts::regular)
            io.FontDefault = Fonts::regular;
    }
}

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

void setupStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;
    ImVec4 accent = getAccentColor();

    // Base dark palette (from UnionPlayer/ImGuiFileBrowser)
    c[ImGuiCol_Text]                  = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.09f, 0.09f, 0.09f, 1.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.09f, 0.09f, 0.09f, 1.00f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.128f, 0.128f, 0.128f, 1.00f);
    c[ImGuiCol_Border]                = ImVec4(0.19f, 0.19f, 0.19f, 0.50f);
    c[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg]               = ImVec4(0.160f, 0.160f, 0.160f, 0.40f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.199f, 0.199f, 0.199f, 1.00f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    c[ImGuiCol_TitleBg]               = ImVec4(0.172f, 0.172f, 0.172f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.172f, 0.172f, 0.172f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.121f, 0.121f, 0.121f, 1.00f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
    c[ImGuiCol_CheckMark]             = accent;
    c[ImGuiCol_SliderGrab]            = ImVec4(0.54f, 0.54f, 0.54f, 1.00f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(0.67f, 0.67f, 0.67f, 1.00f);
    c[ImGuiCol_Button]                = ImVec4(0.28f, 0.28f, 0.28f, 0.50f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.32f, 0.32f, 0.32f, 1.00f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    c[ImGuiCol_Header]                = ImVec4(0.20f, 0.20f, 0.20f, 0.55f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.314f, 0.314f, 0.314f, 0.80f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    c[ImGuiCol_Separator]             = ImVec4(0.28f, 0.28f, 0.28f, 0.29f);
    c[ImGuiCol_SeparatorHovered]      = ImVec4(0.44f, 0.44f, 0.44f, 0.29f);
    c[ImGuiCol_SeparatorActive]       = ImVec4(0.40f, 0.44f, 0.47f, 1.00f);
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.28f, 0.28f, 0.28f, 0.29f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.44f, 0.44f, 0.44f, 0.29f);
    c[ImGuiCol_ResizeGripActive]      = ImVec4(0.40f, 0.44f, 0.47f, 1.00f);
    c[ImGuiCol_Tab]                   = ImVec4(0.172f, 0.172f, 0.172f, 1.00f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    c[ImGuiCol_TabActive]             = ImVec4(0.09f, 0.09f, 0.09f, 1.00f);
    c[ImGuiCol_TabSelectedOverline]   = ImVec4(0.09f, 0.09f, 0.09f, 0.10f);
    c[ImGuiCol_TabUnfocused]          = ImVec4(0.172f, 0.172f, 0.172f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.18f, 0.18f, 0.18f, 0.10f);
    c[ImGuiCol_DockingPreview]        = ImVec4(0.60f, 0.60f, 0.60f, 0.70f);
    c[ImGuiCol_DockingEmptyBg]        = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    c[ImGuiCol_PlotLines]             = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]      = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    c[ImGuiCol_PlotHistogram]         = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    c[ImGuiCol_PlotHistogramHovered]  = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.26f, 0.26f, 0.26f, 0.35f);
    c[ImGuiCol_DragDropTarget]        = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_NavHighlight]          = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.01f, 0.01f, 0.01f, 0.65f);
    c[ImGuiCol_TableHeaderBg]         = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    c[ImGuiCol_TableBorderStrong]     = ImVec4(0.31f, 0.31f, 0.31f, 0.20f);
    c[ImGuiCol_TableBorderLight]      = ImVec4(0.23f, 0.23f, 0.23f, 0.20f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(1.00f, 1.00f, 1.00f, 0.01f);

    // Geometry
    style.WindowPadding     = ImVec2(12.0f, 12.0f);
    style.FramePadding      = ImVec2(8.0f, 6.0f);
    style.CellPadding       = ImVec2(6.0f, 4.0f);
    style.ItemSpacing       = ImVec2(6.0f, 6.0f);
    style.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
    style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
    style.IndentSpacing     = 25.0f;
    style.ScrollbarSize     = 15.0f;
    style.GrabMinSize       = 10.0f;
    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 0.0f;
    style.PopupBorderSize   = 0.0f;
    style.FrameBorderSize   = 1.0f;
    style.TabBorderSize     = 1.0f;
    style.WindowRounding    = 0.0f;
    style.ChildRounding     = 0.0f;
    style.FrameRounding     = 4.0f;
    style.PopupRounding     = 4.0f;
    style.ScrollbarRounding = 2.0f;
    style.GrabRounding      = 3.0f;
    style.LogSliderDeadzone = 4.0f;
    style.TabRounding       = 0.0f;
}

// ---------------------------------------------------------------------------
// Windows dark title bar
// ---------------------------------------------------------------------------

void enableDarkTitleBar([[maybe_unused]] GLFWwindow* window)
{
#ifdef _WIN32
    HWND hwnd = glfwGetWin32Window(window);
    BOOL value = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value));
#endif
}

// ---------------------------------------------------------------------------
// Panel header
// ---------------------------------------------------------------------------

bool panelHeader(const char* title, bool& visible)
{
    bool closed = false;

    // Bold title
    if (Fonts::bold) ImGui::PushFont(Fonts::bold);
    ImGui::TextUnformatted(title);
    if (Fonts::bold) ImGui::PopFont();

    // Close button on the right
    float buttonSize = ImGui::GetFontSize() + 4.0f;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - buttonSize);
    ImVec2 buttonPos = ImGui::GetCursorScreenPos();

    ImGui::PushID(title);
    bool clicked = ImGui::InvisibleButton("##close", ImVec2(buttonSize, buttonSize));
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    // Draw X icon via icon font, or fallback to text
    if (Fonts::icons)
    {
        ImGui::PushFont(Fonts::icons);
        const char* closeIcon = reinterpret_cast<const char*>(u8"\uE5CD");
        ImVec2 iconSize = ImGui::CalcTextSize(closeIcon);
        ImVec2 iconPos(
            buttonPos.x + (buttonSize - iconSize.x) * 0.5f,
            buttonPos.y + (buttonSize - iconSize.y) * 0.5f);
        ImU32 col = hovered
            ? ImGui::GetColorU32(ImGuiCol_Text)
            : ImGui::GetColorU32(ImGuiCol_TextDisabled);
        ImGui::GetWindowDrawList()->AddText(iconPos, col, closeIcon);
        ImGui::PopFont();
    }
    else
    {
        // Fallback: draw "x" with regular font
        const char* x = "x";
        ImVec2 xSize = ImGui::CalcTextSize(x);
        ImVec2 xPos(
            buttonPos.x + (buttonSize - xSize.x) * 0.5f,
            buttonPos.y + (buttonSize - xSize.y) * 0.5f);
        ImU32 col = hovered
            ? ImGui::GetColorU32(ImGuiCol_Text)
            : ImGui::GetColorU32(ImGuiCol_TextDisabled);
        ImGui::GetWindowDrawList()->AddText(xPos, col, x);
    }

    if (clicked)
    {
        visible = false;
        closed = true;
    }

    ImGui::Separator();
    return closed;
}

} // namespace SR
