#pragma once

#include <string_view>

namespace frontend {
// Shared by saved hosts and the SSH tunnel: never accept options or shell text.
inline bool safeSshHost(std::string_view s) {
    if (s.empty() || s.size() > 1024 || s.front() == '-')
        return false;
    for (char c : s) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                           c == '-' || c == '+' || c == '@' || c == ':' ||
                           c == '%' || c == '[' || c == ']' || c == '=' ||
                           c == ',';
        if (!plain)
            return false;
    }
    return true;
}
} // namespace frontend
