#include "core/single_instance.h"
#include "core/platform.h"
#include "core/atomic_file_io.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>

namespace SR {

#ifdef _WIN32

SingleInstance::SingleInstance(const std::string& name)
{
    // Create a named mutex. If it already exists, GetLastError() == ERROR_ALREADY_EXISTS.
    std::wstring wideName(name.begin(), name.end());
    m_mutex = CreateMutexW(nullptr, FALSE, wideName.c_str());
    m_isFirst = (GetLastError() != ERROR_ALREADY_EXISTS);
}

SingleInstance::~SingleInstance()
{
    if (m_mutex)
    {
        ReleaseMutex(m_mutex);
        CloseHandle(m_mutex);
        m_mutex = nullptr;
    }
}

bool SingleInstance::isFirst() const
{
    return m_isFirst;
}

void SingleInstance::signalExisting()
{
    // Find the tray's message-only HWND and post a custom message
    // The tray window class is "SmallRenderTray", parented to HWND_MESSAGE
    HWND hwnd = FindWindowExW(HWND_MESSAGE, nullptr, L"SmallRenderTray", nullptr);
    if (hwnd)
    {
        // WM_APP + 2 = "show window" signal
        PostMessageW(hwnd, WM_APP + 2, 0, 0);
    }
}

void SingleInstance::sendSubmitRequest(const std::string& file, const std::string& templateId)
{
    auto appData = getAppDataDir();
    auto requestPath = appData / "submit_request.json";

    nlohmann::json j;
    j["file"] = file;
    if (!templateId.empty())
        j["template_id"] = templateId;

    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    j["timestamp_ms"] = nowMs;

    AtomicFileIO::writeJson(requestPath, j);
    signalExisting();
}

#else

// Stub for non-Windows — implement via pidfile + flock in v2
SingleInstance::SingleInstance(const std::string& /*name*/) {}
SingleInstance::~SingleInstance() {}
bool SingleInstance::isFirst() const { return true; }
void SingleInstance::signalExisting() {}
void SingleInstance::sendSubmitRequest(const std::string& /*file*/, const std::string& /*templateId*/) {}

#endif

} // namespace SR
