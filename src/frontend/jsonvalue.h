#pragma once

// A small strict JSON document: read (refusing anything not JSON, duplicate
// keys, bad UTF-8, lone surrogates, too deep or too large input) and written
// back, keeping the order of the members and every value it does not understand
// (numbers keep their text). For files the program reads and later writes again
// without losing what other versions or the user put there (settings.json). No
// dependencies.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frontend::json {

class Value {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };

    static Value null() {
        return Value{};
    }
    static Value boolean(bool b) {
        Value v;
        v.kind_ = Kind::Bool;
        v.bool_ = b;
        return v;
    }
    static Value number(std::int64_t n) {
        Value v;
        v.kind_ = Kind::Number;
        v.text_ = std::to_string(n);
        return v;
    }
    static Value text(std::string s) {
        Value v;
        v.kind_ = Kind::String;
        v.text_ = std::move(s);
        return v;
    }
    static Value array() {
        Value v;
        v.kind_ = Kind::Array;
        return v;
    }
    static Value object() {
        Value v;
        v.kind_ = Kind::Object;
        return v;
    }
    // A number as it was written (already checked to be JSON).
    static Value rawNumber(std::string raw) {
        Value v;
        v.kind_ = Kind::Number;
        v.text_ = std::move(raw);
        return v;
    }

    Kind kind() const {
        return kind_;
    }
    bool isNull() const {
        return kind_ == Kind::Null;
    }
    bool isObject() const {
        return kind_ == Kind::Object;
    }
    bool isArray() const {
        return kind_ == Kind::Array;
    }
    std::optional<bool> boolean() const {
        return kind_ == Kind::Bool ? std::optional<bool>(bool_) : std::nullopt;
    }
    std::optional<std::string> string() const {
        return kind_ == Kind::String ? std::optional<std::string>(text_)
                                     : std::nullopt;
    }
    // Only a whole number of at most 15 digits.
    std::optional<std::int64_t> integer() const {
        if (kind_ != Kind::Number || text_.empty() || text_.size() > 16) {
            return std::nullopt;
        }
        std::size_t i = text_[0] == '-' ? 1 : 0;
        if (i >= text_.size()) {
            return std::nullopt;
        }
        std::int64_t n = 0;
        for (; i < text_.size(); ++i) {
            if (text_[i] < '0' || text_[i] > '9') {
                return std::nullopt;
            }
            n = n * 10 + (text_[i] - '0');
        }
        return text_[0] == '-' ? -n : n;
    }
    const std::string &rawText() const {
        return text_;
    }

    const std::vector<std::pair<std::string, Value>> &members() const {
        return members_;
    }
    const std::vector<Value> &items() const {
        return items_;
    }
    std::vector<Value> &items() {
        return items_;
    }
    const Value *find(std::string_view key) const {
        for (const auto &m : members_) {
            if (m.first == key) {
                return &m.second;
            }
        }
        return nullptr;
    }
    Value *find(std::string_view key) {
        for (auto &m : members_) {
            if (m.first == key) {
                return &m.second;
            }
        }
        return nullptr;
    }
    // Replaces the member in place, else adds it at the end (objects only).
    void set(const std::string &key, Value value) {
        if (Value *existing = find(key)) {
            *existing = std::move(value);
        }
        else {
            members_.emplace_back(key, std::move(value));
        }
    }
    bool erase(std::string_view key) {
        for (auto it = members_.begin(); it != members_.end(); ++it) {
            if (it->first == key) {
                members_.erase(it);
                return true;
            }
        }
        return false;
    }

private:
    friend struct Parser;
    Kind kind_ = Kind::Null;
    bool bool_ = false;
    std::string text_; // String contents, or the text of a number.
    std::vector<Value> items_;
    std::vector<std::pair<std::string, Value>> members_;
};

constexpr std::size_t kMaxBytes =
    1u << 20; // A settings file is small; anything larger is not one.
constexpr int kMaxDepth = 24;
constexpr std::size_t kMaxValues = 50000;

struct Parser {
    std::string_view s;
    std::size_t pos = 0;
    std::size_t count = 0;
    std::string error;

    bool fail(const std::string &why) {
        if (error.empty()) {
            error = why + " (at byte " + std::to_string(pos) + ")";
        }
        return false;
    }
    void space() {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\n' ||
                                  s[pos] == '\t' || s[pos] == '\r')) {
            ++pos;
        }
    }
    bool eat(char c) {
        space();
        if (pos < s.size() && s[pos] == c) {
            ++pos;
            return true;
        }
        return false;
    }
    static void appendUtf8(std::string &out, char32_t cp) {
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
    static bool validUtf8(std::string_view t) {
        for (std::size_t i = 0; i < t.size();) {
            const auto b = static_cast<unsigned char>(t[i]);
            std::size_t n = b < 0x80         ? 1
                            : (b >> 5) == 6  ? 2
                            : (b >> 4) == 14 ? 3
                            : (b >> 3) == 30 ? 4
                                             : 0;
            if (n == 0 || i + n > t.size()) {
                return false;
            }
            char32_t cp = n == 1 ? b : b & (0xFFu >> (n + 1));
            for (std::size_t k = 1; k < n; ++k) {
                const auto c = static_cast<unsigned char>(t[i + k]);
                if ((c & 0xC0) != 0x80) {
                    return false;
                }
                cp = (cp << 6) | (c & 0x3F);
            }
            if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) ||
                (n == 4 && cp < 0x10000) || cp > 0x10FFFF ||
                (cp >= 0xD800 && cp <= 0xDFFF)) {
                return false;
            }
            i += n;
        }
        return true;
    }
    bool hex4(char32_t &out) {
        if (pos + 4 > s.size()) {
            return fail("bad \\u escape");
        }
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char h = s[pos++];
            out <<= 4;
            if (h >= '0' && h <= '9') {
                out |= char32_t(h - '0');
            }
            else if (h >= 'a' && h <= 'f') {
                out |= char32_t(h - 'a' + 10);
            }
            else if (h >= 'A' && h <= 'F') {
                out |= char32_t(h - 'A' + 10);
            }
            else {
                return fail("bad \\u escape");
            }
        }
        return true;
    }
    bool string(std::string &out) {
        if (!eat('"')) {
            return fail("a string was expected");
        }
        out.clear();
        while (pos < s.size()) {
            const char c = s[pos++];
            if (c == '"') {
                return validUtf8(out) ? true : fail("the string is not UTF-8");
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail("a control character in a string");
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos >= s.size()) {
                return fail("the string ends in a backslash");
            }
            const char e = s[pos++];
            switch (e) {
            case '"':
            case '\\':
            case '/':
                out += e;
                break;
            case 'n':
                out += '\n';
                break;
            case 't':
                out += '\t';
                break;
            case 'r':
                out += '\r';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'u': {
                char32_t cp = 0;
                if (!hex4(cp)) {
                    return false;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    char32_t low = 0;
                    if (pos + 2 > s.size() || s[pos] != '\\' ||
                        s[pos + 1] != 'u') {
                        return fail("a lone surrogate");
                    }
                    pos += 2;
                    if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                        return fail("a lone surrogate");
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return fail("a lone surrogate");
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                return fail("an unknown escape");
            }
        }
        return fail("the string is not closed");
    }
    bool number(Value &out) {
        const std::size_t begin = pos;
        if (pos < s.size() && s[pos] == '-') {
            ++pos;
        }
        const auto digits = [&] {
            const std::size_t from = pos;
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
                ++pos;
            }
            return pos - from;
        };
        if (pos < s.size() && s[pos] == '0') {
            ++pos;
        }
        else if (digits() == 0) {
            return fail("a number was expected");
        }
        if (pos < s.size() && s[pos] == '.') {
            ++pos;
            if (digits() == 0) {
                return fail("digits expected after the decimal point");
            }
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            ++pos;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
                ++pos;
            }
            if (digits() == 0) {
                return fail("digits expected in the exponent");
            }
        }
        if (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
            return fail("a number may not start with 0");
        }
        out = Value::rawNumber(std::string(s.substr(begin, pos - begin)));
        return true;
    }
    bool literal(std::string_view word) {
        if (s.substr(pos, word.size()) == word) {
            pos += word.size();
            return true;
        }
        return fail("not JSON");
    }
    bool value(int depth, Value &out) {
        if (depth > kMaxDepth) {
            return fail("nested too deep");
        }
        if (++count > kMaxValues) {
            return fail("too many values");
        }
        space();
        if (pos >= s.size()) {
            return fail("the document ends too early");
        }
        const char c = s[pos];
        if (c == '"') {
            std::string text;
            if (!string(text)) {
                return false;
            }
            out = Value::text(std::move(text));
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return number(out);
        }
        if (c == 't') {
            out = Value::boolean(true);
            return literal("true");
        }
        if (c == 'f') {
            out = Value::boolean(false);
            return literal("false");
        }
        if (c == 'n') {
            out = Value::null();
            return literal("null");
        }
        if (c == '[') {
            ++pos;
            out = Value::array();
            if (eat(']')) {
                return true;
            }
            for (;;) {
                Value item;
                if (!value(depth + 1, item)) {
                    return false;
                }
                out.items_.push_back(std::move(item));
                if (eat(',')) {
                    continue;
                }
                if (eat(']')) {
                    return true;
                }
                return fail("a comma or ] was expected");
            }
        }
        if (c == '{') {
            ++pos;
            out = Value::object();
            if (eat('}')) {
                return true;
            }
            for (;;) {
                std::string key;
                if (!string(key)) {
                    return false;
                }
                if (!eat(':')) {
                    return fail("a colon was expected");
                }
                if (out.find(key)) {
                    return fail("the key \"" + key + "\" is given twice");
                }
                Value item;
                if (!value(depth + 1, item)) {
                    return false;
                }
                out.members_.emplace_back(std::move(key), std::move(item));
                if (eat(',')) {
                    continue;
                }
                if (eat('}')) {
                    return true;
                }
                return fail("a comma or } was expected");
            }
        }
        return fail("not JSON");
    }
};

// The document, or nullopt (with the reason in `error` when given).
inline std::optional<Value> parse(std::string_view text,
                                  std::string *error = nullptr) {
    Parser parser;
    parser.s = text;
    Value root;
    bool ok = text.size() <= kMaxBytes ||
              parser.fail("the document is larger than 1 MiB");
    ok = ok && parser.value(0, root);
    if (ok) {
        parser.space();
        ok = parser.pos == text.size() ||
             parser.fail("something follows the document");
    }
    if (!ok) {
        if (error) {
            *error = parser.error.empty() ? "not JSON" : parser.error;
        }
        return std::nullopt;
    }
    return root;
}

namespace detail {
inline void dumpString(std::string &out, const std::string &text) {
    static const char *hex = "0123456789abcdef";
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += "\\u00";
                out += hex[(static_cast<unsigned char>(c) >> 4) & 15];
                out += hex[static_cast<unsigned char>(c) & 15];
            }
            else {
                out += c;
            }
        }
    }
    out += '"';
}

inline void dump(std::string &out, const Value &v, bool pretty, int indent) {
    const auto newline = [&](int level) {
        if (pretty) {
            out += '\n';
            out.append(std::size_t(level) * 2, ' ');
        }
    };
    switch (v.kind()) {
    case Value::Kind::Null:
        out += "null";
        break;
    case Value::Kind::Bool:
        out += v.boolean().value_or(false) ? "true" : "false";
        break;
    case Value::Kind::Number:
        out += v.rawText();
        break;
    case Value::Kind::String:
        dumpString(out, v.string().value_or(""));
        break;
    case Value::Kind::Array:
        if (v.items().empty()) {
            out += "[]";
            break;
        }
        out += '[';
        for (std::size_t i = 0; i < v.items().size(); ++i) {
            if (i) {
                out += ',';
            }
            newline(indent + 1);
            dump(out, v.items()[i], pretty, indent + 1);
        }
        newline(indent);
        out += ']';
        break;
    case Value::Kind::Object:
        if (v.members().empty()) {
            out += "{}";
            break;
        }
        out += '{';
        for (std::size_t i = 0; i < v.members().size(); ++i) {
            if (i) {
                out += ',';
            }
            newline(indent + 1);
            dumpString(out, v.members()[i].first);
            out += pretty ? ": " : ":";
            dump(out, v.members()[i].second, pretty, indent + 1);
        }
        newline(indent);
        out += '}';
        break;
    }
}
} // namespace detail

// Compact, or indented by two spaces with a final newline.
inline std::string dump(const Value &v, bool pretty = false) {
    std::string out;
    detail::dump(out, v, pretty, 0);
    if (pretty) {
        out += '\n';
    }
    return out;
}

} // namespace frontend::json
