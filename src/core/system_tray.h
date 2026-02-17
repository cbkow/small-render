#pragma once

#include <functional>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#endif

namespace SR {

enum class TrayIconState { Green, Blue, Yellow, Red, Gray };

class SystemTray
{
public:
    SystemTray() = default;
    ~SystemTray();

    SystemTray(const SystemTray&) = delete;
    SystemTray& operator=(const SystemTray&) = delete;

    bool init();
    void shutdown();

    void setIcon(TrayIconState state);
    void setTooltip(const std::string& text);
    void setStatusText(const std::string& text);
    void setNodeActive(bool active);

    // Callbacks set by main.cpp
    std::function<void()> onShowWindow;
    std::function<void()> onStopResume;
    std::function<void()> onExit;

private:
#ifdef _WIN32
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void createIcons();
    void destroyIcons();
    void showContextMenu();

    HWND m_hwnd = nullptr;
    NOTIFYICONDATAW m_nid = {};
    HICON m_icons[5] = {}; // Green, Blue, Yellow, Red, Gray
    TrayIconState m_currentState = TrayIconState::Gray;
    std::string m_statusText = "Initializing";
    bool m_nodeActive = false;
    bool m_initialized = false;

    static constexpr UINT WM_TRAYICON     = WM_APP + 1;
    static constexpr UINT WM_SHOW_WINDOW  = WM_APP + 2; // Posted by SingleInstance
    static constexpr UINT IDM_SHOW    = 1001;
    static constexpr UINT IDM_TOGGLE  = 1002;
    static constexpr UINT IDM_EXIT    = 1003;
#endif
};

} // namespace SR
