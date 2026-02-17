#include "core/system_tray.h"

#ifdef _WIN32

#include <cstring>
#include <filesystem>

namespace SR {

// Tray icon filenames — order must match TrayIconState enum
static const wchar_t* s_trayIconFiles[5] = {
    L"resources\\icons\\tray_green.ico",   // Green
    L"resources\\icons\\tray_blue.ico",    // Blue
    L"resources\\icons\\tray_yellow.ico",  // Yellow
    L"resources\\icons\\tray_red.ico",     // Red
    L"resources\\icons\\tray_grey.ico"     // Gray
};

SystemTray::~SystemTray()
{
    if (m_initialized)
        shutdown();
}

bool SystemTray::init()
{
    // Register window class
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SmallRenderTray";
    RegisterClassW(&wc);

    // Create message-only window
    m_hwnd = CreateWindowExW(
        0, L"SmallRenderTray", L"SmallRender Tray",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!m_hwnd)
        return false;

    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    // Create GDI circle icons
    createIcons();

    // Add tray icon
    m_nid.cbSize = sizeof(NOTIFYICONDATAW);
    m_nid.hWnd = m_hwnd;
    m_nid.uID = 1;
    m_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    m_nid.uCallbackMessage = WM_TRAYICON;
    m_nid.hIcon = m_icons[static_cast<int>(TrayIconState::Gray)];
    wcscpy_s(m_nid.szTip, L"SmallRender");

    Shell_NotifyIconW(NIM_ADD, &m_nid);

    m_initialized = true;
    return true;
}

void SystemTray::shutdown()
{
    if (!m_initialized)
        return;

    Shell_NotifyIconW(NIM_DELETE, &m_nid);
    destroyIcons();

    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }

    UnregisterClassW(L"SmallRenderTray", GetModuleHandleW(nullptr));
    m_initialized = false;
}

void SystemTray::setIcon(TrayIconState state)
{
    if (!m_initialized || state == m_currentState)
        return;

    m_currentState = state;
    m_nid.hIcon = m_icons[static_cast<int>(state)];
    m_nid.uFlags = NIF_ICON;
    Shell_NotifyIconW(NIM_MODIFY, &m_nid);
}

void SystemTray::setTooltip(const std::string& text)
{
    if (!m_initialized)
        return;

    // Convert UTF-8 to wide
    wchar_t wide[128] = {};
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide, 127);
    wcscpy_s(m_nid.szTip, wide);
    m_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &m_nid);
}

void SystemTray::setStatusText(const std::string& text)
{
    m_statusText = text;
}

void SystemTray::setNodeActive(bool active)
{
    m_nodeActive = active;
}

void SystemTray::createIcons()
{
    // Resolve exe directory for icon file paths
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();

    for (int i = 0; i < 5; ++i)
    {
        auto icoPath = exeDir / s_trayIconFiles[i];
        m_icons[i] = static_cast<HICON>(LoadImageW(
            nullptr, icoPath.wstring().c_str(),
            IMAGE_ICON, 16, 16, LR_LOADFROMFILE));

        // Fallback: create a simple colored square if .ico not found
        if (!m_icons[i])
        {
            static const COLORREF fallbackColors[5] = {
                RGB(77, 204, 77), RGB(77, 128, 230), RGB(230, 200, 50),
                RGB(230, 77, 77), RGB(140, 140, 140)
            };

            constexpr int SIZE = 16;
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = SIZE;
            bmi.bmiHeader.biHeight = SIZE;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            uint32_t* bits = nullptr;
            HDC dc = GetDC(nullptr);
            HBITMAP colorBmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS,
                reinterpret_cast<void**>(&bits), nullptr, 0);
            HBITMAP maskBmp = CreateBitmap(SIZE, SIZE, 1, 1, nullptr);

            if (bits)
            {
                BYTE r = GetRValue(fallbackColors[i]);
                BYTE g = GetGValue(fallbackColors[i]);
                BYTE b = GetBValue(fallbackColors[i]);
                for (int p = 0; p < SIZE * SIZE; ++p)
                    bits[p] = (255u << 24) | (r << 16) | (g << 8) | b;
            }

            ICONINFO ii = {};
            ii.fIcon = TRUE;
            ii.hbmMask = maskBmp;
            ii.hbmColor = colorBmp;
            m_icons[i] = CreateIconIndirect(&ii);

            DeleteObject(colorBmp);
            DeleteObject(maskBmp);
            ReleaseDC(nullptr, dc);
        }
    }
}

void SystemTray::destroyIcons()
{
    for (int i = 0; i < 5; ++i)
    {
        if (m_icons[i])
        {
            DestroyIcon(m_icons[i]);
            m_icons[i] = nullptr;
        }
    }
}

void SystemTray::showContextMenu()
{
    HMENU menu = CreatePopupMenu();

    // Title (disabled)
    AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, L"SmallRender");

    // Status line (disabled)
    wchar_t statusWide[128] = {};
    std::string statusLine = "Status: " + m_statusText;
    MultiByteToWideChar(CP_UTF8, 0, statusLine.c_str(), -1, statusWide, 127);
    AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, statusWide);

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, IDM_SHOW, L"Show Window");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    if (m_nodeActive)
        AppendMenuW(menu, MF_STRING, IDM_TOGGLE, L"Stop Node");
    else
        AppendMenuW(menu, MF_STRING, IDM_TOGGLE, L"Resume Node");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    // Required Win32 workaround: SetForegroundWindow before TrackPopupMenu
    SetForegroundWindow(m_hwnd);

    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, m_hwnd, nullptr);

    // Required Win32 workaround: post dummy message after TrackPopupMenu
    PostMessageW(m_hwnd, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

LRESULT CALLBACK SystemTray::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* self = reinterpret_cast<SystemTray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_TRAYICON:
        if (self)
        {
            switch (LOWORD(lParam))
            {
            case WM_LBUTTONDBLCLK:
                if (self->onShowWindow)
                    self->onShowWindow();
                break;
            case WM_RBUTTONUP:
                self->showContextMenu();
                break;
            }
        }
        return 0;

    case WM_SHOW_WINDOW:
        // Posted by SingleInstance from a second process
        if (self && self->onShowWindow)
            self->onShowWindow();
        return 0;

    case WM_COMMAND:
        if (self)
        {
            switch (LOWORD(wParam))
            {
            case IDM_SHOW:
                if (self->onShowWindow)
                    self->onShowWindow();
                break;
            case IDM_TOGGLE:
                if (self->onStopResume)
                    self->onStopResume();
                break;
            case IDM_EXIT:
                if (self->onExit)
                    self->onExit();
                break;
            }
        }
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace SR

#else

// Stub for non-Windows — tray icon not yet implemented
namespace SR {

SystemTray::~SystemTray() {}
bool SystemTray::init() { return true; }
void SystemTray::shutdown() {}
void SystemTray::setIcon(TrayIconState) {}
void SystemTray::setTooltip(const std::string&) {}
void SystemTray::setStatusText(const std::string&) {}
void SystemTray::setNodeActive(bool) {}

} // namespace SR

#endif
