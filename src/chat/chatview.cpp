#include "chatview.h"

#include "chattext.h"

#include <algorithm>
#include <cmath>

namespace chat {
namespace {

struct Rgb {
    int r, g, b;
};

Rgb hsv(double h, double s, double v) {
    const double c = v * s;
    const double x = c * (1 - std::fabs(std::fmod(h / 60.0, 2.0) - 1));
    const double m = v - c;
    double r = 0, g = 0, b = 0;
    if (h < 60) {
        r = c, g = x;
    }
    else if (h < 120) {
        r = x, g = c;
    }
    else if (h < 180) {
        g = c, b = x;
    }
    else if (h < 240) {
        g = x, b = c;
    }
    else if (h < 300) {
        r = x, b = c;
    }
    else {
        r = c, b = x;
    }
    return {int((r + m) * 255), int((g + m) * 255), int((b + m) * 255)};
}

int xterm256(Rgb c) {
    auto level = [](int v) { return std::clamp((v * 5 + 127) / 255, 0, 5); };
    const int cr = level(c.r), cg = level(c.g), cb = level(c.b);
    auto channel = [](int l) { return l == 0 ? 0 : 55 + l * 40; };
    const int cube = 16 + 36 * cr + 6 * cg + cb;
    const int cd = (channel(cr) - c.r) * (channel(cr) - c.r) +
                   (channel(cg) - c.g) * (channel(cg) - c.g) +
                   (channel(cb) - c.b) * (channel(cb) - c.b);
    const int avg = (c.r + c.g + c.b) / 3;
    const int gray = std::clamp((avg - 8 + 5) / 10, 0, 23);
    const int gv = 8 + gray * 10;
    const int gd = (gv - c.r) * (gv - c.r) + (gv - c.g) * (gv - c.g) +
                   (gv - c.b) * (gv - c.b);
    return gd < cd ? 232 + gray : cube;
}

std::string color(const TermCaps &caps, bool background, Rgb c) {
    if (caps.truecolor) {
        return std::string("\x1b[") + (background ? "48" : "38") + ";2;" +
               std::to_string(c.r) + ";" + std::to_string(c.g) + ";" +
               std::to_string(c.b) + "m";
    }
    if (caps.colors256) {
        return std::string("\x1b[") + (background ? "48" : "38") + ";5;" +
               std::to_string(xterm256(c)) + "m";
    }
    return {}; // 16 colors: the bubble relies on reverse video instead.
}

std::size_t hashName(const std::string &name) {
    std::size_t h = 1469598103934665603ull;
    for (const char ch : name) {
        h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
    }
    return h;
}

std::u32string decode(const std::string &utf8) {
    std::u32string out;
    if (!decodeUtf8Strict(utf8, out)) {
        out.clear();
    }
    return out;
}

constexpr const char *kReset = "\x1b[0m";

} // namespace

// --- layout ---------------------------------------------------------------

std::vector<Segment> wrapSegments(const std::u32string &text,
                                  std::size_t width) {
    std::vector<Segment> lines;
    width = std::max<std::size_t>(width, 1);
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (true) {
        if (n - i <= width) {
            lines.push_back({i, n});
            break;
        }
        std::size_t cut = std::u32string::npos;
        for (std::size_t j = i + width; j > i; --j) {
            if (text[j] == U' ') {
                cut = j;
                break;
            }
        }
        if (cut != std::u32string::npos) {
            lines.push_back({i, cut});
            i = cut + 1;
        }
        else {
            lines.push_back({i, i + width});
            i += width;
        }
        if (i >= n) {
            lines.push_back({n, n}); // The text ended on a break.
            break;
        }
    }
    return lines;
}

CaretPos caretIn(const std::vector<Segment> &lines, std::size_t cursor) {
    for (std::size_t k = 0; k < lines.size(); ++k) {
        if (cursor <= lines[k].end) {
            return {k, cursor >= lines[k].begin ? cursor - lines[k].begin : 0};
        }
    }
    const Segment &last = lines.back();
    return {lines.size() - 1, last.end - last.begin + (cursor - last.end)};
}

// --- the editor -------------------------------------------------------------

bool ChatInput::insert(char32_t c) {
    std::u32string candidate = text_;
    candidate.insert(candidate.begin() + std::ptrdiff_t(cursor_), c);
    if (!checkDraft(encodeUtf8(candidate)).ok()) {
        return false;
    }
    text_ = std::move(candidate);
    ++cursor_;
    return true;
}

bool ChatInput::insertText(const std::string &utf8) {
    std::u32string cps;
    if (!decodeUtf8Strict(utf8, cps)) {
        return false;
    }
    bool any = false;
    for (char32_t c : cps) {
        if (c == U'\n' || c == U'\r' || c == U'\t') {
            c = U' ';
        }
        any = insert(c) || any;
    }
    return any;
}

bool ChatInput::backspace() {
    if (cursor_ == 0) {
        return false;
    }
    text_.erase(cursor_ - 1, 1);
    --cursor_;
    return true;
}

bool ChatInput::del() {
    if (cursor_ >= text_.size()) {
        return false;
    }
    text_.erase(cursor_, 1);
    return true;
}

bool ChatInput::left() {
    if (cursor_ == 0) {
        return false;
    }
    --cursor_;
    return true;
}

bool ChatInput::right() {
    if (cursor_ >= text_.size()) {
        return false;
    }
    ++cursor_;
    return true;
}

bool ChatInput::home() {
    const bool moved = cursor_ != 0;
    cursor_ = 0;
    return moved;
}

bool ChatInput::end() {
    const bool moved = cursor_ != text_.size();
    cursor_ = text_.size();
    return moved;
}

bool ChatInput::killLine() {
    const bool changed = !text_.empty();
    text_.clear();
    cursor_ = 0;
    return changed;
}

bool ChatInput::killWord() {
    std::size_t start = cursor_;
    while (start > 0 && text_[start - 1] == U' ') {
        --start;
    }
    while (start > 0 && text_[start - 1] != U' ') {
        --start;
    }
    if (start == cursor_) {
        return false;
    }
    text_.erase(start, cursor_ - start);
    cursor_ = start;
    return true;
}

void ChatInput::set(const std::string &utf8) {
    text_ = decode(utf8);
    cursor_ = text_.size();
}

std::string ChatInput::text() const {
    return encodeUtf8(text_);
}

// --- the view ---------------------------------------------------------------

ChatView::ChatView(ViewConfig config)
    : config_(config) {}

std::string ChatView::clock(Millis at) const {
    const std::int64_t minutes =
        (config_.unixMsAtZero + at) / 60000 + config_.utcOffsetMinutes;
    const std::int64_t day = ((minutes % 1440) + 1440) % 1440;
    char buffer[8];
    buffer[0] = char('0' + day / 600);
    buffer[1] = char('0' + (day / 60) % 10);
    buffer[2] = ':';
    buffer[3] = char('0' + (day % 60) / 10);
    buffer[4] = char('0' + day % 10);
    buffer[5] = 0;
    return buffer;
}

std::string ChatView::line(const std::string &text, bool dim, bool bold) const {
    std::string out;
    if (bold) {
        out += "\x1b[1m";
    }
    if (dim) {
        out += "\x1b[2m";
    }
    out += text;
    out += kReset;
    return out;
}

ChatView::Rendered ChatView::bubble(const BubbleSpec &spec) const {
    const TermCaps &caps = config_.caps;
    const bool colors = caps.truecolor || caps.colors256;
    const int columns = std::max(config_.columns, 8);
    const std::size_t maxWidth =
        std::size_t(std::clamp(columns * 7 / 10, 12, columns - 1));
    const std::size_t innerMax = maxWidth - 2;
    const std::u32string text = decode(spec.text);
    const auto segments = wrapSegments(text, innerMax);
    std::size_t longest = 0;
    for (const auto &s : segments) {
        longest = std::max(longest, s.end - s.begin);
    }
    std::string face;
    if (spec.own) {
        face = caps.unicode ? "\xE2\x98\xBA " : ":) "; // A small face.
    }
    std::string name = spec.name + "";
    std::string rest;
    if (spec.at) {
        rest += "  " + clock(*spec.at);
    }
    if (!spec.state.empty()) {
        rest += "  " + spec.state;
    }
    std::u32string title = decode(face + name + rest);
    const std::size_t nameLength = decode(face + name).size();
    if (title.size() > innerMax) {
        title.resize(innerMax);
    }
    const std::size_t inner = std::max<std::size_t>({title.size(), longest, 1});
    const std::size_t width = inner + 2;
    const double hue = double(hashName(spec.name) % 360);
    const Rgb titleBg = hsv(hue, 0.5, spec.live ? 0.42 : 0.5);
    const Rgb bodyBg = spec.live ? Rgb{52, 44, 34} : Rgb{34, 38, 50};
    Rendered out;
    // Title row.
    {
        std::string row;
        if (colors) {
            row += color(caps, true, titleBg) +
                   color(caps, false, {245, 245, 250});
        }
        else {
            row += "\x1b[7m";
        }
        std::u32string shown = U" " + title;
        const std::size_t boldPart = std::min(nameLength + 1, shown.size());
        row += "\x1b[1m" + encodeUtf8(shown.substr(0, boldPart)) + "\x1b[22m";
        if (colors) {
            row += color(caps, false, {205, 205, 218});
        }
        row += encodeUtf8(shown.substr(boldPart));
        row += std::string(width - std::min(width, shown.size()), ' ');
        row += kReset;
        out.lines.push_back(row);
    }
    // Body rows.
    for (const auto &s : segments) {
        std::string row;
        if (colors) {
            row +=
                color(caps, true, bodyBg) + color(caps, false, {232, 232, 238});
        }
        const std::u32string part = text.substr(s.begin, s.end - s.begin);
        row +=
            " " + encodeUtf8(part) + std::string(inner - part.size() + 1, ' ');
        row += kReset;
        out.lines.push_back(row);
    }
    const CaretPos caret =
        caretIn(segments, std::min(spec.cursor, text.size()));
    out.caretRow = 1 + caret.row;
    out.caretColumn = 1 + std::min(caret.column, inner);
    return out;
}

std::string ChatView::banner(Millis) const {
    return line("Live chat, draft sent as you type", false, true);
}

std::vector<std::string> ChatView::printable(ChatClient &client, Millis now) {
    std::vector<std::string> lines;
    const auto state = client.state();
    if (state != lastState_) {
        if (state == ChatClient::State::Connecting) {
            lines.push_back(line(" \xC2\xB7 connecting...", true));
        }
        else if (state == ChatClient::State::Joined) {
            lines.push_back(
                line(" \xC2\xB7 connected as " + client.name(), true));
        }
        else if (state == ChatClient::State::Closed) {
            lines.push_back(
                line(" \xC2\xB7 disconnected: " + client.closeReason(), true));
        }
        lastState_ = state;
    }
    for (const auto &e : client.takeEvents()) {
        switch (e.kind) {
        case ChatClient::Event::Kind::Message: {
            BubbleSpec spec;
            spec.name = e.bubble.name;
            spec.text = e.bubble.text;
            spec.state = "sent";
            spec.own = e.bubble.own;
            spec.at = e.bubble.at;
            for (auto &l : bubble(spec).lines) {
                lines.push_back(std::move(l));
            }
            lines.emplace_back();
            break;
        }
        case ChatClient::Event::Kind::Join:
            lines.push_back(line(" \xC2\xB7 " + e.name + " joined", true));
            break;
        case ChatClient::Event::Kind::Leave:
            lines.push_back(line(" \xC2\xB7 " + e.name + " left", true));
            break;
        case ChatClient::Event::Kind::Notice:
            lines.push_back(line(" ! " + e.text, false, true));
            break;
        case ChatClient::Event::Kind::Error:
            lines.push_back(line(" ! " + e.text, false, true));
            break;
        case ChatClient::Event::Kind::Hide:
            lines.push_back(
                line(" \xC2\xB7 a message was removed by a moderator", true));
            break;
        case ChatClient::Event::Kind::NameTaken:
            lines.push_back(line(
                " \xC2\xB7 that name is taken: type another and press Enter",
                false,
                true));
            break;
        case ChatClient::Event::Kind::State:
            break;
        }
    }
    const bool pulsing =
        config_.animations && start_ >= 0 && now - start_ < config_.bannerPulse;
    if (!bannerPrinted_ && start_ >= 0 && !pulsing) {
        bannerPrinted_ = true;
        lines.insert(lines.begin(), banner(now));
    }
    return lines;
}

std::optional<Millis> ChatView::nextFrame(Millis now) const {
    if (!config_.animations || start_ < 0 || bannerPrinted_) {
        return std::nullopt;
    }
    return start_ + ((now - start_) / 500 + 1) * 500;
}

std::string ChatView::render(ChatClient &client,
                             const ChatInput &input,
                             Millis now) {
    if (start_ < 0) {
        start_ = now;
    }
    std::string out = "\x1b[?25l\r";
    if (regionRows_ > 0) {
        if (cursorRow_ > 0) {
            out += "\x1b[" + std::to_string(cursorRow_) + "A";
        }
        out += "\x1b[J";
    }
    for (const auto &l : printable(client, now)) {
        out += l + "\r\n";
    }
    // The live region: a status line, the other typers, the own bubble.
    std::vector<std::string> lines;
    std::string status = " Mattrix chat";
    switch (client.state()) {
    case ChatClient::State::Joined:
        status += " \xC2\xB7 " + std::to_string(client.online()) + " here";
        if (client.hiddenTypers() > 0) {
            status += " \xC2\xB7 +" + std::to_string(client.hiddenTypers()) +
                      " typing";
        }
        status += " \xC2\xB7 Ctrl-C leaves";
        break;
    case ChatClient::State::NeedName:
        status += " \xC2\xB7 name taken: type another and press Enter";
        break;
    case ChatClient::State::Closed:
        status += " \xC2\xB7 offline";
        break;
    default:
        status += " \xC2\xB7 connecting";
        break;
    }
    const bool pulsing = config_.animations && !bannerPrinted_ &&
                         now - start_ < config_.bannerPulse;
    if (pulsing) {
        const bool bright = ((now - start_) / 500) % 2 == 0;
        lines.push_back(
            line("  Live chat, draft sent as you type", !bright, bright));
    }
    lines.push_back(line(status, true));
    std::vector<std::vector<std::string>> typerBubbles;
    const auto &typers = client.typers();
    const std::size_t first = typers.size() > config_.maxTypersShown
                                  ? typers.size() - config_.maxTypersShown
                                  : 0;
    for (std::size_t i = first; i < typers.size(); ++i) {
        BubbleSpec spec;
        spec.name = typers[i].name;
        spec.text = typers[i].text;
        spec.state = config_.caps.unicode ? "typing\xE2\x80\xA6" : "typing...";
        spec.live = true;
        typerBubbles.push_back(bubble(spec).lines);
    }
    BubbleSpec own;
    own.name = client.name();
    own.text = input.text();
    own.own = true;
    own.live = true;
    own.cursor = input.cursor();
    switch (client.ownState()) {
    case ChatClient::OwnState::Idle:
        own.state = "ready";
        break;
    case ChatClient::OwnState::DraftSent:
        own.state = "draft sent";
        break;
    case ChatClient::OwnState::NotSent:
        own.state = "not sent";
        break;
    }
    const Rendered ownBubble = bubble(own);
    const std::size_t room = std::size_t(std::max(config_.rows - 1, 3));
    std::size_t needed = lines.size() + ownBubble.lines.size();
    for (const auto &b : typerBubbles) {
        needed += b.size() + 1;
    }
    std::size_t skip = 0;
    while (needed > room && skip < typerBubbles.size()) {
        needed -= typerBubbles[skip].size() + 1;
        ++skip;
    }
    for (std::size_t i = skip; i < typerBubbles.size(); ++i) {
        for (const auto &l : typerBubbles[i]) {
            lines.push_back(l);
        }
        lines.emplace_back();
    }
    const std::size_t ownStart = lines.size();
    for (const auto &l : ownBubble.lines) {
        lines.push_back(l);
    }
    std::size_t caretRow = ownStart + ownBubble.caretRow;
    if (lines.size() > room) { // Too tall even so: keep the bottom.
        const std::size_t drop = lines.size() - room;
        lines.erase(lines.begin(), lines.begin() + std::ptrdiff_t(drop));
        caretRow = caretRow >= drop ? caretRow - drop : 0;
    }
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size()) {
            out += "\r\n";
        }
    }
    // The caret goes where the text goes.
    const std::size_t lastRow = lines.size() - 1;
    if (lastRow > caretRow) {
        out += "\x1b[" + std::to_string(lastRow - caretRow) + "A";
    }
    out += "\x1b[" + std::to_string(ownBubble.caretColumn + 1) + "G\x1b[?25h";
    regionRows_ = lines.size();
    cursorRow_ = caretRow;
    return out;
}

std::string ChatView::resize(int columns, int rows) {
    config_.columns = columns;
    config_.rows = rows;
    std::string out;
    if (regionRows_ > 0) {
        const std::size_t below =
            regionRows_ - 1 - std::min(cursorRow_, regionRows_ - 1);
        if (below > 0) {
            out += "\x1b[" + std::to_string(below) + "B";
        }
        out += "\r\n";
        regionRows_ = 0;
        cursorRow_ = 0;
    }
    return out;
}

std::string ChatView::finish() {
    std::string out;
    if (regionRows_ > 0) {
        const std::size_t below =
            regionRows_ - 1 - std::min(cursorRow_, regionRows_ - 1);
        if (below > 0) {
            out += "\x1b[" + std::to_string(below) + "B";
        }
        regionRows_ = 0;
        cursorRow_ = 0;
    }
    out += std::string(kReset) + "\x1b[?25h\r\n";
    return out;
}

} // namespace chat
