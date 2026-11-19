#include "websocketchannel.h"

#include <stdexcept>

WebSocketChannel::WebSocketChannel(const std::string &url) {
    if (!emscripten_websocket_is_supported()) {
        throw std::runtime_error("WebSockets are not supported");
    }
    EmscriptenWebSocketCreateAttributes attributes;
    emscripten_websocket_init_create_attributes(&attributes);
    attributes.url = url.c_str();
    attributes.protocols = "matterm-tty.v1";
    socket_ = emscripten_websocket_new(&attributes);
    if (socket_ <= 0) {
        throw std::runtime_error("Could not create the WebSocket");
    }
    emscripten_websocket_set_onopen_callback(socket_, this, onOpen);
    emscripten_websocket_set_onmessage_callback(socket_, this, onMessage);
    emscripten_websocket_set_onclose_callback(socket_, this, onClose);
    emscripten_websocket_set_onerror_callback(socket_, this, onError);
}

WebSocketChannel::~WebSocketChannel() {
    if (socket_ > 0) {
        // Detach callbacks that point at this object before it goes away.
        emscripten_websocket_set_onopen_callback(socket_, nullptr, nullptr);
        emscripten_websocket_set_onmessage_callback(socket_, nullptr, nullptr);
        emscripten_websocket_set_onclose_callback(socket_, nullptr, nullptr);
        emscripten_websocket_set_onerror_callback(socket_, nullptr, nullptr);
        emscripten_websocket_close(socket_, 1000, "");
        emscripten_websocket_delete(socket_);
    }
}

void WebSocketChannel::setEvents(Events events) {
    events_ = std::move(events);
}

bool WebSocketChannel::send(std::string_view bytes) {
    return open_ &&
           emscripten_websocket_send_binary(
               socket_, const_cast<char *>(bytes.data()), bytes.size()) ==
               EMSCRIPTEN_RESULT_SUCCESS;
}

void WebSocketChannel::close(int code) {
    if (socket_ > 0 && open_) {
        // Only 1000 and 3000-4999 may be sent by an application.
        emscripten_websocket_close(socket_, code == 1000 ? 1000 : 4000, "");
    }
}

EM_BOOL WebSocketChannel::onOpen(int,
                                 const EmscriptenWebSocketOpenEvent *,
                                 void *user) {
    auto &self = *static_cast<WebSocketChannel *>(user);
    self.open_ = true;
    if (self.events_.opened) {
        self.events_.opened();
    }
    return EM_TRUE;
}

EM_BOOL WebSocketChannel::onMessage(int,
                                    const EmscriptenWebSocketMessageEvent *e,
                                    void *user) {
    auto &self = *static_cast<WebSocketChannel *>(user);
    if (e->isText) {
        self.close(1000); // The protocol has no text messages.
        return EM_TRUE;
    }
    if (self.events_.message) {
        self.events_.message(std::string_view(
            reinterpret_cast<const char *>(e->data), e->numBytes));
    }
    return EM_TRUE;
}

EM_BOOL WebSocketChannel::onClose(int,
                                  const EmscriptenWebSocketCloseEvent *e,
                                  void *user) {
    auto &self = *static_cast<WebSocketChannel *>(user);
    self.open_ = false;
    if (!self.closedReported_) {
        self.closedReported_ = true;
        if (self.events_.closed) {
            self.events_.closed(e->code);
        }
    }
    return EM_TRUE;
}

EM_BOOL WebSocketChannel::onError(int,
                                  const EmscriptenWebSocketErrorEvent *,
                                  void *user) {
    // A close event always follows; report the failure there (code 1006).
    (void)user;
    return EM_TRUE;
}
