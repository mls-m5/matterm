#pragma once

#include "textselection.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace frontend {
namespace url_detail {
inline int hex(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}
inline bool alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}
inline bool ipv6(std::string_view host) {
    const auto groups = [](std::string_view part) -> std::optional<unsigned> {
        if (part.empty()) {
            return 0;
        }
        unsigned count = 0;
        while (true) {
            const auto colon = part.find(':');
            const auto group = part.substr(0, colon);
            if (group.empty() || group.size() > 4) {
                return std::nullopt;
            }
            for (const char c : group) {
                if (hex(c) < 0) {
                    return std::nullopt;
                }
            }
            ++count;
            if (colon == std::string_view::npos) {
                return count;
            }
            part.remove_prefix(colon + 1);
        }
    };
    const auto compression = host.find("::");
    if (compression == std::string_view::npos) {
        return groups(host) == std::optional<unsigned>(8);
    }
    if (host.find("::", compression + 2) != std::string_view::npos) {
        return false;
    }
    const auto left = groups(host.substr(0, compression));
    const auto right = groups(host.substr(compression + 2));
    return left && right && *left + *right < 8;
}
inline bool hostname(std::string_view host) {
    if (host.empty() || host.size() > 253) {
        return false;
    }
    bool numeric = true;
    bool numericLike = true;
    bool ipv4Labels = true;
    unsigned labels = 0;
    const bool dotted = host.find('.') != std::string_view::npos;
    while (true) {
        const auto dot = host.find('.');
        const auto label = host.substr(0, dot);
        if (label.empty() || label.size() > 63 || !alnum(label.front()) ||
            !alnum(label.back())) {
            return false;
        }
        bool digits = true;
        unsigned value = 0;
        for (const char c : label) {
            if (!alnum(c) && c != '-') {
                return false;
            }
            if (c < '0' || c > '9') {
                digits = false;
            }
            else {
                // Bound the value even for long numeric labels.
                value = std::min(256u, value * 10 + unsigned(c - '0'));
            }
        }
        if (digits &&
            (value > 255 || (label.size() > 1 && label.front() == '0'))) {
            ipv4Labels = false;
        }
        const bool hexNumber = label.size() > 2 && label[0] == '0' &&
                               (label[1] == 'x' || label[1] == 'X') &&
                               std::all_of(label.begin() + 2,
                                           label.end(),
                                           [](char c) { return hex(c) >= 0; });
        numericLike = numericLike && (digits || hexNumber);
        numeric = numeric && digits;
        ++labels;
        if (dot == std::string_view::npos) {
            break;
        }
        host.remove_prefix(dot + 1);
    }
    return numeric ? dotted && labels == 4 && ipv4Labels : !numericLike;
}
inline bool space(char32_t cp) {
    // Control characters are not delimiters: reject a token containing them.
    return cp == U' ' || cp == 0xa0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202f || cp == 0x205f || cp == 0x3000;
}
inline std::optional<Selection::Cell> previous(const TerminalModel &model,
                                               Selection::Cell cell) {
    if (cell.column) {
        return Selection::Cell{cell.row, cell.column - 1};
    }
    if (cell.row > model.firstRetainedRow() &&
        model.textRowWrapped(cell.row - 1)) {
        return Selection::Cell{cell.row - 1, model.columns() - 1};
    }
    return std::nullopt;
}
inline std::optional<Selection::Cell> next(const TerminalModel &model,
                                           Selection::Cell cell) {
    if (++cell.column == model.columns()) {
        if (!model.textRowWrapped(cell.row)) {
            return std::nullopt;
        }
        ++cell.row;
        cell.column = 0;
    }
    return model.textCodePoint(cell) ? std::optional<Selection::Cell>(cell)
                                     : std::nullopt;
}
} // namespace url_detail

// Conservative ASCII URI policy. Use percent encoding for non-ASCII paths
// and punycode for international hostnames. Never hand arbitrary schemes or
// shell/JavaScript source to an opener. This is not a general-purpose URL
// parser.
inline bool validHttpUrl(std::string_view url) {
    if (url.size() > 2048) {
        return false;
    }
    std::string scheme;
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string_view::npos || schemeEnd > 5) {
        return false;
    }
    for (char c : url.substr(0, schemeEnd)) {
        scheme += c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
    }
    if (scheme != "http" && scheme != "https") {
        return false;
    }
    for (std::size_t i = 0; i < url.size(); ++i) {
        const unsigned char c = url[i];
        if (!url_detail::alnum(char(c)) &&
            std::string_view("-._~:/?#[]@!$&()*+,;=%").find(char(c)) ==
                std::string_view::npos) {
            return false;
        }
        if (c == '%') {
            if (i + 2 >= url.size()) {
                return false;
            }
            const int hi = url_detail::hex(url[i + 1]),
                      lo = url_detail::hex(url[i + 2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            const int decoded = hi * 16 + lo;
            if (decoded < 32 || decoded == 127 || decoded == '"' ||
                decoded == '\'' || decoded == '\\') {
                return false;
            }
            i += 2;
        }
    }
    auto authority = url.substr(schemeEnd + 3);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return false;
    }
    std::string_view port;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos ||
            !url_detail::ipv6(authority.substr(1, close - 1))) {
            return false;
        }
        const auto rest = authority.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') {
                return false;
            }
            port = rest.substr(1);
            if (port.empty()) {
                return false;
            }
        }
    }
    else {
        const auto colon = authority.find(':');
        if (!url_detail::hostname(authority.substr(0, colon))) {
            return false;
        }
        if (colon != std::string_view::npos) {
            port = authority.substr(colon + 1);
            if (port.empty()) {
                return false;
            }
        }
    }
    unsigned number = 0;
    if (port.size() > 5) {
        return false;
    }
    for (const char c : port) {
        if (c < '0' || c > '9') {
            return false;
        }
        number = number * 10 + unsigned(c - '0');
    }
    return port.empty() || (number > 0 && number <= 65535);
}

struct UrlMatch {
    std::string text;
    Selection cells;

    void underline(const TerminalModel &model, RenderFrame &frame) const {
        auto overlay = frame;
        overlay.selectionBegin = overlay.selectionEnd = 0;
        cells.apply(model, overlay);
        frame.underlineBegin = overlay.selectionBegin;
        frame.underlineEnd = overlay.selectionEnd;
    }
};

inline bool localLinkGesture(bool control,
                             bool shift,
                             bool applicationMouse,
                             bool captured = false) {
    return control && !captured && (!applicationMouse || shift);
}

inline std::optional<UrlMatch> urlAt(const TerminalModel &model,
                                     Selection::Cell cell) {
    const auto cp = model.textCodePoint(cell);
    if (!cp || url_detail::space(*cp)) {
        return std::nullopt;
    }
    // Bound work even on a malicious megabyte-long soft-wrapped token. Extra
    // room beyond the URL cap permits surrounding prose punctuation.
    constexpr std::size_t tokenLimit = 4096;
    auto first = cell;
    std::size_t count = 1;
    while (const auto prev = url_detail::previous(model, first)) {
        if (url_detail::space(*model.textCodePoint(*prev))) {
            break;
        }
        if (++count > tokenLimit) {
            return std::nullopt;
        }
        first = *prev;
    }
    std::string token;
    for (auto current = std::optional<Selection::Cell>(first); current;
         current = url_detail::next(model, *current)) {
        const auto value = *model.textCodePoint(*current);
        if (url_detail::space(value)) {
            break;
        }
        if (token.size() == tokenLimit || value > 127) {
            return std::nullopt;
        }
        token += char(value);
    }
    std::size_t begin = 0, end = token.size();
    while (begin < end && std::string_view("([{<\"'").find(token[begin]) !=
                              std::string_view::npos) {
        ++begin;
    }
    int balance[3]{};
    constexpr std::string_view open = "([{", close = ")]}";
    for (std::size_t i = begin; i < end; ++i) {
        for (std::size_t pair = 0; pair < 3; ++pair) {
            balance[pair] += token[i] == open[pair];
            balance[pair] -= token[i] == close[pair];
        }
    }
    while (end > begin) {
        const char tail = token[end - 1];
        if (std::string_view(".,;:!?>\"'").find(tail) !=
            std::string_view::npos) {
            --end;
        }
        else if (const auto pair = close.find(tail);
                 pair != std::string_view::npos && balance[pair] < 0) {
            ++balance[pair];
            --end;
        }
        else {
            break;
        }
    }
    const auto url = std::string_view(token).substr(begin, end - begin);
    if (begin == end || !validHttpUrl(url)) {
        return std::nullopt;
    }
    // ASCII tokens use one byte per cell; traversal above crosses only soft
    // wraps, so trimmed byte offsets map directly back to absolute cell IDs.
    const auto position = [&](std::size_t offset) {
        const auto column = first.column + offset;
        return Selection::Cell{first.row + column / model.columns(),
                               column % model.columns()};
    };
    const auto start = position(begin), last = position(end - 1);
    if (Selection::before(cell, start) || Selection::before(last, cell)) {
        return std::nullopt;
    }
    return UrlMatch{std::string(url), {start, last, model.selectionEpoch()}};
}
} // namespace frontend
