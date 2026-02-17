#include "core/node_identity.h"
#include "core/platform.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <rpc.h>
#include <dxgi.h>
#pragma comment(lib, "dxgi.lib")
#endif

namespace SR {

void NodeIdentity::loadOrGenerate(const std::filesystem::path& appDataDir)
{
    auto idPath = appDataDir / "node_id.txt";

    // Try to read existing
    if (std::filesystem::exists(idPath))
    {
        std::ifstream file(idPath);
        if (file.is_open())
        {
            std::string id;
            std::getline(file, id);
            if (id.size() == 12)
            {
                m_nodeId = id;
                std::cout << "[NodeIdentity] Loaded node_id: " << m_nodeId << std::endl;
                return;
            }
        }
    }

    // Generate new
    m_nodeId = generate();

    // Persist
    std::ofstream file(idPath);
    if (file.is_open())
    {
        file << m_nodeId;
        std::cout << "[NodeIdentity] Generated new node_id: " << m_nodeId << std::endl;
    }
    else
    {
        std::cerr << "[NodeIdentity] Failed to write node_id.txt" << std::endl;
    }
}

std::string NodeIdentity::generate()
{
#ifdef _WIN32
    UUID uuid;
    UuidCreate(&uuid);

    // Take first 12 hex characters from the UUID
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    oss << std::setw(8) << uuid.Data1;
    oss << std::setw(4) << uuid.Data2;
    return oss.str(); // Exactly 12 hex chars
#else
    // Fallback: read from /dev/urandom
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    unsigned char bytes[6];
    urandom.read(reinterpret_cast<char*>(bytes), 6);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 6; i++)
        oss << std::setw(2) << static_cast<int>(bytes[i]);
    return oss.str();
#endif
}

void NodeIdentity::querySystemInfo()
{
    m_systemInfo.hostname = getHostname();
    m_systemInfo.cpuCores = static_cast<int>(std::thread::hardware_concurrency());

#ifdef _WIN32
    // RAM
    MEMORYSTATUSEX memInfo{};
    memInfo.dwLength = sizeof(memInfo);
    if (GlobalMemoryStatusEx(&memInfo))
    {
        m_systemInfo.ramMB = static_cast<uint64_t>(memInfo.ullTotalPhys / (1024 * 1024));
    }

    // GPU via DXGI
    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory))))
    {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(factory->EnumAdapters(0, &adapter)))
        {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc)))
            {
                int len = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
                if (len > 0)
                {
                    std::string name(static_cast<size_t>(len - 1), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name.data(), len, nullptr, nullptr);
                    m_systemInfo.gpuName = name;
                }
            }
            adapter->Release();
        }
        factory->Release();
    }
#endif

    std::cout << "[NodeIdentity] System: " << m_systemInfo.hostname
              << " | CPU cores: " << m_systemInfo.cpuCores
              << " | RAM: " << m_systemInfo.ramMB << " MB"
              << " | GPU: " << m_systemInfo.gpuName << std::endl;
}

} // namespace SR
