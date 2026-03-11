#pragma once

#include <uv.h>
#include <cassert>

// RAII wrapper around uv_poll_t that tracks init/active/closing/closed state.
//
// Because uv_close() is asynchronous, the handle memory must stay alive until
// the close callback fires.  The destructor therefore must NOT call uv_close();
// callers must call Close() explicitly and keep the owning object alive until
// closed() returns true (i.e. until the next uv_run iteration).
class PollHandle {
public:
    PollHandle() = default;
    PollHandle(const PollHandle&) = delete;
    PollHandle& operator=(const PollHandle&) = delete;
    PollHandle(PollHandle&&) = delete;
    PollHandle& operator=(PollHandle&&) = delete;

    void Init(uv_loop_t* loop, uv_os_sock_t sock) {
        if (inited_) return;
        uv_poll_init_socket(loop, &handle_, sock);
        inited_ = true;
    }

    void Start(int events, uv_poll_cb cb, void* data) {
        if (closing_) return;
        handle_.data = data;
        if (!active_) active_ = true;
        uv_poll_start(&handle_, events, cb);
    }

    void Stop() {
        if (active_) {
            uv_poll_stop(&handle_);
            active_ = false;
        }
    }

    void Close() {
        if (closing_ || !inited_) return;
        Stop();
        closing_ = true;
        uv_close(reinterpret_cast<uv_handle_t*>(&handle_), [](uv_handle_t* h) {
            auto* self = reinterpret_cast<PollHandle*>(
                reinterpret_cast<char*>(h) - offsetof(PollHandle, handle_));
            self->closed_ = true;
        });
    }

    bool inited() const { return inited_; }
    bool active() const { return active_; }
    bool closing() const { return closing_; }
    bool closed() const { return closed_; }
    uv_poll_t* raw() { return &handle_; }

private:
    uv_poll_t handle_{};
    bool inited_ = false;
    bool active_ = false;
    bool closing_ = false;
    bool closed_ = false;
};
