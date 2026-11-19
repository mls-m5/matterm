#pragma once

#include "messagechannel.h"

#include <emscripten/websocket.h>

#include <string>

// MessageChannel over the browser's WebSocket (Emscripten's API). Binary
// messages only; a text message closes the connection. The subprotocol
// `matterm-tty.v1` is requested, as docs/PROTOCOL.md requires.
class WebSocketChannel final : public MessageChannel {
public:
    explicit WebSocketChannel(const std::string &url);
    ~WebSocketChannel() override;
    WebSocketChannel(const WebSocketChannel &) = delete;
    WebSocketChannel &operator=(const WebSocketChannel &) = delete;

    void setEvents(Events events) override;
    bool send(std::string_view bytes) override;
    void close(int code) override;

private:
    static EM_BOOL onOpen(int, const EmscriptenWebSocketOpenEvent *, void *);
    static EM_BOOL onMessage(int,
                             const EmscriptenWebSocketMessageEvent *,
                             void *);
    static EM_BOOL onClose(int, const EmscriptenWebSocketCloseEvent *, void *);
    static EM_BOOL onError(int, const EmscriptenWebSocketErrorEvent *, void *);

    EMSCRIPTEN_WEBSOCKET_T socket_ = 0;
    Events events_;
    bool open_ = false;
    bool closedReported_ = false;
};
