#pragma once

#include <filesystem>
#include <string>

namespace SR {

// Returns platform app data directory: %LOCALAPPDATA%\SmallRender\ on Windows
std::filesystem::path getAppDataDir();

// Creates directory tree if it doesn't exist. Returns true on success.
bool ensureDir(const std::filesystem::path& path);

// Returns "windows", "linux", or "macos"
std::string getOS();

// Returns machine hostname
std::string getHostname();

// Opens a folder in the platform file manager (Explorer, Finder, etc.)
void openFolderInExplorer(const std::filesystem::path& folder);

} // namespace SR
