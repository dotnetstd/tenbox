#pragma once

#include "ipc/ipc_transport.h"

namespace ipc {

class UnixSocketTransport final : public IpcTransport {
public:
    UnixSocketTransport() = default;
    ~UnixSocketTransport() override;

    bool Connect(const std::string& endpoint) override;
    bool IsConnected() const override;
    bool Send(const void* data, size_t len) override;
    int  PollRead(int timeout_ms) override;
    ssize_t Recv(void* buf, size_t max_len) override;
    void Close() override;

private:
    int fd_ = -1;
};

} // namespace ipc
