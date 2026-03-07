#include "platform/macos/ipc/socket_transport.h"
#include "core/vmm/types.h"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace ipc {

UnixSocketTransport::~UnixSocketTransport() {
    Close();
}

bool UnixSocketTransport::Connect(const std::string& endpoint) {
    if (fd_ >= 0) return true;

    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
        LOG_ERROR("UnixSocketTransport: socket() failed: %s", strerror(errno));
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (endpoint.size() >= sizeof(addr.sun_path)) {
        LOG_ERROR("UnixSocketTransport: path too long: %s", endpoint.c_str());
        close(fd_);
        fd_ = -1;
        return false;
    }
    std::strncpy(addr.sun_path, endpoint.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) < 0) {
        LOG_ERROR("UnixSocketTransport: connect(%s) failed: %s",
                  endpoint.c_str(), strerror(errno));
        close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

bool UnixSocketTransport::IsConnected() const {
    return fd_ >= 0;
}

bool UnixSocketTransport::Send(const void* data, size_t len) {
    if (fd_ < 0) return false;

    const char* ptr = static_cast<const char*>(data);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = ::send(fd_, ptr, remaining, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        ptr += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

int UnixSocketTransport::PollRead(int timeout_ms) {
    if (fd_ < 0) return -1;

    struct pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0) return -1;
    if (ret == 0) return 0;
    if (pfd.revents & (POLLERR | POLLHUP)) return -1;
    return 1;
}

ssize_t UnixSocketTransport::Recv(void* buf, size_t max_len) {
    if (fd_ < 0) return -1;
    return ::recv(fd_, buf, max_len, 0);
}

void UnixSocketTransport::Close() {
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
}

std::unique_ptr<IpcTransport> CreateClientTransport() {
    return std::make_unique<UnixSocketTransport>();
}

} // namespace ipc
