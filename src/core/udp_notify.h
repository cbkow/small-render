#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <WinSock2.h>
using SocketType = SOCKET;
constexpr SocketType INVALID_SOCK = INVALID_SOCKET;
#else
using SocketType = int;
constexpr SocketType INVALID_SOCK = -1;
#endif

namespace SR {

class UdpNotify
{
public:
    UdpNotify() = default;
    ~UdpNotify();

    UdpNotify(const UdpNotify&) = delete;
    UdpNotify& operator=(const UdpNotify&) = delete;

    // Start multicast socket. Returns false on failure (graceful degradation).
    bool start(const std::string& nodeId, uint16_t port = 4242,
               const std::string& group = "239.42.0.1");
    void stop();

    // Fire-and-forget multicast send.
    void send(const nlohmann::json& msg);

    // Non-blocking receive. Returns parsed messages, filtering out self-sent.
    std::vector<nlohmann::json> poll();

    bool isRunning() const { return m_socket != INVALID_SOCK; }

private:
    SocketType m_socket = INVALID_SOCK;
    struct sockaddr_in m_groupAddr{};
    std::string m_nodeId;

    static constexpr size_t MAX_MSG_SIZE = 1400; // MTU guard
};

} // namespace SR
