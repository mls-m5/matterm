#pragma once

#include <string>

namespace tty {

void appendUtf8(std::string &out, char32_t codePoint);
// Lenient: malformed bytes become single code points, nothing throws.
std::u32string decodeUtf8(const std::string &text);

} // namespace tty
