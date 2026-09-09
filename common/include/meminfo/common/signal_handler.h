#pragma once
#include <uv.h>
#include <functional>

namespace meminfo {

// RAII signal handler using libuv.
// Catches SIGTERM and SIGINT, calls the provided callback, then stops the event loop.
class SignalHandler {
public:
    // callback is called on SIGTERM/SIGINT, then the loop is stopped.
    SignalHandler(uv_loop_t* loop, std::function<void()> shutdown_callback);
    ~SignalHandler();
    
    // Non-copyable, non-movable
    SignalHandler(const SignalHandler&) = delete;
    SignalHandler& operator=(const SignalHandler&) = delete;
    
private:
    static void on_signal(uv_signal_t* handle, int signum);
    
    uv_signal_t* sigterm_handle_ = nullptr;
    uv_signal_t* sigint_handle_ = nullptr;
    std::function<void()> callback_;
    bool closed_ = false;
};

} // namespace meminfo
