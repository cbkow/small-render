#include "core/atomic_file_io.h"

#include <fstream>
#include <iostream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace SR {

bool AtomicFileIO::writeJson(const std::filesystem::path& path, const nlohmann::json& data)
{
    auto tmpPath = path;
    tmpPath += ".tmp";

    try
    {
        std::ofstream file(tmpPath);
        if (!file.is_open())
        {
            std::cerr << "[AtomicFileIO] Failed to open temp file: " << tmpPath << std::endl;
            return false;
        }

        file << data.dump(2);
        file.flush();

        if (!file.good())
        {
            std::cerr << "[AtomicFileIO] Write failed for: " << tmpPath << std::endl;
            file.close();
            return false;
        }
        file.close();

        flushToDisk(tmpPath);
        std::filesystem::rename(tmpPath, path);
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[AtomicFileIO] writeJson error: " << e.what() << std::endl;
        // Clean up temp file on failure
        std::error_code ec;
        std::filesystem::remove(tmpPath, ec);
        return false;
    }
}

std::optional<nlohmann::json> AtomicFileIO::safeReadJson(const std::filesystem::path& path)
{
    try
    {
        if (!std::filesystem::exists(path))
            return std::nullopt;

        std::ifstream file(path);
        if (!file.is_open())
            return std::nullopt;

        nlohmann::json data = nlohmann::json::parse(file);
        return data;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[AtomicFileIO] safeReadJson error for " << path << ": " << e.what() << std::endl;
        return std::nullopt;
    }
}

bool AtomicFileIO::writeText(const std::filesystem::path& path, const std::string& content)
{
    auto tmpPath = path;
    tmpPath += ".tmp";

    try
    {
        std::ofstream file(tmpPath);
        if (!file.is_open())
        {
            std::cerr << "[AtomicFileIO] Failed to open temp file: " << tmpPath << std::endl;
            return false;
        }

        file << content;
        file.flush();

        if (!file.good())
        {
            std::cerr << "[AtomicFileIO] Write failed for: " << tmpPath << std::endl;
            file.close();
            return false;
        }
        file.close();

        flushToDisk(tmpPath);
        std::filesystem::rename(tmpPath, path);
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[AtomicFileIO] writeText error: " << e.what() << std::endl;
        std::error_code ec;
        std::filesystem::remove(tmpPath, ec);
        return false;
    }
}

std::optional<std::string> AtomicFileIO::safeReadText(const std::filesystem::path& path)
{
    try
    {
        if (!std::filesystem::exists(path))
            return std::nullopt;

        std::ifstream file(path);
        if (!file.is_open())
            return std::nullopt;

        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        return content;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[AtomicFileIO] safeReadText error for " << path << ": " << e.what() << std::endl;
        return std::nullopt;
    }
}

void AtomicFileIO::flushToDisk(const std::filesystem::path& path)
{
#ifdef _WIN32
    HANDLE hFile = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );

    if (hFile != INVALID_HANDLE_VALUE)
    {
        if (!FlushFileBuffers(hFile))
        {
            std::cerr << "[AtomicFileIO] Warning: FlushFileBuffers failed" << std::endl;
        }
        CloseHandle(hFile);
    }
#endif
    // On Linux/macOS, fsync would go here
}

} // namespace SR
