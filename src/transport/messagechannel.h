#pragma once

#include <functional>
#include <string_view>

// A reliable, ordered, message-oriented channel to the server: a WebSocket, a
// WebRTC data channel, or an in-memory pair in tests. One send is one message;
// the protocol adds no framing of its own. No threading is implied: callbacks
// run on the thread that drives the channel and must not re-enter it.
class MessageChannel {
public:
    struct Events {
        std::function<void()> opened;
        // Views are valid only during the call.
        std::function<void(std::string_view)> message;
        // `code` is a WebSocket close code, or 1006 for an abnormal close.
        std::function<void(int code)> closed;
    };

    virtual ~MessageChannel() = default;
    virtual void setEvents(Events events) = 0;
    // False when the message cannot be queued (closed channel, full buffer).
    virtual bool send(std::string_view bytes) = 0;
    virtual void close(int code) = 0;
};
