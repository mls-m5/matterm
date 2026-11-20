#include "chattext.h"

#include <algorithm>

namespace chat {
namespace {

constexpr char32_t kReplacement = 0xFFFD;

// Decodes one code point at `i`; returns the number of bytes used, or 0 when
// the bytes there are not strict UTF-8.
std::size_t decodeOne(std::string_view s, std::size_t i, char32_t &out) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) {
        out = b0;
        return 1;
    }
    std::size_t length = 0;
    char32_t value = 0;
    char32_t minimum = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        length = 2;
        value = b0 & 0x1F;
        minimum = 0x80;
    }
    else if (b0 >= 0xE0 && b0 <= 0xEF) {
        length = 3;
        value = b0 & 0x0F;
        minimum = 0x800;
    }
    else if (b0 >= 0xF0 && b0 <= 0xF4) {
        length = 4;
        value = b0 & 0x07;
        minimum = 0x10000;
    }
    else {
        return 0;
    }
    if (i + length > s.size()) {
        return 0;
    }
    for (std::size_t k = 1; k < length; ++k) {
        const auto b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xC0) != 0x80) {
            return 0;
        }
        value = (value << 6) | (b & 0x3F);
    }
    if (value < minimum || value > 0x10FFFF ||
        (value >= 0xD800 && value <= 0xDFFF)) {
        return 0;
    }
    out = value;
    return length;
}

void append(std::string &out, char32_t c) {
    if (c < 0x80) {
        out += char(c);
    }
    else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    }
    else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
    else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

bool isReserved(const std::string &key) {
    static const char *const names[] = {
        "server", "admin", "system", "root", "you", "me", "moderator"};
    for (const char *n : names) {
        if (key == n) {
            return true;
        }
    }
    return key.compare(0, 3, "mod") == 0;
}

} // namespace

bool decodeUtf8Strict(std::string_view bytes, std::u32string &out) {
    out.clear();
    std::size_t i = 0;
    while (i < bytes.size()) {
        char32_t c = 0;
        const auto used = decodeOne(bytes, i, c);
        if (used == 0) {
            return false;
        }
        out += c;
        i += used;
    }
    return true;
}

std::string encodeUtf8(const std::u32string &text) {
    std::string out;
    for (const char32_t c : text) {
        append(out, c);
    }
    return out;
}

bool isControlOrFormat(char32_t c) {
    return c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x00AD || c == 0x034F ||
           c == 0x061C || c == 0x180E || (c >= 0x200B && c <= 0x200F) ||
           (c >= 0x2028 && c <= 0x202E) || (c >= 0x2060 && c <= 0x206F) ||
           (c >= 0xFE00 && c <= 0xFE0F) || c == 0xFEFF ||
           (c >= 0xFFF9 && c <= 0xFFFB) || (c >= 0xE0000 && c <= 0xE0FFF);
}

bool isAllowedChar(char32_t c) {
    if (c >= 0x20 && c <= 0x7E) {
        return true;
    }
    if (c >= 0xC0 && c <= 0xFF) {
        return c != 0xD7 && c != 0xF7; // Not the multiplication/division signs.
    }
    return c >= 0x100 && c <= 0x17F; // Latin Extended-A.
}

TextResult checkText(std::string_view utf8, const TextRules &rules) {
    TextResult result;
    std::u32string cps;
    if (!decodeUtf8Strict(utf8, cps)) {
        result.error = TextError::BadUtf8;
        return result;
    }
    result.chars = cps.size();
    if (cps.size() > rules.maxChars) {
        result.error = TextError::TooLong;
        return result;
    }
    for (const char32_t c : cps) {
        if (!isAllowedChar(c)) {
            result.error = TextError::Forbidden;
            return result;
        }
    }
    if (cps.empty()) {
        result.error = rules.allowEmpty ? TextError::None : TextError::Empty;
        return result;
    }
    std::size_t run = 1;
    for (std::size_t i = 1; i < cps.size(); ++i) {
        run = cps[i] == cps[i - 1] ? run + 1 : 1;
        if (run > kMaxRun) {
            result.error = TextError::Repeated;
            return result;
        }
    }
    const bool onlySpaces = std::all_of(
        cps.begin(), cps.end(), [](char32_t c) { return c == U' '; });
    if (!rules.allowEdgeSpaces) {
        if (onlySpaces) {
            result.error = TextError::OnlySpaces;
        }
        else if (cps.front() == U' ' || cps.back() == U' ') {
            result.error = TextError::EdgeSpace;
        }
    }
    return result;
}

TextResult checkMessage(std::string_view utf8) {
    return checkText(utf8, TextRules{});
}

TextResult checkDraft(std::string_view utf8) {
    TextRules rules;
    rules.allowEdgeSpaces = true;
    rules.allowEmpty = true;
    return checkText(utf8, rules);
}

std::string trimSpaces(std::string_view text) {
    const auto first = text.find_first_not_of(' ');
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(' ');
    return std::string(text.substr(first, last - first + 1));
}

NameError checkName(std::string_view name) {
    if (name.size() < kMinNameChars || name.size() > kMaxNameChars) {
        return NameError::Length;
    }
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return NameError::Characters;
        }
    }
    return isReserved(nameKey(name)) ? NameError::Reserved : NameError::None;
}

std::string nameKey(std::string_view name) {
    std::string key(name);
    for (char &c : key) {
        if (c >= 'A' && c <= 'Z') {
            c = char(c - 'A' + 'a');
        }
    }
    return key;
}

std::string sanitizeIncoming(std::string_view bytes, std::size_t maxChars) {
    std::string out;
    std::size_t chars = 0;
    std::size_t i = 0;
    while (i < bytes.size() && chars < maxChars) {
        char32_t c = 0;
        auto used = decodeOne(bytes, i, c);
        if (used == 0) {
            used = 1;
            c = kReplacement;
        }
        i += used;
        if (isControlOrFormat(c)) {
            continue;
        }
        if (!isAllowedChar(c)) {
            c = kReplacement;
        }
        append(out, c);
        ++chars;
    }
    return out;
}

} // namespace chat
