#pragma once

#include "ipc/ipc_transport.h"

#include <windows.h>

namespace ipc {

class WindowsPipeTransport final : public IpcTransport {
public:
    WindowsPipeTransport() = default;
    ~WindowsPipeTransport() override;

    bool Connect(const std::string& endpoint) override;
    bool IsConnected() const override;
    bool Send(const void* data, size_t len) override;
    int  PollRead(int timeout_ms) override;
    ssize_t Recv(void* buf, size_t max_len) override;
    void Close() override;

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

} // namespace ipc
