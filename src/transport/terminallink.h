#pragma once

#include <functional>
#include <string_view>

// What a terminal front end needs from wherever its terminal lives: a remote
// server (ClientSession), a local PTY, or a scripted simulator. The terminal
// model, parser, and renderer never see a transport.
class TerminalLink {
public:
    struct Events {
        // Terminal output bytes, in order. The receiver must have consumed
        // (parsed) them when the callback returns.
        std::function<void(std::string_view)> output;
        // The program ended. `code` is -1 when it was killed by a signal.
        std::function<void(int code, int signal)> exited;
        // The link is gone (error or lost connection); not sent after exited.
        std::function<void(std::string_view reason)> closed;
    };

    virtual ~TerminalLink() = default;
    virtual void setEvents(Events events) = 0;
    // Bytes for the program, exactly as InputEncoder produced them. False when
    // they cannot be accepted (not connected, closed).
    virtual bool sendInput(std::string_view bytes) = 0;
    virtual void resize(int columns,
                        int rows,
                        int pixelWidth = 0,
                        int pixelHeight = 0) = 0;
    // End the session.
    virtual void close() = 0;
};
