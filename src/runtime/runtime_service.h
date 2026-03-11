#pragma once

#include "common/ports.h"
#include "ipc/protocol_v1.h"
#include "ipc/shared_framebuffer.h"

#include <uv.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class Vm;

class ManagedConsolePort final : public ConsolePort {
public:
    void Write(const uint8_t* data, size_t size) override;
    size_t Read(uint8_t* out, size_t size) override;

    void PushInput(const uint8_t* data, size_t size);

    // Returns and clears any pending output data.
    std::string FlushPending();
    bool HasPending() const;

    // Called when new output data is available.
    void SetDataAvailableCallback(std::function<void()> callback);

    using InputCallback = std::function<void(const uint8_t*, size_t)>;
    void SetInputCallback(InputCallback cb);

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<uint8_t> queue_;
    std::string pending_write_;
    std::function<void()> data_available_callback_;
    InputCallback input_callback_;
};

class ManagedInputPort final : public InputPort {
public:
    bool PollKeyboard(KeyboardEvent* event) override;
    bool PollPointer(PointerEvent* event) override;

    void PushKeyEvent(const KeyboardEvent& ev);
    void PushPointerEvent(const PointerEvent& ev);

    using KeyCallback = std::function<void(const KeyboardEvent&)>;
    using PointerCallback = std::function<void(const PointerEvent&)>;
    void SetKeyEventCallback(KeyCallback cb);
    void SetPointerEventCallback(PointerCallback cb);

private:
    std::mutex mutex_;
    std::deque<KeyboardEvent> key_queue_;
    std::deque<PointerEvent> pointer_queue_;
    KeyCallback key_callback_;
    PointerCallback pointer_callback_;
};

class ManagedDisplayPort final : public DisplayPort {
public:
    void SubmitFrame(DisplayFrame frame) override;
    void SubmitCursor(const CursorInfo& cursor) override;
    void SubmitScanoutState(bool active, uint32_t width, uint32_t height) override;
    void SetFrameHandler(std::function<void(DisplayFrame)> handler);
    void SetCursorHandler(std::function<void(const CursorInfo&)> handler);
    void SetStateHandler(std::function<void(bool, uint32_t, uint32_t)> handler);

private:
    std::mutex mutex_;
    std::function<void(DisplayFrame)> frame_handler_;
    std::function<void(const CursorInfo&)> cursor_handler_;
    std::function<void(bool, uint32_t, uint32_t)> state_handler_;
};

class ManagedClipboardPort final : public ClipboardPort {
public:
    void OnClipboardEvent(const ClipboardEvent& event) override;
    void SetEventHandler(std::function<void(const ClipboardEvent&)> handler);

private:
    std::mutex mutex_;
    std::function<void(const ClipboardEvent&)> event_handler_;
};

class ManagedAudioPort final : public AudioPort {
public:
    void SubmitPcm(AudioChunk chunk) override;
    void SetPcmHandler(std::function<void(AudioChunk)> handler);

private:
    std::mutex mutex_;
    std::function<void(AudioChunk)> pcm_handler_;
};

class RuntimeControlService {
public:
    RuntimeControlService(std::string vm_id, std::string pipe_name);
    ~RuntimeControlService();

    bool Start();
    void Stop();

    void AttachVm(Vm* vm);
    std::shared_ptr<ManagedConsolePort> ConsolePort() const { return console_port_; }
    std::shared_ptr<ManagedInputPort> GetInputPort() const { return input_port_; }
    std::shared_ptr<ManagedDisplayPort> GetDisplayPort() const { return display_port_; }
    std::shared_ptr<ManagedClipboardPort> GetClipboardPort() const { return clipboard_port_; }
    std::shared_ptr<ManagedAudioPort> GetAudioPort() const { return audio_port_; }
    void PublishState(const std::string& state, int exit_code = 0);

private:
    bool Send(const ipc::Message& message);
    bool SendWithPayload(const ipc::Message& message);
    void EventLoopThread();
    void HandleMessage(const ipc::Message& message);
    void DrainSendQueues();
    void FlushConsoleData();
    void WriteRaw(const std::string& data);

    static void OnPipeRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
    static void OnAllocBuffer(uv_handle_t* handle, size_t suggested, uv_buf_t* buf);
    static void OnWriteDone(uv_write_t* req, int status);
    static void OnSendWakeup(uv_async_t* handle);
    static void OnConsoleCoalesce(uv_timer_t* handle);
    static void OnStopSignal(uv_async_t* handle);
    static void OnPipeConnect(uv_connect_t* req, int status);

    std::string vm_id_;
    std::string pipe_name_;
    std::shared_ptr<ManagedConsolePort> console_port_ = std::make_shared<ManagedConsolePort>();
    std::shared_ptr<ManagedInputPort> input_port_ = std::make_shared<ManagedInputPort>();
    std::shared_ptr<ManagedDisplayPort> display_port_ = std::make_shared<ManagedDisplayPort>();
    std::shared_ptr<ManagedClipboardPort> clipboard_port_ = std::make_shared<ManagedClipboardPort>();
    std::shared_ptr<ManagedAudioPort> audio_port_ = std::make_shared<ManagedAudioPort>();

    std::atomic<bool> running_{false};

    std::mutex start_mutex_;
    std::condition_variable start_cv_;
    bool loop_ready_ = false;

    std::thread loop_thread_;

    Vm* vm_ = nullptr;
    std::atomic<uint64_t> next_event_id_{1};

    // Send queues (protected by send_queue_mutex_, drained on event loop thread).
    std::mutex send_queue_mutex_;
    std::deque<std::string> console_queue_;

    // libuv event loop handles
    uv_loop_t loop_{};
    uv_pipe_t pipe_{};
    uv_connect_t connect_req_{};
    uv_async_t send_wakeup_{};
    uv_async_t stop_wakeup_{};
    uv_timer_t console_coalesce_timer_{};
    bool coalesce_running_ = false;
    bool pipe_connected_ = false;

    // Receive buffer
    std::string recv_pending_;
    size_t recv_payload_needed_ = 0;
    ipc::Message recv_pending_msg_;

    // Shared-memory framebuffer for zero-copy frame transport.
    ipc::SharedFramebuffer shm_fb_;
    uint64_t shm_frame_seq_ = 0;
    uint32_t shm_generation_ = 0;
    bool shm_init_sent_ = false;

    static constexpr size_t kMaxPendingFrames = 4;
    std::deque<std::string> frame_queue_;
    uint64_t frame_drop_count_ = 0;
    std::chrono::steady_clock::time_point last_frame_drop_log_{};

    static constexpr size_t kMaxPendingAudio = 32;
    std::deque<std::string> audio_queue_;
};

std::string EncodeHex(const uint8_t* data, size_t size);
std::vector<uint8_t> DecodeHex(const std::string& value);
