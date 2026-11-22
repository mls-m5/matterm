#pragma once

// A single-line text field model for the cell UI (docs/TABS_PLAN.md section 8):
// code points, a cursor, the usual editing keys. No drawing, no key tables: the
// owner maps keys to these calls, so tests need no window.
#include <algorithm>
#include <cstddef>
#include <string>

namespace frontend::ui {

inline std::string encodeUtf8(const std::u32string &text) {
    std::string out;
    for (const char32_t cp : text) {
        if (cp < 0x80) {
            out += char(cp);
        }
        else if (cp < 0x800) {
            out += char(0xc0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3f));
        }
        else if (cp < 0x10000) {
            out += char(0xe0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3f));
            out += char(0x80 | (cp & 0x3f));
        }
        else {
            out += char(0xf0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3f));
            out += char(0x80 | ((cp >> 6) & 0x3f));
            out += char(0x80 | (cp & 0x3f));
        }
    }
    return out;
}

class TextInput {
public:
    explicit TextInput(std::size_t maxLength = 200)
        : maxLength_(maxLength) {}

    const std::u32string &text() const {
        return text_;
    }
    std::string utf8() const {
        return encodeUtf8(text_);
    }
    std::size_t cursor() const {
        return cursor_;
    }
    bool empty() const {
        return text_.empty();
    }

    // A printable code point at the cursor; control characters and anything past
    // the length limit are refused. True if the text changed.
    bool insert(char32_t cp) {
        if (cp < 32 || cp == 127 || (cp >= 0x80 && cp < 0xa0) ||
            cp > 0x10ffff || (cp >= 0xd800 && cp < 0xe000) ||
            text_.size() >= maxLength_) {
            return false;
        }
        text_.insert(text_.begin() + std::ptrdiff_t(cursor_), cp);
        ++cursor_;
        return true;
    }
    bool backspace() {
        if (cursor_ == 0) {
            return false;
        }
        text_.erase(text_.begin() + std::ptrdiff_t(cursor_ - 1));
        --cursor_;
        return true;
    }
    bool erase() { // The key Delete.
        if (cursor_ >= text_.size()) {
            return false;
        }
        text_.erase(text_.begin() + std::ptrdiff_t(cursor_));
        return true;
    }
    void left() {
        cursor_ = cursor_ > 0 ? cursor_ - 1 : 0;
    }
    void right() {
        cursor_ = std::min(cursor_ + 1, text_.size());
    }
    void home() {
        cursor_ = 0;
    }
    void end() {
        cursor_ = text_.size();
    }
    bool clear() {
        const bool had = !text_.empty();
        text_.clear();
        cursor_ = 0;
        return had;
    }

    // The first code point to show in a field `width` cells wide so that the
    // cursor (one cell past the text at the end) stays visible.
    std::size_t firstVisible(std::size_t width) const {
        if (width == 0 || cursor_ < width) {
            return 0;
        }
        return cursor_ + 1 - width;
    }

private:
    std::u32string text_;
    std::size_t cursor_ = 0;
    std::size_t maxLength_;
};

} // namespace frontend::ui
