#include "meminfo/common/http_status.h"
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace meminfo {

// Note: when the constructor throws, the server handle has been handed to
// uv_close but the loop must still be run once for that close to complete
// before this object's storage is reused.
HttpStatusServer::HttpStatusServer(uv_loop_t* loop, const std::string& bind_addr, int port, StatusProvider provider)
    : provider_(std::move(provider)), stopped_(false) {
    
    uv_tcp_init(loop, &server_);
    server_.data = this;
    
    struct sockaddr_in addr;
    int r = uv_ip4_addr(bind_addr.c_str(), port, &addr);
    if (r) {
        uv_close(reinterpret_cast<uv_handle_t*>(&server_), nullptr);
        stopped_ = true;
        throw std::runtime_error("Invalid bind address '" + bind_addr + "': " + uv_strerror(r));
    }

    // A failed bind used to go unnoticed here, surfacing later as a confusing
    // listen error (or, worse, a server bound to the wrong address).
    r = uv_tcp_bind(&server_, reinterpret_cast<const struct sockaddr*>(&addr), 0);
    if (r) {
        uv_close(reinterpret_cast<uv_handle_t*>(&server_), nullptr);
        stopped_ = true;
        throw std::runtime_error("Bind error: " + std::string(uv_strerror(r)));
    }

    r = uv_listen(reinterpret_cast<uv_stream_t*>(&server_), 128, on_connection);
    if (r) {
        uv_close(reinterpret_cast<uv_handle_t*>(&server_), nullptr);
        stopped_ = true;
        throw std::runtime_error("Listen error: " + std::string(uv_strerror(r)));
    }
}

HttpStatusServer::~HttpStatusServer() {
    stop();
}

void HttpStatusServer::stop() {
    if (!stopped_) {
        stopped_ = true;
        if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&server_))) {
            uv_close(reinterpret_cast<uv_handle_t*>(&server_), nullptr);
        }
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
        std::string request(buf->base, static_cast<size_t>(nread));
        std::string body;
        std::string response;

        if (request.rfind("GET /status", 0) == 0) {
            if (self->provider_) {
                body = self->provider_();
            }
            response = "HTTP/1.1 200 OK\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: " + std::to_string(body.size()) + "\r\n"
                       "Connection: close\r\n\r\n" + body;
        } else {
            response = "HTTP/1.1 404 Not Found\r\n"
                       "Content-Length: 0\r\n"
                       "Connection: close\r\n\r\n";
        }

        // Stop reading before replying: the write callback closes the
        // connection, and a second read completing in between would close it
        // again and free the handle twice.
        uv_read_stop(client);

        uv_write_t* req = new uv_write_t;
        char* payload = static_cast<char*>(malloc(response.size()));
        if (!payload) {
            delete req;
            delete[] buf->base;
            uv_close(reinterpret_cast<uv_handle_t*>(client), [](uv_handle_t* handle) {
                delete reinterpret_cast<uv_tcp_t*>(handle);
            });
            return;
        }
        std::memcpy(payload, response.data(), response.size());

        uv_buf_t write_buf = uv_buf_init(payload, static_cast<unsigned int>(response.size()));
        req->data = payload;

        if (uv_write(req, client, &write_buf, 1, on_write_done) != 0) {
            free(payload);
            delete req;
            uv_close(reinterpret_cast<uv_handle_t*>(client), [](uv_handle_t* handle) {
                delete reinterpret_cast<uv_tcp_t*>(handle);
            });
        }
    } else if (nread < 0) {
        if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(client))) {
            uv_close(reinterpret_cast<uv_handle_t*>(client), [](uv_handle_t* handle) {
                delete reinterpret_cast<uv_tcp_t*>(handle);
            });
        }
    }

    delete[] buf->base;
}

void HttpStatusServer::on_write_done(uv_write_t* req, int /*status*/) {
    free(req->data);

    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(req->handle))) {
        uv_close(reinterpret_cast<uv_handle_t*>(req->handle), [](uv_handle_t* handle) {
            delete reinterpret_cast<uv_tcp_t*>(handle);
        });
    }

    delete req;
}

} // namespace meminfo
