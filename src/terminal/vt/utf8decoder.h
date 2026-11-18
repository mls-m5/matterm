#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Incremental Unicode decoding only, not a VT parser. Invalid UTF-8 is replaced
// with U+FFFD. Keep an instance across byte chunks; finish() flushes a
// truncated sequence at end of stream, then resets the decoder.
class Utf8Decoder {
public:
    std::u32string feed(std::string_view bytes);
    std::u32string finish();
    void reset() noexcept;

private:
    std::uint32_t value_ = 0;
    unsigned remaining_ = 0;
    unsigned nextMin_ = 0x80;
    unsigned nextMax_ = 0xbf;
};
