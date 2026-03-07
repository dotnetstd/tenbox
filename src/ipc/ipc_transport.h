#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace ipc {

// Platform-agnostic bidirectional byte-stream transport between the
// Manager (server) and Runtime (client) processes.
//
// Implementations:
//   Windows  — WindowsPipeTransport  (Named Pipes)
//   macOS    — UnixSocketTransport   (Unix Domain Sockets)
class IpcTransport {
public:
    virtual ~IpcTransport() = default;

    // Connect to the server endpoint identified by |endpoint|.
    // On Windows this is a pipe name (e.g. "tenbox_vm_xxx"),
    // on Unix this is a socket path.
    virtual bool Connect(const std::string& endpoint) = 0;

    virtual bool IsConnected() const = 0;

    // Send |len| bytes.  Returns true when all bytes have been written.
    virtual bool Send(const void* data, size_t len) = 0;

    // Convenience overload.
    bool Send(const std::string& data) {
        return Send(data.data(), data.size());
    }

    // Wait up to |timeout_ms| for readable data.
    // Returns:  1 = data available,  0 = timeout,  -1 = error / disconnected.
    virtual int PollRead(int timeout_ms) = 0;

    // Read up to |max_len| bytes into |buf|.
    // Returns bytes read, 0 on EOF, -1 on error.
    virtual ssize_t Recv(void* buf, size_t max_len) = 0;

    // Flush and close the transport.
    virtual void Close() = 0;
};

// Factory: create the platform-appropriate transport for the runtime
// (client) side.
std::unique_ptr<IpcTransport> CreateClientTransport();

} // namespace ipc
