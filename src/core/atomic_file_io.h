#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace SR {

class AtomicFileIO
{
public:
    // Write JSON atomically: serialize → write .tmp → flush → rename
    static bool writeJson(const std::filesystem::path& path, const nlohmann::json& data);

    // Read and parse JSON. Returns nullopt on missing file or parse error.
    static std::optional<nlohmann::json> safeReadJson(const std::filesystem::path& path);

    // Write plain text atomically: write .tmp → flush → rename
    static bool writeText(const std::filesystem::path& path, const std::string& content);

    // Read plain text. Returns nullopt on missing file or read error.
    static std::optional<std::string> safeReadText(const std::filesystem::path& path);

private:
    // Flush file buffers to disk (Windows: FlushFileBuffers)
    static void flushToDisk(const std::filesystem::path& path);
};

} // namespace SR
