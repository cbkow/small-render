#include "core/ipc_server.h"

#include <iostream>
#include <cstring>

#ifdef _WIN32

namespace SR {

IpcServer::IpcServer()
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

IpcServer::~IpcServer()
{
    close();
    if (m_stopEvent) CloseHandle(m_stopEvent);
    if (m_connectEvent) CloseHandle(m_connectEvent);
}

bool IpcServer::create(const std::string& nodeId)
{
    std::wstring pipeName = L"\\\\.\\pipe\\SmallRenderAgent_";
    for (char c : nodeId) pipeName += static_cast<wchar_t>(c);

    m_pipe = CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,          // max instances
        65536,      // out buffer
        65536,      // in buffer
        0,          // default timeout
        nullptr     // default security
    );

    if (m_pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "[IpcServer] CreateNamedPipe failed: " << GetLastError() << std::endl;
        return false;
    }

    std::cout << "[IpcServer] Pipe created for node " << nodeId << std::endl;
    return true;
}

bool IpcServer::acceptConnection()
{
    if (m_pipe == INVALID_HANDLE_VALUE) return false;

    ResetEvent(m_connectEvent);

    OVERLAPPED ov{};
    ov.hEvent = m_connectEvent;

    BOOL result = ConnectNamedPipe(m_pipe, &ov);
    if (result)
    {
        // Client already connected (rare but valid)
        m_connected = true;
        std::cout << "[IpcServer] Client connected (immediate)" << std::endl;
        return true;
    }

    DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED)
    {
        // Client was already waiting before we called ConnectNamedPipe
        m_connected = true;
        std::cout << "[IpcServer] Client connected (already waiting)" << std::endl;
        return true;
    }

    if (err != ERROR_IO_PENDING)
    {
        std::cerr << "[IpcServer] ConnectNamedPipe failed: " << err << std::endl;
        return false;
    }

    // Wait for either client connection or stop signal
    HANDLE events[] = { m_connectEvent, m_stopEvent };
    DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, INFINITE);

    if (waitResult == WAIT_OBJECT_0)
    {
        // Connection event fired
        DWORD bytesTransferred = 0;
        if (GetOverlappedResult(m_pipe, &ov, &bytesTransferred, FALSE))
        {
            m_connected = true;
            std::cout << "[IpcServer] Client connected" << std::endl;
            return true;
        }
        else
        {
            std::cerr << "[IpcServer] GetOverlappedResult failed: " << GetLastError() << std::endl;
            return false;
        }
    }
    else if (waitResult == WAIT_OBJECT_0 + 1)
    {
        // Stop event
        CancelIo(m_pipe);
        std::cout << "[IpcServer] Accept cancelled (stop signaled)" << std::endl;
        return false;
    }
    else
    {
        std::cerr << "[IpcServer] WaitForMultipleObjects failed: " << GetLastError() << std::endl;
        CancelIo(m_pipe);
        return false;
    }
}

bool IpcServer::send(const std::string& json)
{
    if (!m_connected || m_pipe == INVALID_HANDLE_VALUE) return false;

    std::lock_guard<std::mutex> lock(m_writeMutex);

    // Write 4-byte little-endian length prefix
    uint32_t len = static_cast<uint32_t>(json.size());
    DWORD bytesWritten = 0;

    if (!WriteFile(m_pipe, &len, sizeof(len), &bytesWritten, nullptr) || bytesWritten != sizeof(len))
    {
        std::cerr << "[IpcServer] Failed to write length prefix: " << GetLastError() << std::endl;
        m_connected = false;
        return false;
    }

    // Write payload
    if (!WriteFile(m_pipe, json.data(), len, &bytesWritten, nullptr) || bytesWritten != len)
    {
        std::cerr << "[IpcServer] Failed to write payload: " << GetLastError() << std::endl;
        m_connected = false;
        return false;
    }

    return true;
}

bool IpcServer::readExact(void* buf, DWORD count, int timeoutMs)
{
    DWORD totalRead = 0;
    auto* p = static_cast<char*>(buf);

    while (totalRead < count)
    {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return false;

        DWORD bytesRead = 0;
        BOOL result = ReadFile(m_pipe, p + totalRead, count - totalRead, &bytesRead, &ov);

        if (result)
        {
            // Read completed synchronously
            CloseHandle(ov.hEvent);
            totalRead += bytesRead;
            continue;
        }

        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING)
        {
            CloseHandle(ov.hEvent);
            if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA)
            {
                m_connected = false;
            }
            return false;
        }

        // Wait for read to complete, stop event, or timeout
        HANDLE events[] = { ov.hEvent, m_stopEvent };
        DWORD timeout = (timeoutMs < 0) ? INFINITE : static_cast<DWORD>(timeoutMs);
        DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, timeout);

        if (waitResult == WAIT_OBJECT_0)
        {
            DWORD transferred = 0;
            if (GetOverlappedResult(m_pipe, &ov, &transferred, FALSE))
            {
                totalRead += transferred;
            }
            else
            {
                DWORD ovErr = GetLastError();
                CloseHandle(ov.hEvent);
                if (ovErr == ERROR_BROKEN_PIPE) m_connected = false;
                return false;
            }
        }
        else
        {
            // Timeout or stop event
            CancelIo(m_pipe);
            CloseHandle(ov.hEvent);
            if (waitResult == WAIT_OBJECT_0 + 1) m_connected = false;
            return false;
        }

        CloseHandle(ov.hEvent);
    }

    return true;
}

std::optional<std::string> IpcServer::receive(int timeoutMs)
{
    if (!m_connected || m_pipe == INVALID_HANDLE_VALUE) return std::nullopt;

    // Read 4-byte length prefix
    uint32_t len = 0;
    if (!readExact(&len, sizeof(len), timeoutMs))
        return std::nullopt;

    // Sanity check
    if (len > 16 * 1024 * 1024)
    {
        std::cerr << "[IpcServer] Message too large: " << len << " bytes" << std::endl;
        m_connected = false;
        return std::nullopt;
    }

    // Read payload
    std::string payload(len, '\0');
    if (!readExact(payload.data(), len, timeoutMs))
        return std::nullopt;

    return payload;
}

void IpcServer::disconnect()
{
    if (m_pipe != INVALID_HANDLE_VALUE && m_connected)
    {
        FlushFileBuffers(m_pipe);
        DisconnectNamedPipe(m_pipe);
        m_connected = false;
        std::cout << "[IpcServer] Client disconnected" << std::endl;
    }
}

void IpcServer::close()
{
    signalStop();
    disconnect();
    if (m_pipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
        std::cout << "[IpcServer] Pipe closed" << std::endl;
    }
}

void IpcServer::signalStop()
{
    if (m_stopEvent) SetEvent(m_stopEvent);
}

} // namespace SR

#endif // _WIN32
