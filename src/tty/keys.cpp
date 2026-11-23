#include "tty/keys.h"

#include <algorithm>
#include <cstdlib>

namespace tty {
namespace {
Key simple(Key::Type type) {
    Key key;
    key.type = type;
    return key;
}
// The F-key of the "CSI n ~" codes 11..24 (rxvt/xterm numbering, with gaps).
int functionNumber(int code) {
    switch (code) {
    case 11:
        return 1;
    case 12:
        return 2;
    case 13:
        return 3;
    case 14:
        return 4;
    case 15:
        return 5;
    case 17:
        return 6;
    case 18:
        return 7;
    case 19:
        return 8;
    case 20:
        return 9;
    case 21:
        return 10;
    case 23:
        return 11;
    default:
        return 12;
    }
}
} // namespace

void KeyDecoder::textByte(unsigned char byte, std::vector<Key> &out) {
    if (need_ == 0) {
        if (byte < 0x80) {
            Key key;
            key.text.assign(1, char(byte));
            out.push_back(std::move(key));
            return;
        }
        need_ = byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : byte >= 0xC0 ? 2 : 0;
        if (need_ == 0) {
            return; // Stray continuation byte.
        }
        pending_.assign(1, char(byte));
        return;
    }
    if ((byte & 0xC0) != 0x80) { // Broken sequence: drop it, reread the byte.
        pending_.clear();
        need_ = 0;
        textByte(byte, out);
        return;
    }
    pending_.push_back(char(byte));
    if (pending_.size() == need_) {
        Key key;
        key.text = std::move(pending_);
        pending_.clear();
        need_ = 0;
        out.push_back(std::move(key));
    }
}

void KeyDecoder::finishCsi(std::vector<Key> &out) {
    const std::size_t before = out.size();
    // params_ holds the parameter and intermediate bytes plus the final byte.
    const char final = params_.back();
    std::string args = params_.substr(0, params_.size() - 1);
    auto number = [&](std::size_t index, int fallback) {
        std::size_t begin = 0;
        for (std::size_t i = 0; i < index; ++i) {
            begin = args.find(';', begin);
            if (begin == std::string::npos) {
                return fallback;
            }
            ++begin;
        }
        const auto end = args.find(';', begin);
        const auto text = args.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (text.empty()) {
            return fallback;
        }
        // Parameters come from the (untrusted) terminal input: clamp, never
        // overflow.
        const long value = std::strtol(text.c_str(), nullptr, 10);
        return int(std::clamp(value, 0L, 100000L));
    };
    if (!args.empty() && args[0] == '<' && (final == 'M' || final == 'm')) {
        args.erase(0, 1);
        Key key = simple(Key::Type::Mouse);
        const int code = number(0, 0);
        key.column = number(1, 1) - 1;
        key.row = number(2, 1) - 1;
        key.press = final == 'M';
        key.motion = (code & 32) != 0;
        key.button = (code & 64) ? 64 + (code & 1) : (code & 3);
        out.push_back(std::move(key));
        return;
    }
    switch (final) {
    case 'A':
        out.push_back(simple(Key::Type::Up));
        break;
    case 'B':
        out.push_back(simple(Key::Type::Down));
        break;
    case 'C':
        out.push_back(simple(Key::Type::Right));
        break;
    case 'D':
        out.push_back(simple(Key::Type::Left));
        break;
    case 'H':
        out.push_back(simple(Key::Type::Home));
        break;
    case 'F':
        out.push_back(simple(Key::Type::End));
        break;
    case 'Z': {
        Key key = simple(Key::Type::Tab);
        key.shift = true;
        out.push_back(std::move(key));
        break;
    }
    case 'P':
    case 'Q':
    case 'R':
    case 'S': { // F1..F4 with a modifier ("1;5P"); "24;80R" is a cursor report.
        if (args.size() < 2 || args[0] != '1' || args[1] != ';') {
            break;
        }
        Key key = simple(Key::Type::Function);
        key.number = 1 + (final - 'P');
        out.push_back(std::move(key));
        break;
    }
    case '~':
        switch (number(0, 0)) {
        case 2:
            out.push_back(simple(Key::Type::Insert));
            break;
        case 11:
        case 12:
        case 13:
        case 14:
        case 15:
        case 17:
        case 18:
        case 19:
        case 20:
        case 21:
        case 23:
        case 24: {
            Key key = simple(Key::Type::Function);
            key.number = functionNumber(number(0, 0));
            out.push_back(std::move(key));
            break;
        }
        case 1:
        case 7:
            out.push_back(simple(Key::Type::Home));
            break;
        case 4:
        case 8:
            out.push_back(simple(Key::Type::End));
            break;
        case 3:
            out.push_back(simple(Key::Type::Delete));
            break;
        case 5:
            out.push_back(simple(Key::Type::PageUp));
            break;
        case 6:
            out.push_back(simple(Key::Type::PageDown));
            break;
        case 200:
            pasting_ = true;
            break;
        case 201:
            pasting_ = false;
            break;
        default:
            break;
        }
        break;
    default:
        break; // Cursor position reports and other replies.
    }
    // A modifier parameter ("1;5D", "3;5~"): 1 + (shift 1, alt 2, ctrl 4).
    const int modifier = number(1, 1) - 1;
    for (std::size_t i = before; i < out.size(); ++i) {
        if (modifier & 1) {
            out[i].shift = true;
        }
        if (modifier & 2) {
            out[i].alt = true;
        }
        if (modifier & 4) {
            out[i].ctrl = true;
        }
    }
}

std::vector<Key> KeyDecoder::feed(std::string_view bytes) {
    std::vector<Key> out;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const std::size_t before = out.size();
        raw_.push_back(bytes[i]);
        step((unsigned char)bytes[i], i + 1 == bytes.size(), out);
        finishUnit(out, before);
    }
    // A lone ESC split from its sequence by the chunk boundary stays pending.
    return out;
}

void KeyDecoder::finishUnit(std::vector<Key> &out, std::size_t before) {
    if (out.size() > before) {
        if (!raw_.empty()) { // (Empty after Alt+key: the inner read has set
                             // it.)
            out[before].raw = std::move(raw_);
        }
        raw_.clear();
    }
    else if (state_ == State::Ground && need_ == 0) {
        raw_.clear(); // A sequence that is no key (a reply): forget its bytes.
    }
}

void KeyDecoder::step(unsigned char byte, bool last, std::vector<Key> &out) {
    {
        switch (state_) {
        case State::Esc:
            state_ = State::Ground;
            if (byte == '[') {
                state_ = State::Csi;
                params_.clear();
            }
            else if (byte == 'O') {
                state_ = State::Ss3;
            }
            else if (byte == ']' || byte == 'P' || byte == '_' || byte == '^' ||
                     byte == 'X') {
                state_ = State::String;
            }
            else if (byte == 0x1b) {
                out.push_back(simple(Key::Type::Escape));
                state_ = State::Esc;
            }
            else { // Alt+key: the byte is read as it would be on its own.
                const char one = char(byte);
                raw_.pop_back(); // feed() adds the byte itself.
                for (auto &key : feed(std::string_view(&one, 1))) {
                    key.alt = true;
                    out.push_back(std::move(key));
                }
            }
            return;
        case State::Csi:
            params_.push_back(char(byte));
            if (byte >= 0x40 && byte <= 0x7e) {
                finishCsi(out);
                state_ = State::Ground;
            }
            else if (params_.size() > 32) {
                state_ = State::Ground;
            }
            return;
        case State::Ss3:
            state_ = State::Ground;
            switch (byte) {
            case 'A':
                out.push_back(simple(Key::Type::Up));
                break;
            case 'B':
                out.push_back(simple(Key::Type::Down));
                break;
            case 'C':
                out.push_back(simple(Key::Type::Right));
                break;
            case 'D':
                out.push_back(simple(Key::Type::Left));
                break;
            case 'H':
                out.push_back(simple(Key::Type::Home));
                break;
            case 'F':
                out.push_back(simple(Key::Type::End));
                break;
            case 'P':
            case 'Q':
            case 'R':
            case 'S': {
                Key key = simple(Key::Type::Function);
                key.number = 1 + (byte - 'P');
                out.push_back(std::move(key));
                break;
            }
            default:
                break;
            }
            return;
        case State::String:
            if (byte == 0x07) {
                state_ = State::Ground;
            }
            else if (byte == 0x1b) {
                state_ = State::StringEsc;
            }
            return;
        case State::StringEsc:
            state_ = byte == 0x1b ? State::StringEsc : State::Ground;
            return;
        case State::Ground:
            break;
        }
        if (byte != '\r' && byte != '\n') {
            afterCr_ = false;
        }
        if (need_ > 0) {
            textByte(byte, out);
            return;
        }
        switch (byte) {
        case 0x1b:
            if (last) {
                out.push_back(simple(Key::Type::Escape));
            }
            else {
                state_ = State::Esc;
            }
            break;
        case '\r':
        case '\n':
            if (byte == '\n' && afterCr_) {
                afterCr_ = false; // The LF of a CR LF pair.
                break;
            }
            afterCr_ = byte == '\r';
            if (pasting_) {
                Key key;
                key.text = "\n";
                out.push_back(std::move(key));
            }
            else {
                out.push_back(simple(Key::Type::Enter));
            }
            break;
        case 0x7f:
        case 0x08:
            out.push_back(simple(Key::Type::Backspace));
            break;
        case '\t':
            out.push_back(simple(Key::Type::Tab));
            break;
        default:
            if (byte < 0x20) {
                Key key = simple(Key::Type::Ctrl);
                key.text.assign(1, char('a' + byte - 1));
                out.push_back(std::move(key));
            }
            else {
                textByte(byte, out);
            }
            break;
        }
    }
}

} // namespace tty
