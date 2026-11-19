#include "vtparser.h"

#include "palette.h"
#include "terminalmodel.h"
#include "utf8decoder.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace {
constexpr unsigned char escape = 0x1b;
constexpr std::size_t maxParameters = 32;
constexpr int maxParameter = 1000000;
constexpr std::size_t maxString = 2048;

} // namespace

struct VtParser::Impl {
    enum class State {
        Ground,
        Escape,
        EscapeIntermediate,
        Csi,
        CsiIgnore,
        String,
        StringEscape
    };
    struct Rendition {
        ColorReference foreground;
        ColorReference background;
        bool inverse = false;
        bool conceal = false;
        CellAttribute attributes = CellAttribute::None;
    };
    struct Context {
        Rendition style;
        Rendition saved;
        bool hasSaved = false;
        bool g0Graphics = false;
        bool g1Graphics = false;
        bool useG1 = false;
        bool savedG0 = false;
        bool savedG1 = false;
        bool savedUseG1 = false;
    };

    TerminalModel &model;
    VtEvents events;
    const Rendition defaults;
    Context primary;
    Context alternate;
    Utf8Decoder utf8;
    TerminalModes modes;
    State state = State::Ground;
    std::array<int, maxParameters> parameters{};
    std::size_t parameterCount = 0;
    char prefix = 0;
    bool intermediate = false;
    char intermediateChar = 0;
    char charsetSelector = 0;
    bool osc = false;
    bool discardString = false;
    std::string controlString;
    char32_t lastPainted = 0;

    Impl(TerminalModel &model,
         VtEvents events,
         ColorReference foreground,
         ColorReference background)
        : model(model)
        , events(std::move(events))
        , defaults{foreground, background}
        , primary{defaults, defaults, false}
        , alternate{defaults, defaults, false} {
        applyColors();
    }

    Context &context() {
        return model.alternateScreen() ? alternate : primary;
    }
    void applyColors() {
        const auto &style = context().style;
        model.setColorReferences(style.foreground, style.background);
        auto attributes = style.attributes;
        if (style.inverse) {
            attributes = attributes | CellAttribute::Inverse;
        }
        if (style.conceal) {
            attributes = attributes | CellAttribute::Conceal;
        }
        model.setAttributes(attributes);
    }
    void save() {
        model.saveCursor();
        context().saved = context().style;
        context().hasSaved = true;
        context().savedG0 = context().g0Graphics;
        context().savedG1 = context().g1Graphics;
        context().savedUseG1 = context().useG1;
    }
    void restore() {
        if (!context().hasSaved) {
            return;
        }
        model.restoreCursor();
        context().style = context().saved;
        context().g0Graphics = context().savedG0;
        context().g1Graphics = context().savedG1;
        context().useG1 = context().savedUseG1;
        applyColors();
    }
    void alternateMode(bool enabled) {
        if (model.alternateScreen() == enabled) {
            return;
        }
        const auto inherited = context();
        model.setAlternateScreen(enabled);
        if (enabled) {
            alternate = Context{inherited.style, inherited.style, false};
            alternate.g0Graphics = inherited.g0Graphics;
            alternate.g1Graphics = inherited.g1Graphics;
            alternate.useG1 = inherited.useG1;
        }
        applyColors();
    }
    void resetInput() noexcept {
        state = State::Ground;
        utf8.reset();
        controlString.clear();
        parameterCount = 0;
        prefix = 0;
        intermediate = false;
        osc = false;
        discardString = false;
    }
    void paint(std::u32string text) {
        const auto &ctx = context();
        if (ctx.useG1 ? ctx.g1Graphics : ctx.g0Graphics) {
            static constexpr std::u32string_view graphics =
                U"◆▒␉␌␍␊°±␤␋┘┐┌└┼⎺⎻─⎼⎽├┤┴┬│≤≥π≠£·";
            static_assert(graphics.size() == 31);
            for (auto &ch : text) {
                if (ch >= 0x60 && ch <= 0x7e) {
                    ch = graphics[std::size_t(ch - 0x60)];
                }
            }
        }
        if (!text.empty()) {
            lastPainted = text.back();
        }
        model.write(text);
    }
    void resetTerminal() {
        lastPainted = 0;
        model.reset();
        primary = Context{defaults, defaults, false};
        alternate = Context{defaults, defaults, false};
        modes = {};
        applyColors();
        model.eraseDisplay(TerminalModel::EraseMode::All);
    }
    void control(unsigned char byte) {
        if (byte == 0x0e) {
            context().useG1 = true;
            return;
        }
        if (byte == 0x0f) {
            context().useG1 = false;
            return;
        }
        // Only these C0 actions are currently supported. BEL has no side
        // effect.
        if (byte == '\r' || byte == '\n' || byte == '\b' || byte == '\t') {
            const char32_t ch = byte;
            model.write(std::u32string_view(&ch, 1));
        }
    }
    void beginCsi() {
        state = State::Csi;
        parameterCount = 1;
        parameters[0] = -1;
        prefix = 0;
        intermediate = false;
        intermediateChar = 0;
    }
    void beginString(bool isOsc) {
        state = State::String;
        osc = isOsc;
        discardString = !isOsc;
        controlString.clear();
    }
    void reportColor(std::string_view request) {
        if (!events.palette || !events.reply) {
            return;
        }
        Rgb8 color;
        if (request == "10;?") {
            color = events.palette->resolve(defaults.foreground, true);
        }
        else if (request == "11;?") {
            color = events.palette->resolve(defaults.background, false);
        }
        else if (request.substr(0, 2) == "4;" && request.size() >= 5 &&
                 request.substr(request.size() - 2) == ";?") {
            const auto indexText = request.substr(2, request.size() - 4);
            unsigned index = 0;
            const auto end = indexText.data() + indexText.size();
            const auto parsed = std::from_chars(indexText.data(), end, index);
            if (parsed.ec != std::errc{} || parsed.ptr != end ||
                index >= events.palette->colors.size()) {
                return;
            }
            color = events.palette->colors[index];
        }
        else {
            return;
        }
        std::string reply = "\x1b]";
        reply.append(request.substr(0, request.size() - 1));
        reply += "rgb:";
        const auto append = [&](std::uint8_t channel) {
            constexpr char hex[] = "0123456789abcdef";
            // Duplicate the byte to scale 0..255 to 0..65535 exactly.
            for (int i = 0; i < 2; ++i) {
                reply += hex[channel >> 4];
                reply += hex[channel & 15];
            }
        };
        append(color.r);
        reply += '/';
        append(color.g);
        reply += '/';
        append(color.b);
        reply += "\x1b\\";
        events.reply(reply);
    }
    static std::optional<std::size_t> parseIndex(std::string_view text) {
        unsigned index = 0;
        const auto end = text.data() + text.size();
        const auto parsed = std::from_chars(text.data(), end, index);
        if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != end ||
            index > 255) {
            return std::nullopt;
        }
        return std::size_t(index);
    }
    // Accepts rgb:R/G/B with 1-4 hex digits per channel, and #RGB/#RRGGBB/...
    static std::optional<Rgb8> parseColor(std::string_view text) {
        const auto scale = [](std::string_view hex) -> std::optional<int> {
            unsigned value = 0;
            const auto end = hex.data() + hex.size();
            const auto parsed = std::from_chars(hex.data(), end, value, 16);
            if (hex.empty() || hex.size() > 4 || parsed.ec != std::errc{} ||
                parsed.ptr != end) {
                return std::nullopt;
            }
            const unsigned maximum = (1u << (4 * hex.size())) - 1;
            return int((value * 255 + maximum / 2) / maximum);
        };
        std::array<std::string_view, 3> parts;
        if (text.substr(0, 4) == "rgb:") {
            text.remove_prefix(4);
            for (std::size_t i = 0; i < 3; ++i) {
                const auto slash = text.find('/');
                if ((i < 2) != (slash != std::string_view::npos)) {
                    return std::nullopt;
                }
                parts[i] = text.substr(0, slash);
                text.remove_prefix(slash == std::string_view::npos
                                       ? text.size()
                                       : slash + 1);
            }
        }
        else if (text.size() > 1 && text[0] == '#' && (text.size() - 1) % 3 == 0 &&
                 text.size() <= 13) {
            const auto width = (text.size() - 1) / 3;
            for (std::size_t i = 0; i < 3; ++i) {
                parts[i] = text.substr(1 + i * width, width);
            }
        }
        else {
            return std::nullopt;
        }
        const auto r = scale(parts[0]), g = scale(parts[1]), b = scale(parts[2]);
        if (!r || !g || !b) {
            return std::nullopt;
        }
        return Rgb8{std::uint8_t(*r), std::uint8_t(*g), std::uint8_t(*b)};
    }
    // OSC 4;index;spec[;index;spec...]: set colors, or query with "?".
    void paletteRequest(std::string_view data) {
        data.remove_prefix(2);
        while (!data.empty()) {
            const auto first = data.find(';');
            if (first == std::string_view::npos) {
                return;
            }
            const auto second = data.find(';', first + 1);
            const auto index = parseIndex(data.substr(0, first));
            const auto spec = data.substr(first + 1, second == std::string_view::npos
                                                         ? std::string_view::npos
                                                         : second - first - 1);
            if (index) {
                if (spec == "?") {
                    reportColor("4;" + std::to_string(*index) + ";?");
                }
                else if (const auto color = parseColor(spec);
                         color && events.setPaletteColor) {
                    events.setPaletteColor(*index, *color);
                }
            }
            if (second == std::string_view::npos) {
                return;
            }
            data.remove_prefix(second + 1);
        }
    }
    void paletteReset(std::string_view data) {
        data.remove_prefix(3);
        if (!events.resetPaletteColor) {
            return;
        }
        if (data.empty() || data == ";") {
            events.resetPaletteColor(std::nullopt);
            return;
        }
        data.remove_prefix(1); // The ';' after 104.
        while (!data.empty()) {
            const auto semicolon = data.find(';');
            if (const auto index = parseIndex(data.substr(0, semicolon))) {
                events.resetPaletteColor(*index);
            }
            if (semicolon == std::string_view::npos) {
                return;
            }
            data.remove_prefix(semicolon + 1);
        }
    }
    void endString() {
        if (osc && !discardString) {
            const std::string_view data = controlString;
            if (data.substr(0, 2) == "0;" || data.substr(0, 2) == "2;") {
                if (events.title) {
                    events.title(data.substr(2));
                }
            }
            else if (data.substr(0, 2) == "4;") {
                paletteRequest(data);
            }
            else if (data == "104" || data.substr(0, 4) == "104;") {
                paletteReset(data);
            }
            else {
                reportColor(data);
            }
        }
        controlString.clear();
        state = State::Ground;
    }
    void escapeByte(unsigned char byte) {
        if (byte == '[') {
            beginCsi();
        }
        else if (byte == ']') {
            beginString(true);
        }
        else if (byte == 'P' || byte == 'X' || byte == '^' || byte == '_') {
            beginString(false);
        }
        else if (byte >= 0x20 && byte <= 0x2f) {
            state = State::EscapeIntermediate;
            charsetSelector = byte == '(' || byte == ')' ? char(byte) : 0;
        }
        else {
            state = State::Ground;
            if (byte == '7') {
                save();
            }
            else if (byte == '8') {
                restore();
            }
            else if (byte == 'c') {
                resetTerminal();
            }
            else if (byte == '=' || byte == '>') {
                modes.applicationKeypad = byte == '=';
            }
            else if (byte == 'M') {
                model.reverseIndex();
            }
            else if (byte == 'H') {
                model.setTabStop();
            }
            else if (byte == 'Z') {
                if (events.reply) {
                    events.reply("\x1b[?1;0c");
                }
            }
            else if (byte == 'D') {
                model.lineFeed();
            }
            else if (byte == 'E') {
                model.carriageReturn();
                model.lineFeed();
            }
        }
    }
    int value(std::size_t index,
              int fallback,
              bool zeroIsDefault = true) const {
        if (index >= parameterCount || parameters[index] < 0 ||
            (zeroIsDefault && parameters[index] == 0)) {
            return fallback;
        }
        return parameters[index];
    }
    void relative(int dx, int dy) {
        const auto cursor = model.cursor();
        const auto x = std::clamp(std::int64_t(cursor.column) + dx,
                                  std::int64_t(0),
                                  std::int64_t(model.columns() - 1));
        const auto top = model.originMode() ? model.scrollTop() : 0;
        const auto bottom =
            model.originMode() ? model.scrollBottom() : int(model.rows()) - 1;
        const auto y = std::clamp(std::int64_t(cursor.row) + dy,
                                  std::int64_t(top),
                                  std::int64_t(bottom));
        model.moveCursor(int(x), int(y));
    }
    void sgr() {
        auto &style = context().style;
        for (std::size_t i = 0; i < parameterCount; ++i) {
            const int code = value(i, 0, false);
            if (code == 0) {
                style = defaults;
            }
            else if (code == 1) {
                style.attributes = style.attributes | CellAttribute::Bold;
            }
            else if (code == 2) {
                style.attributes = style.attributes | CellAttribute::Faint;
            }
            else if (code == 3) {
                style.attributes = style.attributes | CellAttribute::Italic;
            }
            else if (code == 4) {
                style.attributes = style.attributes | CellAttribute::Underline;
            }
            else if (code == 5 || code == 6) {
                style.attributes = style.attributes | CellAttribute::Blink;
            }
            else if (code == 9) {
                style.attributes = style.attributes | CellAttribute::Strike;
            }
            else if (code == 22) {
                style.attributes = style.attributes & ~(CellAttribute::Bold |
                                                        CellAttribute::Faint);
            }
            else if (code == 23) {
                style.attributes = style.attributes & ~CellAttribute::Italic;
            }
            else if (code == 24) {
                style.attributes = style.attributes & ~CellAttribute::Underline;
            }
            else if (code == 25) {
                style.attributes = style.attributes & ~CellAttribute::Blink;
            }
            else if (code == 29) {
                style.attributes = style.attributes & ~CellAttribute::Strike;
            }
            else if (code == 7) {
                style.inverse = true;
            }
            else if (code == 27) {
                style.inverse = false;
            }
            else if (code == 8) {
                style.conceal = true;
            }
            else if (code == 28) {
                style.conceal = false;
            }
            else if (code == 39) {
                style.foreground = defaults.foreground;
            }
            else if (code == 49) {
                style.background = defaults.background;
            }
            else if (code >= 30 && code <= 37) {
                style.foreground =
                    ColorReference::indexed(std::uint8_t(code - 30));
            }
            else if (code >= 40 && code <= 47) {
                style.background =
                    ColorReference::indexed(std::uint8_t(code - 40));
            }
            else if (code >= 90 && code <= 97) {
                style.foreground =
                    ColorReference::indexed(std::uint8_t(code - 90 + 8));
            }
            else if (code >= 100 && code <= 107) {
                style.background =
                    ColorReference::indexed(std::uint8_t(code - 100 + 8));
            }
            else if (code == 38 || code == 48) {
                if (++i >= parameterCount) {
                    break;
                }
                const int mode = parameters[i];
                ColorReference color{};
                bool valid = false;
                if (mode == 5) {
                    if (++i >= parameterCount) {
                        break;
                    }
                    const int index = parameters[i];
                    valid = index >= 0 && index <= 255;
                    if (valid) {
                        color = ColorReference::indexed(std::uint8_t(index));
                    }
                }
                else if (mode == 2) {
                    if (parameterCount - i <= 3) {
                        break;
                    }
                    const int r = parameters[++i];
                    const int g = parameters[++i];
                    const int b = parameters[++i];
                    valid = r >= 0 && r <= 255 && g >= 0 && g <= 255 &&
                            b >= 0 && b <= 255;
                    if (valid) {
                        color = ColorReference::rgb({std::uint8_t(r),
                                                     std::uint8_t(g),
                                                     std::uint8_t(b)});
                    }
                }
                else {
                    break;
                } // Unknown extended-color format: do not reinterpret its
                  // payload.
                if (valid) {
                    if (code == 38) {
                        style.foreground = color;
                    }
                    else {
                        style.background = color;
                    }
                }
            }
            // Unsupported attributes (e.g. double underline) are ignored.
        }
        applyColors();
    }
    static TerminalModes::MouseTracking mouseTrackingFor(int mode) {
        using Tracking = TerminalModes::MouseTracking;
        return mode == 9      ? Tracking::Press
               : mode == 1000 ? Tracking::Normal
               : mode == 1002 ? Tracking::Drag
               : mode == 1003 ? Tracking::Any
                              : Tracking::Off;
    }
    static TerminalModes::MouseEncoding mouseEncodingFor(int mode) {
        using Encoding = TerminalModes::MouseEncoding;
        return mode == 1005   ? Encoding::Utf8
               : mode == 1006 ? Encoding::Sgr
               : mode == 1015 ? Encoding::Urxvt
                              : Encoding::X10;
    }
    // DECSTR: soft reset of modes and rendition; screen contents are kept.
    void softReset() {
        primary.style = defaults;
        alternate.style = defaults;
        primary.hasSaved = alternate.hasSaved = false;
        for (auto *context : {&primary, &alternate}) {
            context->g0Graphics = context->g1Graphics = context->useG1 = false;
        }
        modes = {};
        model.setInsertMode(false);
        model.setOriginMode(false);
        model.setAutoWrap(true);
        model.setScrollRegion(0, int(model.rows()) - 1);
        model.setCursorVisible(true);
        model.setCursorAppearance(model.cursor().shape, true);
        applyColors();
    }
    void dispatch(unsigned char final) {
        if (intermediate) {
            if (intermediateChar == '!' && final == 'p' && prefix == 0) {
                softReset();
                return;
            }
            if (intermediateChar == ' ' && final == 'q' && prefix == 0 &&
                parameterCount == 1) {
                const int style = value(0, 0, false);
                if (style <= 6) {
                    const auto shape = style <= 2 ? CursorState::Shape::Block
                                       : style <= 4
                                           ? CursorState::Shape::Underline
                                           : CursorState::Shape::Bar;
                    model.setCursorAppearance(shape,
                                              style == 0 || style % 2 == 1);
                }
            }
            return;
        }
        if (prefix == '?') {
            if (final != 'h' && final != 'l') {
                return;
            }
            for (std::size_t i = 0; i < parameterCount; ++i) {
                if (parameters[i] == 1) {
                    modes.applicationCursor = final == 'h';
                }
                else if (parameters[i] == 2004) {
                    modes.bracketedPaste = final == 'h';
                }
                else if (parameters[i] == 6) {
                    model.setOriginMode(final == 'h');
                }
                else if (parameters[i] == 7) {
                    model.setAutoWrap(final == 'h');
                }
                else if (parameters[i] == 12) {
                    model.setCursorAppearance(model.cursor().shape,
                                              final == 'h');
                }
                else if (parameters[i] == 25) {
                    model.setCursorVisible(final == 'h');
                }
                else if (parameters[i] == 1049 || parameters[i] == 47 ||
                         parameters[i] == 1047) {
                    alternateMode(final == 'h');
                }
                else if (parameters[i] == 1048) {
                    if (final == 'h') {
                        save();
                    }
                    else {
                        restore();
                    }
                }
                else if (parameters[i] == 1004) {
                    modes.focusReporting = final == 'h';
                }
                else if (const auto tracking = mouseTrackingFor(parameters[i]);
                         tracking != TerminalModes::MouseTracking::Off) {
                    if (final == 'h') {
                        modes.mouseTracking = tracking;
                    }
                    else if (modes.mouseTracking == tracking) {
                        modes.mouseTracking = TerminalModes::MouseTracking::Off;
                    }
                }
                else if (const auto encoding = mouseEncodingFor(parameters[i]);
                         encoding != TerminalModes::MouseEncoding::X10) {
                    if (final == 'h') {
                        modes.mouseEncoding = encoding;
                    }
                    else if (modes.mouseEncoding == encoding) {
                        modes.mouseEncoding = TerminalModes::MouseEncoding::X10;
                    }
                }
            }
            return;
        }
        if (prefix != 0) {
            return;
        }
        if (final == 'h' || final == 'l') {
            for (std::size_t i = 0; i < parameterCount; ++i) {
                if (parameters[i] == 4) {
                    model.setInsertMode(final == 'h');
                }
            }
            return;
        }
        const int count = value(0, 1);
        switch (final) {
        case 'A':
            relative(0, -count);
            break;
        case 'B':
        case 'e':
            relative(0, count);
            break;
        case 'C':
        case 'a':
            relative(count, 0);
            break;
        case 'D':
            relative(-count, 0);
            break;
        case 'E':
            relative(0, count);
            model.carriageReturn();
            break;
        case 'F':
            relative(0, -count);
            model.carriageReturn();
            break;
        case 'G':
            model.moveCursor(count - 1, model.cursor().row);
            break;
        case 'd':
            model.addressCursor(model.cursor().column, count - 1);
            break;
        case 'H':
        case 'f':
            model.addressCursor(value(1, 1) - 1, count - 1);
            break;
        case 'J':
        case 'K': {
            const int mode = value(0, 0, false);
            if (mode <= 2) {
                const auto erase = mode == 0 ? TerminalModel::EraseMode::ToEnd
                                   : mode == 1
                                       ? TerminalModel::EraseMode::ToStart
                                       : TerminalModel::EraseMode::All;
                if (final == 'J') {
                    model.eraseDisplay(erase);
                }
                else {
                    model.eraseLine(erase);
                }
            }
            break;
        }
        case 'I':
            model.tabForward(count);
            break;
        case 'Z':
            model.tabBackward(count);
            break;
        case 'g': {
            const int mode = value(0, 0, false);
            if (mode == 0 || mode == 3) {
                model.clearTabStops(mode == 3);
            }
            break;
        }
        case '@':
            model.insertCharacters(count);
            break;
        case 'P':
            model.deleteCharacters(count);
            break;
        case 'X':
            model.eraseCharacters(count);
            break;
        case 'L':
            model.insertLines(count);
            break;
        case 'M':
            model.deleteLines(count);
            break;
        case 'S':
            model.scrollUp(count);
            break;
        case 'T':
            model.scrollDown(count);
            break;
        case 'r':
            model.setScrollRegion(count - 1, value(1, int(model.rows())) - 1);
            break;
        case 'c':
            if (parameterCount == 1 && value(0, 0, false) == 0 &&
                events.reply) {
                events.reply("\x1b[?1;0c");
            }
            break;
        case 'm':
            sgr();
            break;
        case 'b':
            if (lastPainted) {
                model.write(std::u32string(
                    std::size_t(std::min(count, 65535)), lastPainted));
            }
            break;
        case 's':
            save();
            break;
        case 'u':
            restore();
            break;
        case 'n':
            if (parameterCount == 1 && events.reply) {
                if (parameters[0] == 5) {
                    events.reply("\x1b[0n");
                }
                else if (parameters[0] == 6) {
                    const auto cursor = model.cursor();
                    const auto reply =
                        "\x1b[" +
                        std::to_string(
                            cursor.row + 1 -
                            (model.originMode() ? model.scrollTop() : 0)) +
                        ";" + std::to_string(cursor.column + 1) + "R";
                    events.reply(reply);
                }
            }
            break;
        default:
            break;
        }
    }
    void csiByte(unsigned char byte) {
        if (byte >= 0x40 && byte <= 0x7e) {
            if (state == State::Csi) {
                dispatch(byte);
            }
            state = State::Ground;
            return;
        }
        if (state == State::CsiIgnore) {
            return;
        }
        if (byte >= 0x20 && byte <= 0x2f) {
            intermediateChar = intermediate ? 0 : char(byte);
            intermediate = true;
            return;
        }
        if (intermediate) {
            state = State::CsiIgnore;
            return;
        }
        if (byte >= '0' && byte <= '9') {
            int &current = parameters[parameterCount - 1];
            if (current < 0) {
                current = 0;
            }
            const int digit = byte - '0';
            if (current > (maxParameter - digit) / 10) {
                state = State::CsiIgnore;
            }
            else {
                current = current * 10 + digit;
            }
        }
        else if (byte == ';') {
            if (parameterCount == maxParameters) {
                state = State::CsiIgnore;
            }
            else {
                parameters[parameterCount++] = -1;
            }
        }
        else if (byte >= 0x3c && byte <= 0x3f && prefix == 0 &&
                 parameterCount == 1 && parameters[0] == -1) {
            prefix = char(byte);
        }
        else {
            state = State::CsiIgnore;
        }
    }
    void byte(unsigned char byte) {
        if (byte == 0x18 || byte == 0x1a) {
            resetInput();
            return;
        } // CAN/SUB
        if (state == State::String || state == State::StringEscape) {
            if (state == State::StringEscape) {
                if (byte == '\\') {
                    endString();
                    return;
                }
                // Malformed embedded escapes make an OSC non-actionable, but
                // payload stays discarded until BEL/ST or explicit
                // cancellation.
                discardString = true;
                if (byte == escape) {
                    return;
                }
                state = State::String;
            }
            if (byte == escape) {
                state = State::StringEscape;
            }
            else if (byte == 7 && osc) {
                endString();
            }
            else if (byte >= 0x20 && byte != 0x7f && !discardString) {
                if (controlString.size() == maxString) {
                    discardString = true;
                    controlString.clear();
                }
                else {
                    controlString.push_back(char(byte));
                }
            }
            return;
        }
        if (byte == escape) {
            state = State::Escape;
            return;
        }
        if (byte < 0x20) {
            control(byte);
            return;
        }
        if (byte == 0x7f) {
            return;
        }
        if (state == State::Escape) {
            escapeByte(byte);
        }
        else if (state == State::EscapeIntermediate) {
            if (byte >= 0x30) {
                if (byte == '0' || byte == 'B') {
                    if (charsetSelector == '(') {
                        context().g0Graphics = byte == '0';
                    }
                    else if (charsetSelector == ')') {
                        context().g1Graphics = byte == '0';
                    }
                }
                state = State::Ground;
            }
            else {
                charsetSelector = 0;
            }
        }
        else if (state == State::Csi || state == State::CsiIgnore) {
            csiByte(byte);
        }
    }
    void feed(std::string_view bytes) {
        for (std::size_t i = 0; i < bytes.size();) {
            const auto next = static_cast<unsigned char>(bytes[i]);
            if (state == State::Ground && next >= 0x20 && next != 0x7f) {
                const auto begin = i;
                do {
                    ++i;
                } while (i < bytes.size() &&
                         static_cast<unsigned char>(bytes[i]) >= 0x20 &&
                         bytes[i] != 0x7f);
                paint(utf8.feed(bytes.substr(begin, i - begin)));
            }
            else {
                if (state == State::Ground) {
                    model.write(utf8.finish());
                }
                byte(next);
                ++i;
            }
        }
    }
};

VtParser::VtParser(TerminalModel &model,
                   VtEvents events,
                   Rgb8 foreground,
                   Rgb8 background)
    : impl_(std::make_unique<Impl>(model,
                                   std::move(events),
                                   ColorReference::rgb(foreground),
                                   ColorReference::rgb(background))) {}
VtParser::VtParser(TerminalModel &model, VtEvents events)
    : impl_(std::make_unique<Impl>(model,
                                   std::move(events),
                                   ColorReference::defaultColor(),
                                   ColorReference::defaultColor())) {}
VtParser::~VtParser() = default;
void VtParser::feed(std::string_view bytes) {
    impl_->feed(bytes);
}
void VtParser::finish() {
    if (impl_->state == Impl::State::Ground) {
        impl_->model.write(impl_->utf8.finish());
    }
    impl_->resetInput();
}
const TerminalModes &VtParser::modes() const noexcept {
    return impl_->modes;
}
void VtParser::resetInput() noexcept {
    impl_->resetInput();
}
