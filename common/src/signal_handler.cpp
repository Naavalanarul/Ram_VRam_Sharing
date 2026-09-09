#include "meminfo/common/signal_handler.h"

namespace meminfo {

SignalHandler::SignalHandler(uv_loop_t* loop, std::function<void()> shutdown_callback)
    : callback_(std::move(shutdown_callback)), closed_(false) {
    
    sigterm_handle_ = new uv_signal_t;
    sigint_handle_ = new uv_signal_t;
    
    uv_signal_init(loop, sigterm_handle_);
    uv_signal_init(loop, sigint_handle_);
    
    sigterm_handle_->data = this;
    sigint_handle_->data = this;
    
    uv_signal_start(sigterm_handle_, on_signal, SIGTERM);
    uv_signal_start(sigint_handle_, on_signal, SIGINT);
}

SignalHandler::~SignalHandler() {
    if (!closed_) {
        closed_ = true;
        
        auto close_cb = [](uv_handle_t* h) {
            delete reinterpret_cast<uv_signal_t*>(h);
        };
        
        if (sigterm_handle_->loop) {
            uv_close(reinterpret_cast<uv_handle_t*>(sigterm_handle_), close_cb);
        } else {
            delete sigterm_handle_;
        }
        
        if (sigint_handle_->loop) {
            uv_close(reinterpret_cast<uv_handle_t*>(sigint_handle_), close_cb);
        } else {
            delete sigint_handle_;
        }
    }
}

void SignalHandler::on_signal(uv_signal_t* handle, int /*signum*/) {
    auto* self = static_cast<SignalHandler*>(handle->data);
    if (self && self->callback_) {
        self->callback_();
    }
    uv_stop(handle->loop);
}

} // namespace meminfo
