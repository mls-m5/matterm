#include "tty/utf8.h"

namespace tty {

void appendUtf8(std::string &out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(char(cp));
    }
    else if (cp < 0x800) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
    else {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

std::u32string decodeUtf8(const std::string &text) {
    std::u32string out;
    for (std::size_t i = 0; i < text.size();) {
        const auto b = (unsigned char)text[i];
        std::size_t n = b < 0x80    ? 1
                        : b >= 0xF0 ? 4
                        : b >= 0xE0 ? 3
                        : b >= 0xC0 ? 2
                                    : 1;
        char32_t cp = n == 1 ? b : b & (0xFF >> (n + 1));
        for (std::size_t k = 1; k < n && i + k < text.size(); ++k) {
            cp = (cp << 6) | ((unsigned char)text[i + k] & 0x3F);
        }
        out.push_back(cp);
        i += n;
    }
    return out;
}

} // namespace tty
