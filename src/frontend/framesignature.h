#pragma once

#include "palette.h"
#include "renderframe.h"
#include "tabtypes.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>
#include <vector>

namespace frontend {
// 64-bit FNV-1a over everything that influences the pixels of a frame: cells,
// cursor, offsets, scrollbar, overlays, blink phase and palette. Equal
// signatures for consecutive frames mean redrawing would change nothing, so an
// idle terminal can skip rendering (and the CPU/GPU work behind it). Collisions
// only risk one skipped redraw until the next change.
class FrameSignature {
public:
    template <class T>
    void add(const T &value) {
        static_assert(std::is_trivially_copyable_v<T>);
        bytes(&value, sizeof value);
    }
    void bytes(const void *data, std::size_t size) {
        // A word at a time (FNV-1a over 64-bit words, the tail byte by byte): cells are hashed every
        // frame, so this is the cost of asking "did anything change".
        auto p = static_cast<const unsigned char *>(data);
        for (; size >= 8; size -= 8, p += 8) {
            std::uint64_t word;
            std::memcpy(&word, p, 8);
            hash_ = (hash_ ^ word) * 1099511628211ull;
            hash_ ^= hash_ >> 29;
        }
        for (std::size_t i = 0; i < size; ++i) {
            hash_ = (hash_ ^ p[i]) * 1099511628211ull;
        }
    }
    std::uint64_t value() const {
        return hash_;
    }

private:
    std::uint64_t hash_ = 1469598103934665603ull;
};

// The cells only (the bulk of a frame), so a caller that needs two signatures hashes them once.
inline std::uint64_t cellsSignature(const RenderFrame &frame) {
    FrameSignature s;
    for (std::size_t y = 0; y < frame.cells.rows(); ++y) {
        const auto row = frame.cells.row(y);
        s.bytes(row.data(), frame.cells.columns() * sizeof(FChar));
    }
    return s.value();
}

// Adds everything that influences the pixels of one frame. `cells`: cellsSignature(frame) when the
// caller has it already. `blink`: false leaves out the blink phase (text blink, cursor blink).
inline void addFrame(FrameSignature &s,
                     const RenderFrame &frame,
                     const Palette &palette,
                     std::optional<std::uint64_t> cells = std::nullopt,
                     bool blink = true,
                     bool steadyCursorVisible = true) {
    s.add(cells ? *cells : cellsSignature(frame));
    s.add(frame.cells.columns());
    s.add(frame.cells.rows());
    const auto &c = frame.cursor;
    s.add(c.column);
    s.add(c.row);
    s.add(c.shape);
    s.add(blink ? c.visible : steadyCursorVisible);
    s.add(c.focused);
    s.add(c.color);
    s.add(c.columnOffset);
    s.add(c.rowOffset);
    s.add(frame.verticalOffsetPixels);
    s.add(frame.textHeightPixels);
    s.add(frame.scrollbar.visible);
    s.add(frame.scrollbar.position);
    s.add(frame.scrollbar.size);
    s.add(blink ? frame.textBlinkVisible : true);
    s.add(frame.selectionBegin);
    s.add(frame.selectionEnd);
    s.add(frame.underlineBegin);
    s.add(frame.underlineEnd);
    for (const auto &r : frame.selectionRanges) {
        s.add(r.first);
        s.add(r.second);
    }
    s.add(frame.selectionRanges.size());
    for (const auto &r : frame.matchRanges) {
        s.add(r.first);
        s.add(r.second);
    }
    s.add(frame.matchRanges.size());
    // The wrapped-line marks: empty (none asked for) differs from all false
    // (the thumb makes room).
    s.add(frame.wrapMarks.size());
    {
        std::uint64_t word = 0;
        int bits = 0;
        for (const bool mark : frame.wrapMarks) {
            word = (word << 1) | (mark ? 1u : 0u);
            if (++bits == 64) {
                s.add(word);
                word = 0;
                bits = 0;
            }
        }
        s.add(word);
    }
    s.bytes(palette.colors.data(), sizeof palette.colors);
    s.add(palette.foreground);
    s.add(palette.background);
}

inline void addRect(FrameSignature &s, const PixelRect &r) {
    s.add(r.x);
    s.add(r.y);
    s.add(r.w);
    s.add(r.h);
}

inline std::uint64_t frameSignature(const RenderFrame &frame,
                                    const Palette &palette) {
    FrameSignature s;
    addFrame(s, frame, palette);
    return s.value();
}

// One pane (or the tab strip) of a window frame, as drawn.
struct PaneSignatureInput {
    const RenderFrame *frame = nullptr;
    const Palette *palette = nullptr;
    PixelRect rect;      // where it is drawn
    PixelRect scrollbar; // the gutter half of its scrollbar (empty: inside)
};

// The signature of a whole window: every pane with its place, every divider
// and the tab strip (pass it as a pane entry). Equal signatures for consecutive
// frames mean nothing changed, so an idle window costs nothing. Order matters.
inline std::uint64_t windowSignature(
    const std::vector<PaneSignatureInput> &panes,
    const std::vector<DividerBox> &dividers,
    int windowWidth = 0,
    int windowHeight = 0) {
    FrameSignature s;
    s.add(windowWidth);
    s.add(windowHeight);
    s.add(panes.size());
    for (const auto &p : panes) {
        addRect(s, p.rect);
        addRect(s, p.scrollbar);
        if (p.frame && p.palette) {
            s.add(true);
            addFrame(s, *p.frame, *p.palette);
        }
        else {
            s.add(false);
        }
    }
    s.add(dividers.size());
    for (const auto &d : dividers) {
        addRect(s, d.rect);
        s.add(d.direction);
        s.add(d.before);
        s.add(d.after);
    }
    return s.value();
}

// What a frame-skipping loop needs: `full` (every pixel, blink phase included) says whether to draw;
// `steady` (without the blink phase) says whether anything *happened*, so that a blinking cursor alone
// does not keep the loop at full speed. `steadyCursorVisible` is the cursor's visibility before blink.
struct PaneSignatureInputEx : PaneSignatureInput {
    bool steadyCursorVisible = true;
};
struct WindowSignatures {
    std::uint64_t full = 0;
    std::uint64_t steady = 0;
};
inline WindowSignatures windowSignatures(const std::vector<PaneSignatureInputEx> &panes,
                                         const std::vector<DividerBox> &dividers,
                                         std::uint64_t extra = 0) {
    FrameSignature full, steady;
    for (FrameSignature *s : {&full, &steady}) {
        s->add(extra);
        s->add(panes.size());
    }
    for (const auto &p : panes) {
        const auto cells = p.frame ? std::optional<std::uint64_t>(cellsSignature(*p.frame)) : std::nullopt;
        for (FrameSignature *s : {&full, &steady}) {
            addRect(*s, p.rect);
            addRect(*s, p.scrollbar);
            if (p.frame && p.palette) {
                s->add(true);
                addFrame(*s, *p.frame, *p.palette, cells, s == &full, p.steadyCursorVisible);
            }
            else {
                s->add(false);
            }
        }
    }
    for (FrameSignature *s : {&full, &steady}) {
        s->add(dividers.size());
        for (const auto &d : dividers) {
            addRect(*s, d.rect);
            s->add(d.direction);
            s->add(d.before);
            s->add(d.after);
        }
    }
    return {full.value(), steady.value()};
}
} // namespace frontend
