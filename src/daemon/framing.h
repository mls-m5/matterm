#pragma once

// Framing of the local daemon's Unix socket (docs/tasks/daemon.md, docs/PROTOCOL.md "Local
// daemon"): every message is a 4-byte big-endian length (1..16384) followed by that many bytes,
// the same bytes a WebSocket binary message carries. Anything else is a violation and the
// connection is closed. Pure data, no I/O, so both ends and the fuzz tests use the same code.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mtd {

constexpr std::size_t kMaxFrame = 16384; // protocol::maxMessageSize

// Appends one frame to `out`. False (out unchanged) for an empty or oversized payload.
inline bool encodeFrame(std::string_view payload, std::string &out) {
    if (payload.empty() || payload.size() > kMaxFrame) return false;
    const auto n = static_cast<std::uint32_t>(payload.size());
    out.push_back(static_cast<char>(n >> 24));
    out.push_back(static_cast<char>(n >> 16));
    out.push_back(static_cast<char>(n >> 8));
    out.push_back(static_cast<char>(n));
    out.append(payload);
    return true;
}

// Takes bytes as they arrive, in any pieces, and hands out complete frames. After a violation
// (length 0 or over kMaxFrame) it stays failed and takes nothing more.
class FrameDecoder {
public:
    // `onFrame(std::string_view)`: valid during the call. False when the stream is violated.
    template <class OnFrame>
    bool feed(std::string_view data, OnFrame &&onFrame) {
        if (failed_) return false;
        buffer_.append(data);
        std::size_t pos = 0;
        for (;;) {
            if (buffer_.size() - pos < 4) break;
            const auto *p = reinterpret_cast<const unsigned char *>(buffer_.data() + pos);
            const std::size_t length = (std::size_t(p[0]) << 24) | (std::size_t(p[1]) << 16) |
                                       (std::size_t(p[2]) << 8) | std::size_t(p[3]);
            if (length == 0 || length > kMaxFrame) {
                failed_ = true;
                buffer_.clear();
                return false;
            }
            if (buffer_.size() - pos - 4 < length) break;
            // The callback may do anything but must not call feed() again on this decoder.
            onFrame(std::string_view(buffer_.data() + pos + 4, length));
            pos += 4 + length;
        }
        buffer_.erase(0, pos);
        return true;
    }
    bool failed() const { return failed_; }
    // Bytes of an incomplete frame held back.
    std::size_t pending() const { return buffer_.size(); }

private:
    std::string buffer_;
    bool failed_ = false;
};

} // namespace mtd
