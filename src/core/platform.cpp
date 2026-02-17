#include "core/platform.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>
#endif

#include <iostream>

namespace SR {

std::filesystem::path getAppDataDir()
{
#ifdef _WIN32
    wchar_t* appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData)))
    {
        std::filesystem::path dir = std::filesystem::path(appData) / L"SmallRender";
        CoTaskMemFree(appData);
        ensureDir(dir);
        return dir;
    }
    CoTaskMemFree(appData);
#endif
    // Fallback
    auto dir = std::filesystem::current_path() / "SmallRender_data";
    ensureDir(dir);
    return dir;
}

bool ensureDir(const std::filesystem::path& path)
{
    try
    {
        if (!std::filesystem::exists(path))
        {
            return std::filesystem::create_directories(path);
        }
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Platform] Failed to create directory: " << e.what() << std::endl;
        return false;
    }
}

std::string getOS()
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

std::string getHostname()
{
#ifdef _WIN32
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(buf) / sizeof(buf[0]);
    if (GetComputerNameW(buf, &size))
    {
        // Convert wide to narrow via WideCharToMultiByte
        int len = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(size), nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(size), result.data(), len, nullptr, nullptr);
        return result;
    }
#endif
    return "unknown";
}

void openFolderInExplorer(const std::filesystem::path& folder)
{
#ifdef _WIN32
    ShellExecuteW(nullptr, L"explore", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = "open \"" + folder.string() + "\"";
    std::system(cmd.c_str());
#elif defined(__linux__)
    std::string cmd = "xdg-open \"" + folder.string() + "\"";
    std::system(cmd.c_str());
#endif
}

} // namespace SR
