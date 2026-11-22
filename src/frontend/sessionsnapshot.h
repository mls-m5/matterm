#pragma once

// A saved session of the native app (docs/TABS_PLAN.md section 9): tabs with their
// titles, the pane tree with ratios, and per pane the folder, the command that was
// running (only offered again, never started) and optionally the text that was on
// the screen. One JSON document per session, written by the app and read back as
// untrusted input: the reader accepts exactly this schema (version 1) within hard
// limits and refuses anything else as a whole.
//
//   {"version":1,"name":"work","saved":1763800000,"active":0,"tabs":[
//     {"title":"build","focus":0,"layout":NODE}]}
//   NODE := {"pane":{"cwd":"/home/a","command":["vim","x"],"text":"...","terminal":"<32 hex>"}}   all optional
//         | {"split":"h"|"v","ratio":500,"a":NODE,"b":NODE}                h: side by side
//
// The tree shape and its limits are those of the browser's LayoutSnapshot.
#include "layoutsnapshot.h"
#include "paneprobe.h"
#include "tabstrip.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace frontend {

struct SavedPane {
    std::string cwd;
    std::vector<std::string> command; // Offered again, never started.
    std::string text;                 // Visible screen at save time, lines joined by '\n'.
    std::string terminal;             // The daemon terminal it showed (32 hex), empty: none. A detached layout.
};

struct SavedNode {
    bool leaf = true;
    SavedPane pane;
    SplitDirection direction = SplitDirection::SideBySide;
    int permille = 500;
    std::unique_ptr<SavedNode> first, second;

    static std::unique_ptr<SavedNode> leafOf(SavedPane p) {
        auto n = std::make_unique<SavedNode>();
        n->pane = std::move(p);
        return n;
    }
    static std::unique_ptr<SavedNode> split(SplitDirection d,
                                            int permille,
                                            std::unique_ptr<SavedNode> a,
                                            std::unique_ptr<SavedNode> b) {
        auto n = std::make_unique<SavedNode>();
        n->leaf = false;
        n->direction = d;
        n->permille = permille;
        n->first = std::move(a);
        n->second = std::move(b);
        return n;
    }
};

struct SavedTab {
    std::string title; // User title, may be empty.
    std::size_t focus = 0; // Index of the focused pane, left/upper first.
    std::unique_ptr<SavedNode> root;
};

namespace snapshot_detail {

inline void appendUtf8(std::string &out, char32_t cp) {
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

// Strict UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF.
inline bool validUtf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t n = 0;
        char32_t cp = 0;
        if (c < 0x80) { ++i; continue; }
        if (c >= 0xc2 && c <= 0xdf) { n = 1; cp = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { n = 2; cp = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 3; cp = c & 0x07; }
        else return false;
        if (i + n >= s.size()) return false;
        for (std::size_t k = 1; k <= n; ++k) {
            const auto d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3f);
        }
        if ((n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            return false;
        }
        i += n + 1;
    }
    return true;
}

// A code point that must not be stored in a title or screen text: controls (C0, DEL,
// C1) and everything the tab strip refuses (zero width, bidi, tags).
inline bool refused(char32_t cp, bool allowNewline) {
    if (cp == U'\n') return !allowNewline;
    return cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) || displayWidth(cp) == 0;
}

// The text without refused code points, cut to `maxBytes` at a character boundary.
inline std::string clean(const std::string &text, std::size_t maxBytes, bool allowNewline) {
    std::string out;
    if (!validUtf8(text)) return out;
    // Valid UTF-8 (checked above): split into characters and keep the allowed ones.
    for (std::size_t i = 0; i < text.size();) {
        const auto b = static_cast<unsigned char>(text[i]);
        const std::size_t n = b < 0x80 ? 1 : b < 0xe0 ? 2 : b < 0xf0 ? 3 : 4;
        char32_t cp = n == 1 ? b : b & (0xff >> (n + 1));
        for (std::size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3f);
        if (!refused(cp, allowNewline)) {
            if (out.size() + n > maxBytes) break;
            out.append(text, i, n);
        }
        i += n;
    }
    return out;
}

} // namespace snapshot_detail

struct SessionSnapshot {
    static constexpr int kVersion = 1;
    static constexpr std::size_t kMaxFile = 1 << 20;
    static constexpr std::size_t kMaxTitle = 64;
    static constexpr std::size_t kMaxName = 64;
    static constexpr std::size_t kMaxPaneText = 32 * 1024;
    static constexpr std::size_t kMaxTotalText = 256 * 1024;

    std::string name;
    std::int64_t saved = 0; // Unix seconds.
    std::size_t active = 0;
    std::vector<SavedTab> tabs;

    // [A-Za-z0-9._-], 1..64, not starting with '.': a file name without surprises.
    static bool validName(std::string_view n) {
        if (n.empty() || n.size() > kMaxName || n.front() == '.') return false;
        for (const char c : n) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                            c == '.' || c == '_' || c == '-';
            if (!ok) return false;
        }
        return true;
    }
    static std::string cleanTitle(const std::string &t) {
        return snapshot_detail::clean(t, kMaxTitle, false);
    }
    static std::string cleanText(const std::string &t) {
        return snapshot_detail::clean(t, kMaxPaneText, true);
    }

    static std::size_t paneCount(const SavedNode &n) {
        return n.leaf ? 1 : paneCount(*n.first) + paneCount(*n.second);
    }

    bool valid() const {
        if (!validName(name) || tabs.empty() || tabs.size() > LayoutSnapshot::kMaxTabs || active >= tabs.size() ||
            saved < 0) {
            return false;
        }
        std::size_t total = 0, text = 0;
        for (const auto &t : tabs) {
            if (!t.root || cleanTitle(t.title) != t.title || !nodeValid(*t.root, 0, text)) return false;
            const auto n = paneCount(*t.root);
            total += n;
            if (n > LayoutSnapshot::kMaxPanesPerTab || total > LayoutSnapshot::kMaxPanes || t.focus >= n ||
                text > kMaxTotalText) {
                return false;
            }
        }
        return true;
    }

    std::string serialize() const {
        std::string o = "{\"version\":1,\"name\":";
        str(o, name);
        o += ",\"saved\":" + std::to_string(saved) + ",\"active\":" + std::to_string(active) + ",\"tabs\":[";
        for (std::size_t i = 0; i < tabs.size(); ++i) {
            if (i) o += ',';
            o += "{\"title\":";
            str(o, tabs[i].title);
            o += ",\"focus\":" + std::to_string(tabs[i].focus) + ",\"layout\":";
            write(*tabs[i].root, o);
            o += '}';
        }
        return o + "]}";
    }

    // Nothing for text that is not exactly this schema or not valid().
    static std::optional<SessionSnapshot> parse(std::string_view text) {
        if (text.size() > kMaxFile) return std::nullopt;
        Reader r{text};
        auto root = r.value(0);
        r.space();
        if (!root || r.pos != text.size() || !std::holds_alternative<Object>(root->v)) return std::nullopt;
        return fromValue(std::get<Object>(root->v));
    }

private:
    // --- a small JSON reader: objects, arrays, strings, integers, no floats, no null/bool ---
    struct Value;
    using Object = std::vector<std::pair<std::string, Value>>;
    using Array = std::vector<Value>;
    struct Value {
        std::variant<std::int64_t, std::string, Array, Object> v;
    };
    static constexpr int kJsonDepth = 24;
    static constexpr std::size_t kJsonValues = 20000;

    struct Reader {
        std::string_view s;
        std::size_t pos = 0;
        std::size_t count = 0;
        void space() {
            while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\n' || s[pos] == '\t' || s[pos] == '\r')) ++pos;
        }
        bool eat(char c) {
            space();
            if (pos < s.size() && s[pos] == c) { ++pos; return true; }
            return false;
        }
        std::optional<std::string> string() {
            if (!eat('"')) return std::nullopt;
            std::string out;
            while (pos < s.size()) {
                const char c = s[pos++];
                if (c == '"') return snapshot_detail::validUtf8(out) ? std::optional<std::string>(out) : std::nullopt;
                if (static_cast<unsigned char>(c) < 0x20) return std::nullopt;
                if (c != '\\') { out += c; continue; }
                if (pos >= s.size()) return std::nullopt;
                const char e = s[pos++];
                switch (e) {
                case '"': case '\\': case '/': out += e; break;
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (pos + 4 > s.size()) return std::nullopt;
                    char32_t cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = s[pos++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= char32_t(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= char32_t(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= char32_t(h - 'A' + 10);
                        else return std::nullopt;
                    }
                    if (cp >= 0xd800 && cp <= 0xdfff) return std::nullopt; // No surrogate pairs: refuse.
                    snapshot_detail::appendUtf8(out, cp);
                    break;
                }
                default: return std::nullopt;
                }
            }
            return std::nullopt;
        }
        std::optional<Value> value(int depth) {
            if (depth > kJsonDepth || ++count > kJsonValues) return std::nullopt;
            space();
            if (pos >= s.size()) return std::nullopt;
            const char c = s[pos];
            if (c == '"') {
                auto str = string();
                if (!str) return std::nullopt;
                return Value{std::move(*str)};
            }
            if (c == '-' || (c >= '0' && c <= '9')) {
                const bool neg = c == '-';
                if (neg) ++pos;
                std::size_t digits = 0;
                std::int64_t n = 0;
                while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9' && digits < 15) {
                    n = n * 10 + (s[pos++] - '0');
                    ++digits;
                }
                if (digits == 0 || (pos < s.size() && ((s[pos] >= '0' && s[pos] <= '9') || s[pos] == '.' ||
                                                       s[pos] == 'e' || s[pos] == 'E'))) {
                    return std::nullopt;
                }
                return Value{neg ? -n : n};
            }
            if (c == '[') {
                ++pos;
                Array a;
                if (eat(']')) return Value{std::move(a)};
                for (;;) {
                    auto v = value(depth + 1);
                    if (!v) return std::nullopt;
                    a.push_back(std::move(*v));
                    if (eat(',')) continue;
                    if (eat(']')) return Value{std::move(a)};
                    return std::nullopt;
                }
            }
            if (c == '{') {
                ++pos;
                Object o;
                if (eat('}')) return Value{std::move(o)};
                for (;;) {
                    auto k = string();
                    if (!k || !eat(':')) return std::nullopt;
                    for (const auto &e : o) {
                        if (e.first == *k) return std::nullopt; // Duplicate key.
                    }
                    auto v = value(depth + 1);
                    if (!v) return std::nullopt;
                    o.emplace_back(std::move(*k), std::move(*v));
                    if (eat(',')) continue;
                    if (eat('}')) return Value{std::move(o)};
                    return std::nullopt;
                }
            }
            return std::nullopt;
        }
    };

    static const Value *find(const Object &o, std::string_view key) {
        for (const auto &e : o) {
            if (e.first == key) return &e.second;
        }
        return nullptr;
    }
    static bool onlyKeys(const Object &o, std::initializer_list<std::string_view> keys) {
        for (const auto &e : o) {
            bool known = false;
            for (const auto k : keys) known = known || e.first == k;
            if (!known) return false;
        }
        return true;
    }
    static const std::string *asString(const Value *v) {
        return v ? std::get_if<std::string>(&v->v) : nullptr;
    }
    static std::optional<std::int64_t> asInt(const Value *v) {
        if (!v) return std::nullopt;
        if (const auto *n = std::get_if<std::int64_t>(&v->v)) return *n;
        return std::nullopt;
    }

    static std::unique_ptr<SavedNode> readNode(const Value &v, int depth) {
        if (depth > 8) return nullptr;
        const auto *o = std::get_if<Object>(&v.v);
        if (!o) return nullptr;
        if (const Value *p = find(*o, "pane")) {
            const auto *po = std::get_if<Object>(&p->v);
            if (!po || o->size() != 1 || !onlyKeys(*po, {"cwd", "command", "text", "terminal"})) return nullptr;
            SavedPane pane;
            if (const Value *c = find(*po, "cwd")) {
                const auto *s = asString(c);
                if (!s || !plainPath(*s)) return nullptr;
                pane.cwd = *s;
            }
            if (const Value *c = find(*po, "command")) {
                const auto *a = std::get_if<Array>(&c->v);
                if (!a) return nullptr;
                for (const auto &e : *a) {
                    const auto *s = asString(&e);
                    if (!s) return nullptr;
                    pane.command.push_back(*s);
                }
                if (!plainCommand(pane.command)) return nullptr;
            }
            if (const Value *c = find(*po, "text")) {
                const auto *s = asString(c);
                if (!s || cleanText(*s) != *s) return nullptr;
                pane.text = *s;
            }
            if (const Value *c = find(*po, "terminal")) {
                const auto *s = asString(c);
                if (!s || !LayoutSnapshot::validId(*s)) return nullptr;
                pane.terminal = *s;
            }
            return SavedNode::leafOf(std::move(pane));
        }
        if (!onlyKeys(*o, {"split", "ratio", "a", "b"})) return nullptr;
        const auto *dir = asString(find(*o, "split"));
        const auto ratio = asInt(find(*o, "ratio"));
        const Value *a = find(*o, "a"), *b = find(*o, "b");
        if (!dir || (*dir != "h" && *dir != "v") || !ratio || *ratio < 1 || *ratio > 999 || !a || !b) return nullptr;
        auto first = readNode(*a, depth + 1);
        auto second = first ? readNode(*b, depth + 1) : nullptr;
        if (!second) return nullptr;
        return SavedNode::split(*dir == "h" ? SplitDirection::SideBySide : SplitDirection::Stacked,
                                int(*ratio), std::move(first), std::move(second));
    }

    static std::optional<SessionSnapshot> fromValue(const Object &o) {
        if (!onlyKeys(o, {"version", "name", "saved", "active", "tabs"})) return std::nullopt;
        const auto version = asInt(find(o, "version"));
        const auto *name = asString(find(o, "name"));
        const auto saved = asInt(find(o, "saved"));
        const auto active = asInt(find(o, "active"));
        const Value *tabs = find(o, "tabs");
        if (!version || *version != kVersion || !name || !saved || !active || *active < 0 || !tabs) {
            return std::nullopt;
        }
        const auto *list = std::get_if<Array>(&tabs->v);
        if (!list || list->size() > LayoutSnapshot::kMaxTabs) return std::nullopt;
        SessionSnapshot s;
        s.name = *name;
        s.saved = *saved;
        s.active = std::size_t(*active);
        for (const auto &t : *list) {
            const auto *to = std::get_if<Object>(&t.v);
            if (!to || !onlyKeys(*to, {"title", "focus", "layout"})) return std::nullopt;
            const auto *title = asString(find(*to, "title"));
            const auto focus = asInt(find(*to, "focus"));
            const Value *layout = find(*to, "layout");
            if (!title || !focus || *focus < 0 || !layout) return std::nullopt;
            SavedTab tab;
            tab.title = *title;
            tab.focus = std::size_t(*focus);
            tab.root = readNode(*layout, 0);
            if (!tab.root) return std::nullopt;
            s.tabs.push_back(std::move(tab));
        }
        if (!s.valid()) return std::nullopt;
        return s;
    }

    static bool nodeValid(const SavedNode &n, int depth, std::size_t &text) {
        if (depth > 8) return false;
        if (n.leaf) {
            if ((!n.pane.cwd.empty() && !plainPath(n.pane.cwd)) ||
                (!n.pane.command.empty() && !plainCommand(n.pane.command)) ||
                cleanText(n.pane.text) != n.pane.text ||
                (!n.pane.terminal.empty() && !LayoutSnapshot::validId(n.pane.terminal))) {
                return false;
            }
            text += n.pane.text.size();
            return true;
        }
        return n.permille >= 1 && n.permille <= 999 && n.first && n.second && nodeValid(*n.first, depth + 1, text) &&
               nodeValid(*n.second, depth + 1, text);
    }

    static void str(std::string &o, std::string_view s) {
        o += '"';
        for (const char ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            if (ch == '"' || ch == '\\') { o += '\\'; o += ch; }
            else if (c < 0x20 || c == 0x7f) {
                static const char *hex = "0123456789abcdef";
                o += "\\u00";
                o += hex[c >> 4];
                o += hex[c & 15];
            }
            else o += ch;
        }
        o += '"';
    }
    static void write(const SavedNode &n, std::string &o) {
        if (n.leaf) {
            o += "{\"pane\":{";
            bool first = true;
            if (!n.pane.cwd.empty()) {
                o += "\"cwd\":";
                str(o, n.pane.cwd);
                first = false;
            }
            if (!n.pane.command.empty()) {
                o += first ? "" : ",";
                o += "\"command\":[";
                for (std::size_t i = 0; i < n.pane.command.size(); ++i) {
                    if (i) o += ',';
                    str(o, n.pane.command[i]);
                }
                o += ']';
                first = false;
            }
            if (!n.pane.text.empty()) {
                o += first ? "" : ",";
                o += "\"text\":";
                str(o, n.pane.text);
                first = false;
            }
            if (!n.pane.terminal.empty()) {
                o += first ? "" : ",";
                o += "\"terminal\":";
                str(o, n.pane.terminal);
            }
            o += "}}";
            return;
        }
        o += n.direction == SplitDirection::SideBySide ? "{\"split\":\"h\"" : "{\"split\":\"v\"";
        o += ",\"ratio\":" + std::to_string(n.permille) + ",\"a\":";
        write(*n.first, o);
        o += ",\"b\":";
        write(*n.second, o);
        o += '}';
    }
};

} // namespace frontend
