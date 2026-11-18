#include "utf8decoder.h"

void Utf8Decoder::reset() noexcept {
    value_ = 0;
    remaining_ = 0;
    nextMin_ = 0x80;
    nextMax_ = 0xbf;
}

std::u32string Utf8Decoder::feed(std::string_view bytes) {
    std::u32string output;
    output.reserve(bytes.size());
    for (std::size_t i = 0; i < bytes.size();) {
        const auto byte = static_cast<unsigned char>(bytes[i]);
        if (remaining_ != 0) {
            if (byte < nextMin_ || byte > nextMax_) {
                output.push_back(U'\ufffd');
                reset();
                // Reprocess this byte: it may begin the next valid character.
                continue;
            }
            value_ = (value_ << 6) | (byte & 0x3f);
            --remaining_;
            nextMin_ = 0x80;
            nextMax_ = 0xbf;
            if (remaining_ == 0) {
                output.push_back(char32_t(value_));
            }
        }
        else if (byte < 0x80) {
            output.push_back(char32_t(byte));
        }
        else if (byte >= 0xc2 && byte <= 0xdf) {
            value_ = byte & 0x1f;
            remaining_ = 1;
        }
        else if (byte >= 0xe0 && byte <= 0xef) {
            value_ = byte & 0x0f;
            remaining_ = 2;
            nextMin_ = byte == 0xe0 ? 0xa0 : 0x80;
            nextMax_ = byte == 0xed ? 0x9f : 0xbf;
        }
        else if (byte >= 0xf0 && byte <= 0xf4) {
            value_ = byte & 0x07;
            remaining_ = 3;
            nextMin_ = byte == 0xf0 ? 0x90 : 0x80;
            nextMax_ = byte == 0xf4 ? 0x8f : 0xbf;
        }
        else {
            output.push_back(U'\ufffd');
        }
        ++i;
    }
    return output;
}

std::u32string Utf8Decoder::finish() {
    std::u32string output;
    if (remaining_ != 0) {
        output.push_back(U'\ufffd');
    }
    reset();
    return output;
}
