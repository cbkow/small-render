#pragma once

#include <string>
#include <filesystem>

namespace SR {

struct SystemInfo
{
    std::string hostname;
    std::string gpuName;
    int cpuCores = 0;
    uint64_t ramMB = 0;
};

class NodeIdentity
{
public:
    // Load existing node_id from disk, or generate and persist a new one
    void loadOrGenerate(const std::filesystem::path& appDataDir);

    // Query hardware info (hostname, GPU, CPU cores, RAM)
    void querySystemInfo();

    const std::string& nodeId() const { return m_nodeId; }
    const SystemInfo& systemInfo() const { return m_systemInfo; }

private:
    std::string generate();

    std::string m_nodeId;
    SystemInfo m_systemInfo;
};

} // namespace SR
