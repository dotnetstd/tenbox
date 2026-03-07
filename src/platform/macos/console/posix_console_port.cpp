#include "platform/macos/console/posix_console_port.h"
#include <unistd.h>
#include <poll.h>
#include <cstring>

PosixConsolePort::PosixConsolePort() {
    if (isatty(STDIN_FILENO)) {
        tcgetattr(STDIN_FILENO, &orig_termios_);
        struct termios raw = orig_termios_;
        cfmakeraw(&raw);
        // Preserve ISIG so Ctrl-C still sends SIGINT to the process
        raw.c_lflag |= ISIG;
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 1;  // 100ms read timeout
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        raw_mode_ = true;
    }

    // Enable UTF-8 output (macOS terminals are UTF-8 by default)
    setvbuf(stdout, nullptr, _IONBF, 0);
}

PosixConsolePort::~PosixConsolePort() {
    if (raw_mode_) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios_);
    }
}

void PosixConsolePort::Write(const uint8_t* data, size_t size) {
    if (!data || size == 0) return;
    std::lock_guard<std::mutex> lock(write_mutex_);
    ::write(STDOUT_FILENO, data, size);
}

size_t PosixConsolePort::Read(uint8_t* out, size_t size) {
    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, 50);  // 50ms timeout
    if (ret <= 0) return 0;

    ssize_t n = ::read(STDIN_FILENO, out, size);
    return (n > 0) ? static_cast<size_t>(n) : 0;
}
