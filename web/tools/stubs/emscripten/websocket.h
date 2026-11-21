// Stand-in for <emscripten/websocket.h> (type-check only, see ../emscripten.h).
#pragma once
#include <cstdint>
using EM_BOOL = int;
using EMSCRIPTEN_WEBSOCKET_T = int;
struct EmscriptenWebSocketOpenEvent {};
struct EmscriptenWebSocketMessageEvent {
    const unsigned char *data;
    std::uint32_t numBytes;
    EM_BOOL isText;
};
struct EmscriptenWebSocketCloseEvent {
    EM_BOOL wasClean;
    unsigned short code;
    char reason[1024];
};
struct EmscriptenWebSocketErrorEvent {};
