#include "meminfo/common/http_status.h"
#include <iostream>
#include <cstring>
#include <stdexcept>

namespace meminfo {

HttpStatusServer::HttpStatusServer(uv_loop_t* loop, const std::string& bind_addr, int port, StatusProvider provider)
    : provider_(std::move(provider)), stopped_(false) {
    
    uv_tcp_init(loop, &server_);
    server_.data = this;
    
    struct sockaddr_in addr;
    uv_ip4_addr(bind_addr.c_str(), port, &addr);
    
    uv_tcp_bind(&server_, reinterpret_cast<const struct sockaddr*>(&addr), 0);
    int r = uv_listen(reinterpret_cast<uv_stream_t*>(&server_), 128, on_connection);
    if (r) {
        throw std::runtime_error("Listen error: " + std::string(uv_strerror(r)));
    }
}

HttpStatusServer::~HttpStatusServer() {
    stop();
}

void HttpStatusServer::stop() {
    if (!stopped_) {
        stopped_ = true;
        uv_close(reinterpret_cast<uv_handle_t*>(&server_), nullptr);
    }
}

void HttpStatusServer::on_connection(uv_stream_t* server, int status) {
    if (status < 0) return;
    
    auto* self = static_cast<HttpStatusServer*>(server->data);
    
    uv_tcp_t* client = new uv_tcp_t;
    uv_tcp_init(server->loop, client);
    client->data = self;
    
    if (uv_accept(server, reinterpret_cast<uv_stream_t*>(client)) == 0) {
        uv_read_start(reinterpret_cast<uv_stream_t*>(client), on_alloc, on_read);
    } else {
        uv_close(reinterpret_cast<uv_handle_t*>(client), [](uv_handle_t* handle) {
            delete reinterpret_cast<uv_tcp_t*>(handle);
        });
    }
}

void HttpStatusServer::on_alloc(uv_handle_t* /*handle*/, size_t suggested, uv_buf_t* buf) {
    buf->base = new char[suggested];
    buf->len = suggested;
}

void HttpStatusServer::on_read(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf) {
    auto* self = static_cast<HttpStatusServer*>(client->data);
    
    if (nread > 0) {
        std::string request(buf->base, nread);
        std::string response;
        
        if (request.find("GET /status") == 0) {
            std::string body = self->provider_();
            response = "HTTP/1.1 200 OK\r\n"
                       "Content-Type: application/json\r\n"
                       "Connection: close\r\n\r\n" + body;
        } else {
            response = "HTTP/1.1 404 Not Found\r\n"
                       "Connection: close\r\n\r\n";
        }
        
        uv_write_t* req = new uv_write_t;
        uv_buf_t write_buf = uv_buf_init(strdup(response.c_str()), response.size());
        req->data = write_buf.base;
        
        uv_write(req, client, &write_buf, 1, on_write_done);
    }
    
    if (nread < 0) {
        uv_close(reinterpret_cast<uv_handle_t*>(client), [](uv_handle_t* handle) {
            delete reinterpret_cast<uv_tcp_t*>(handle);
        });
    }
    
    delete[] buf->base;
}

void HttpStatusServer::on_write_done(uv_write_t* req, int /*status*/) {
    free(req->data);
    
    uv_close(reinterpret_cast<uv_handle_t*>(req->handle), [](uv_handle_t* handle) {
        delete reinterpret_cast<uv_tcp_t*>(handle);
    });
    
    delete req;
}

} // namespace meminfo
