#pragma once

#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace SR {

class SingleInstance
{
public:
    explicit SingleInstance(const std::string& name);
    ~SingleInstance();

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    // Returns true if this is the first instance (no existing monitor running)
    bool isFirst() const;

    // Signal existing instance to show its window (via tray HWND message)
    void signalExisting();

    // Write a submit request file for the existing instance to pick up,
    // then signal it to show window.
    void sendSubmitRequest(const std::string& file, const std::string& templateId);

private:
#ifdef _WIN32
    HANDLE m_mutex = nullptr;
    bool m_isFirst = false;
#endif
};

} // namespace SR
