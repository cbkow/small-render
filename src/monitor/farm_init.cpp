#include "monitor/farm_init.h"
#include "core/config.h"
#include "core/atomic_file_io.h"
#include "core/platform.h"
#include "core/monitor_log.h"

#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace SR {

namespace fs = std::filesystem;

static std::string getExeDir()
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path().string();
#else
    return ".";
#endif
}

static fs::path findBundledTemplatesDir()
{
    fs::path templateDir = fs::path(getExeDir()) / "resources" / "templates";
    if (fs::is_directory(templateDir))
        return templateDir;
    return {};
}

static void copyExampleTemplates(const fs::path& farmPath)
{
    auto bundled = findBundledTemplatesDir();
    if (bundled.empty())
    {
        MonitorLog::instance().warn("farm", "No bundled templates found, skipping example copy");
        return;
    }

    auto destDir = farmPath / "templates" / "examples";
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(bundled, ec))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".json")
        {
            auto dest = destDir / entry.path().filename();
            fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing, ec);
            if (!ec)
                MonitorLog::instance().info("farm", "Copied template: " + entry.path().filename().string());
        }
    }
}

FarmInit::Result FarmInit::init(const fs::path& syncRoot, const std::string& nodeId)
{
    Result result;
    std::error_code ec;

    // Validate sync root
    if (!fs::is_directory(syncRoot, ec))
    {
        result.error = "Sync root is not a valid directory: " + syncRoot.string();
        return result;
    }

    fs::path farmPath = syncRoot / "SmallRender-v1";
    result.farmPath = farmPath;

    bool firstNode = !fs::exists(farmPath, ec);

    if (firstNode)
    {
        MonitorLog::instance().info("farm", "Creating farm structure at: " + farmPath.string());

        // Create full directory structure
        fs::create_directories(farmPath / "nodes", ec);
        fs::create_directories(farmPath / "jobs", ec);
        fs::create_directories(farmPath / "commands", ec);
        fs::create_directories(farmPath / "templates" / "examples", ec);
        fs::create_directories(farmPath / "submissions" / "processed", ec);

        // Write farm.json
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        nlohmann::json farmJson = {
            {"_version", 1},
            {"protocol_version", PROTOCOL_VERSION},
            {"created_by", nodeId},
            {"created_at_ms", nowMs},
            {"last_example_update", APP_VERSION}
        };
        AtomicFileIO::writeJson(farmPath / "farm.json", farmJson);

        // Copy bundled example templates
        copyExampleTemplates(farmPath);

        MonitorLog::instance().info("farm", "Farm created successfully");
    }
    else
    {
        // Check if example templates need update
        auto farmJsonOpt = AtomicFileIO::safeReadJson(farmPath / "farm.json");
        if (farmJsonOpt.has_value())
        {
            auto& fj = farmJsonOpt.value();
            std::string lastUpdate;
            if (fj.contains("last_example_update"))
                lastUpdate = fj["last_example_update"].get<std::string>();

            if (lastUpdate != APP_VERSION)
            {
                MonitorLog::instance().info("farm", "Updating example templates (" + lastUpdate + " -> " + std::string(APP_VERSION) + ")");
                copyExampleTemplates(farmPath);

                fj["last_example_update"] = APP_VERSION;
                AtomicFileIO::writeJson(farmPath / "farm.json", fj);
            }
        }
    }

    // Always ensure own node dirs exist
    fs::create_directories(farmPath / "nodes" / nodeId, ec);
    fs::create_directories(farmPath / "commands" / nodeId / "processed", ec);

    result.success = true;
    return result;
}

} // namespace SR
