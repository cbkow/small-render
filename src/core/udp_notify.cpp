#include "core/udp_notify.h"

#ifdef _WIN32
#include <WS2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#endif

#include <cstring>
#include <iostream>

namespace SR {

#ifdef _WIN32
static bool s_wsaInited = false;

static bool ensureWSA()
{
    if (s_wsaInited) return true;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return false;
    s_wsaInited = true;
    return true;
}
#endif

UdpNotify::~UdpNotify()
{
    stop();
}

bool UdpNotify::start(const std::string& nodeId, uint16_t port, const std::string& group)
{
    if (m_socket != INVALID_SOCK)
        return true;

#ifdef _WIN32
    if (!ensureWSA())
        return false;
#endif

    m_nodeId = nodeId;

    // Create UDP socket
    m_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_socket == INVALID_SOCK)
        return false;

    // Allow multiple listeners on same port
    int reuse = 1;
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    // Bind to INADDR_ANY on the multicast port
    struct sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(port);
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(m_socket, reinterpret_cast<struct sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0)
    {
        stop();
        return false;
    }

    // Join multicast group
    struct ip_mreq mreq{};
    inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(m_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   reinterpret_cast<const char*>(&mreq), sizeof(mreq)) != 0)
    {
        stop();
        return false;
    }

    // Set non-blocking
#ifdef _WIN32
    u_long nonBlocking = 1;
    ioctlsocket(m_socket, FIONBIO, &nonBlocking);
#else
    int flags = fcntl(m_socket, F_GETFL, 0);
    fcntl(m_socket, F_SETFL, flags | O_NONBLOCK);
#endif

    // Set multicast TTL (stay on local network)
    int ttl = 1;
    setsockopt(m_socket, IPPROTO_IP, IP_MULTICAST_TTL,
               reinterpret_cast<const char*>(&ttl), sizeof(ttl));

    // Store group address for sends
    std::memset(&m_groupAddr, 0, sizeof(m_groupAddr));
    m_groupAddr.sin_family = AF_INET;
    m_groupAddr.sin_port = htons(port);
    inet_pton(AF_INET, group.c_str(), &m_groupAddr.sin_addr);

    return true;
}

void UdpNotify::stop()
{
    if (m_socket == INVALID_SOCK)
        return;

#ifdef _WIN32
    closesocket(m_socket);
#else
    close(m_socket);
#endif
    m_socket = INVALID_SOCK;
}

void UdpNotify::send(const nlohmann::json& msg)
{
    if (m_socket == INVALID_SOCK)
        return;

    std::string data = msg.dump();
    if (data.size() > MAX_MSG_SIZE)
        return; // MTU guard — drop oversized messages

    sendto(m_socket, data.c_str(), static_cast<int>(data.size()), 0,
           reinterpret_cast<struct sockaddr*>(&m_groupAddr), sizeof(m_groupAddr));
}

std::vector<nlohmann::json> UdpNotify::poll()
{
    std::vector<nlohmann::json> results;
    if (m_socket == INVALID_SOCK)
        return results;

    char buf[1500];
    struct sockaddr_in sender{};
    int senderLen = sizeof(sender);

    for (;;)
    {
        auto n = recvfrom(m_socket, buf, sizeof(buf) - 1, 0,
                          reinterpret_cast<struct sockaddr*>(&sender),
#ifdef _WIN32
                          &senderLen
#else
                          reinterpret_cast<socklen_t*>(&senderLen)
#endif
        );

        if (n <= 0)
            break; // EWOULDBLOCK or error — done

        buf[n] = '\0';

        try
        {
            auto msg = nlohmann::json::parse(buf, buf + n);

            // Filter out self-sent packets
            std::string from = msg.value("from", "");
            if (from == m_nodeId)
                continue;

            results.push_back(std::move(msg));
        }
        catch (...)
        {
            // Malformed JSON — silently skip
        }
    }

    return results;
}

} // namespace SR
