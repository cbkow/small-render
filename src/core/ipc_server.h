#pragma once

#include <string>
#include <optional>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace SR {

/// Named pipe IPC server for communicating with the agent process.
/// Byte-mode pipe with length-prefixed JSON framing.
/// One instance, one client at a time.
class IpcServer
{
public:
    IpcServer();
    ~IpcServer();

    // Non-copyable, non-movable (owns OS handles)
    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    /// Create the named pipe for the given node ID.
    /// Pipe name: \\.\pipe\SmallRenderAgent_{nodeId}
    bool create(const std::string& nodeId);

    /// Wait for a client to connect. Blocks until connection or stop is signaled.
    /// Returns true if a client connected, false if stop was signaled or error.
    bool acceptConnection();

    /// Send a JSON message (length-prefixed). Thread-safe.
    bool send(const std::string& json);

    /// Receive a JSON message. Blocks up to timeoutMs (-1 = infinite).
    /// Returns nullopt on timeout, disconnect, or error.
    std::optional<std::string> receive(int timeoutMs = -1);

    /// Disconnect the current client (allows re-accepting).
    void disconnect();

    /// Close the pipe entirely.
    void close();

    /// Signal the stop event to unblock acceptConnection().
    void signalStop();

    /// Check if a client is currently connected.
    bool isConnected() const { return m_connected; }

private:
#ifdef _WIN32
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    HANDLE m_stopEvent = nullptr;
    HANDLE m_connectEvent = nullptr;
    bool m_connected = false;
    std::mutex m_writeMutex;

    bool readExact(void* buf, DWORD count, int timeoutMs);
#endif
};

} // namespace SR
