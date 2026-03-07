#include "platform/windows/ipc/pipe_transport.h"
#include "core/vmm/types.h"

namespace ipc {

WindowsPipeTransport::~WindowsPipeTransport() {
    Close();
}

bool WindowsPipeTransport::Connect(const std::string& endpoint) {
    if (handle_ != INVALID_HANDLE_VALUE) return true;

    std::string full_name = R"(\\.\pipe\)" + endpoint;

    handle_ = CreateFileA(
        full_name.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (handle_ == INVALID_HANDLE_VALUE) {
        LOG_ERROR("WindowsPipeTransport: failed to connect to %s: %lu",
                  full_name.c_str(), GetLastError());
        return false;
    }

    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(handle_, &mode, nullptr, nullptr);
    return true;
}

bool WindowsPipeTransport::IsConnected() const {
    return handle_ != INVALID_HANDLE_VALUE;
}

bool WindowsPipeTransport::Send(const void* data, size_t len) {
    if (handle_ == INVALID_HANDLE_VALUE) return false;

    const char* ptr = static_cast<const char*>(data);
    size_t remaining = len;
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(handle_, ptr, static_cast<DWORD>(remaining),
                       &written, nullptr)) {
            return false;
        }
        if (written == 0) return false;
        ptr += written;
        remaining -= written;
    }
    return true;
}

int WindowsPipeTransport::PollRead(int timeout_ms) {
    if (handle_ == INVALID_HANDLE_VALUE) return -1;

    // PeekNamedPipe is non-blocking; poll in a loop with short sleeps
    auto deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(handle_, nullptr, 0, nullptr, &available, nullptr)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE) return -1;
            return -1;
        }
        if (available > 0) return 1;
        if (GetTickCount64() >= deadline) return 0;
        Sleep(1);
    }
}

ssize_t WindowsPipeTransport::Recv(void* buf, size_t max_len) {
    if (handle_ == INVALID_HANDLE_VALUE) return -1;

    DWORD available = 0;
    if (!PeekNamedPipe(handle_, nullptr, 0, nullptr, &available, nullptr)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE) return 0;
        return -1;
    }
    if (available == 0) return 0;

    DWORD to_read = static_cast<DWORD>((std::min)(
        static_cast<size_t>(available), max_len));
    DWORD bytes_read = 0;
    if (!ReadFile(handle_, buf, to_read, &bytes_read, nullptr)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_OPERATION_ABORTED)
            return 0;
        return -1;
    }
    return static_cast<ssize_t>(bytes_read);
}

void WindowsPipeTransport::Close() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(handle_);
        CancelIoEx(handle_, nullptr);
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
}

std::unique_ptr<IpcTransport> CreateClientTransport() {
    return std::make_unique<WindowsPipeTransport>();
}

} // namespace ipc
