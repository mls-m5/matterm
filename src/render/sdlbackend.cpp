#include "sdlbackend.h"

#include "glyphatlas.h"
#include "wrapmark.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
constexpr int scrollbarWidth = 12;

void check(int result) {
    if (result < 0) {
        throw std::runtime_error(SDL_GetError());
    }
}

SDL_Renderer *createRenderer(SDL_Window &window, bool software) {
    auto *renderer = SDL_CreateRenderer(
        &window,
        -1,
        software ? SDL_RENDERER_SOFTWARE
                 : SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        throw std::runtime_error(SDL_GetError());
    }
    return renderer;
}

struct Batch {
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;

    void clear() {
        vertices.clear();
        indices.clear();
    }

    void quad(float x,
              float y,
              float w,
              float h,
              SDL_Color color,
              float u = 0,
              float v = 0,
              float u2 = 0,
              float v2 = 0) {
        if (vertices.size() >
                std::size_t(std::numeric_limits<int>::max() - 4) ||
            indices.size() > std::size_t(std::numeric_limits<int>::max() - 6)) {
            throw std::length_error("SDL geometry batch too large");
        }
        const int base = int(vertices.size());
        vertices.insert(vertices.end(),
                        {{{x, y}, color, {u, v}},
                         {{x + w, y}, color, {u2, v}},
                         {{x + w, y + h}, color, {u2, v2}},
                         {{x, y + h}, color, {u, v2}}});
        for (int i : {0, 1, 2, 0, 2, 3}) {
            indices.push_back(base + i);
        }
    }

    void draw(SDL_Renderer &renderer, SDL_Texture *texture) const {
        if (!indices.empty()) {
            check(SDL_RenderGeometry(&renderer,
                                     texture,
                                     vertices.data(),
                                     int(vertices.size()),
                                     indices.data(),
                                     int(indices.size())));
        }
    }
};
} // namespace

struct SdlBackend::Impl {
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer;
    std::string fontPath;
    std::unique_ptr<GlyphAtlas> atlas;
    SDL_RendererInfo info{};
    int cellWidth;
    int cellHeight;
    int textureWidth;
    int textureHeight;
    Batch backgrounds;
    Batch glyphs;
    Batch decorations;
    Palette defaultPalette;
    bool reserveStrip = false;
    std::optional<SDL_Rect> viewport;
    // Set while drawing a pane of a multi-pane frame: the scrollbar goes to
    // this rectangle (window pixels) and the leftover pixels are painted.
    bool paneMode = false;
    std::optional<SDL_Rect> externalTrack;

    // The size drawing and the cell counts work on: the viewport or the whole
    // drawable.
    void outputSize(int &width, int &height) const {
        if (viewport) {
            width = viewport->w;
            height = viewport->h;
            return;
        }
        check(SDL_GetRendererOutputSize(renderer.get(), &width, &height));
    }

    Impl(SDL_Window &window, const std::string &path, int size, bool software)
        : renderer(createRenderer(window, software), SDL_DestroyRenderer)
        , fontPath(path)
        , atlas(std::make_unique<GlyphAtlas>(*renderer, path, size))
        , cellWidth(std::max(1, atlas->getOrInsert(U'M').advanceX))
        , cellHeight(std::max(1, atlas->lineHeight())) {
        check(SDL_GetRendererInfo(renderer.get(), &info));
        check(SDL_QueryTexture(
            atlas->texture(), nullptr, nullptr, &textureWidth, &textureHeight));
    }
};

SdlBackend::SdlBackend(SDL_Window &window,
                       const std::string &fontPath,
                       int pixelSize,
                       bool software)
    : impl_(std::make_unique<Impl>(window, fontPath, pixelSize, software)) {}

SdlBackend::~SdlBackend() = default;

// One filled triangle per row that continues on the next
// (docs/tasks/wrap-marker.md). In the strip at the pane's own right edge, or,
// for a pane with its scrollbar in a divider, in the far half of that divider.
template <class Impl>
void drawWrapMarks(const Impl &s,
                   const RenderFrame &frame,
                   std::size_t rows,
                   int textHeight,
                   int width,
                   int height,
                   bool external) {
    if (frame.wrapMarks.empty() || rows == 0) {
        return;
    }
    const auto &style = wrapmark::kStyle;
    // The region (window coordinates when `external`, else relative to the
    // pane's viewport) and its clip.
    float regionX = 0;
    int regionWidth = 0;
    float originY = 0;
    SDL_Rect clip{0, 0, width, height};
    if (external) {
        const auto &track = *s.externalTrack;
        // The far half of the divider: as wide as the near half, which holds
        // the scrollbar.
        regionX = float(track.x + track.w + 1);
        regionWidth = track.w - 1;
        if (s.viewport) {
            originY = float(
                s.viewport
                    ->y); // Rows are relative to the pane, the divider is not.
        }
        clip = SDL_Rect{track.x + track.w, track.y, track.w, track.h};
        check(SDL_RenderSetViewport(s.renderer.get(), nullptr));
    }
    else {
        const bool strip = frame.scrollbar.visible || s.reserveStrip;
        const auto layout = wrapmark::stripLayout(scrollbarWidth, true, style);
        if (!strip || !layout.hasMarks()) {
            return;
        }
        regionX = float(width - scrollbarWidth + layout.markX);
        regionWidth = scrollbarWidth - layout.markX;
    }
    std::vector<SDL_Vertex> vertices;
    const SDL_Color color{style.color.r, style.color.g, style.color.b, 255};
    for (std::size_t y = 0; y < rows && y < frame.wrapMarks.size(); ++y) {
        if (!frame.wrapMarks[y]) {
            continue;
        }
        const float top = float(y) * s.cellHeight + frame.verticalOffsetPixels;
        if (top + float(s.cellHeight) <= 0 || top >= float(textHeight)) {
            continue;
        }
        if (const auto t = wrapmark::triangleFor(
                regionX, originY + top, s.cellHeight, regionWidth, style)) {
            for (const auto &p : {t->a, t->b, t->c}) {
                vertices.push_back(SDL_Vertex{{p.x, p.y}, color, {0, 0}});
            }
        }
    }
    if (!vertices.empty()) {
        if (external) {
            check(SDL_RenderSetClipRect(s.renderer.get(), &clip));
        }
        check(SDL_RenderGeometry(s.renderer.get(),
                                 nullptr,
                                 vertices.data(),
                                 int(vertices.size()),
                                 nullptr,
                                 0));
        if (external) {
            check(SDL_RenderSetClipRect(s.renderer.get(), nullptr));
        }
    }
}

void SdlBackend::render(const RenderFrame &frame) {
    auto &s = *impl_;
    const auto &palette = frame.palette ? *frame.palette : s.defaultPalette;
    int width, height;
    s.outputSize(width, height);
    if (!std::isfinite(frame.verticalOffsetPixels) ||
        frame.verticalOffsetPixels > 0 ||
        frame.verticalOffsetPixels <= -s.cellHeight ||
        frame.textHeightPixels < -1 ||
        (frame.scrollbar.visible && (!std::isfinite(frame.scrollbar.position) ||
                                     !std::isfinite(frame.scrollbar.size)))) {
        throw std::invalid_argument("Invalid frame viewport");
    }
    const bool external = s.externalTrack.has_value();
    const int textWidth = frame.scrollbar.visible && !external
                              ? std::max(0, width - scrollbarWidth)
                              : width;
    const int textHeight = frame.textHeightPixels < 0
                               ? height
                               : std::min(height, frame.textHeightPixels);
    const auto columns = std::min(
        frame.cells.columns(),
        std::size_t(textWidth / s.cellWidth + (textWidth % s.cellWidth != 0)));
    const auto rows =
        textHeight == 0
            ? std::size_t(0)
            : std::min(
                  frame.cells.rows(),
                  std::size_t(std::ceil((double(textHeight) -
                                         double(frame.verticalOffsetPixels)) /
                                        s.cellHeight)));
    // The edge cells' backgrounds, to paint the leftover pixels like them.
    std::vector<Rgb8> rowEdge(rows), columnEdge(columns);
    s.backgrounds.clear();
    s.glyphs.clear();
    s.decorations.clear();

    // Resolve all cache misses before submitting draw calls. The frame's views
    // are consumed synchronously; only copied vertex data survives this call.
    for (std::size_t y = 0; y < rows; ++y) {
        const auto row = frame.cells.row(y);
        for (std::size_t x = 0; x < columns; ++x) {
            s.atlas->getOrInsert(row.data()[x].codePoint,
                                 row.data()[x].attributes);
        }
    }
    const bool gliding =
        frame.cursor.columnOffset != 0 || frame.cursor.rowOffset != 0;
    const bool filledBlock = frame.cursor.visible && frame.cursor.focused &&
                             !gliding &&
                             frame.cursor.shape == CursorState::Shape::Block &&
                             frame.cursor.column >= 0 && frame.cursor.row >= 0;
    for (std::size_t y = 0; y < rows; ++y) {
        const auto row = frame.cells.row(y);
        for (std::size_t x = 0; x < columns; ++x) {
            const auto &cell = row.data()[x];
            const float left = float(x) * s.cellWidth;
            const float top =
                float(y) * s.cellHeight + frame.verticalOffsetPixels;
            auto colors = palette.resolve(cell);
            const auto baseBackground = colors.background;
            if (filledBlock && std::size_t(frame.cursor.column) == x &&
                std::size_t(frame.cursor.row) == y) {
                // Invert the cell: the glyph keeps showing in the cell color.
                colors.foreground = colors.background;
                colors.background = frame.cursor.color;
            }
            if (frame.selectionBegin < frame.selectionEnd) {
                const auto index = y * frame.cells.columns() + x;
                if (index >= frame.selectionBegin &&
                    index < frame.selectionEnd) {
                    colors.foreground = {255, 255, 255};
                    colors.background = {49, 86, 145};
                }
            }
            {
                const auto index = y * frame.cells.columns() + x;
                const auto inRange = [index](const auto &ranges) {
                    for (const auto &r : ranges) {
                        if (index >= r.first && index < r.second) {
                            return true;
                        }
                    }
                    return false;
                };
                if (!frame.matchRanges.empty() && inRange(frame.matchRanges)) {
                    colors.foreground = {20, 20, 20};
                    colors.background = {230, 190, 70};
                }
                if (!frame.selectionRanges.empty() &&
                    inRange(frame.selectionRanges)) {
                    colors.foreground = {255, 255, 255};
                    colors.background = {49, 86, 145};
                }
            }
            s.backgrounds.quad(left,
                               top,
                               float(s.cellWidth),
                               float(s.cellHeight),
                               {colors.background.r,
                                colors.background.g,
                                colors.background.b,
                                255});
            if (s.paneMode) {
                const bool selected =
                    frame.selectionBegin < frame.selectionEnd &&
                    y * frame.cells.columns() + x >= frame.selectionBegin &&
                    y * frame.cells.columns() + x < frame.selectionEnd;
                const auto edge = selected ? Rgb8{49, 86, 145} : baseBackground;
                if (x + 1 == columns) {
                    rowEdge[y] = edge;
                }
                if (y + 1 == rows) {
                    columnEdge[x] = edge;
                }
            }
            if (s.reserveStrip && !external && !frame.scrollbar.visible &&
                x + 1 == columns) {
                // The reserved scrollbar strip (and any remainder) looks like
                // the cell beside it while there is no scrollbar. A cursor
                // block is not extended; a selection is.
                const bool selected =
                    frame.selectionBegin < frame.selectionEnd &&
                    y * frame.cells.columns() + x >= frame.selectionBegin &&
                    y * frame.cells.columns() + x < frame.selectionEnd;
                const auto edge = selected ? Rgb8{49, 86, 145} : baseBackground;
                const float extraLeft = left + float(s.cellWidth);
                if (float(width) > extraLeft) {
                    s.backgrounds.quad(extraLeft,
                                       top,
                                       float(width) - extraLeft,
                                       float(s.cellHeight),
                                       {edge.r, edge.g, edge.b, 255});
                }
            }
            const auto index = y * frame.cells.columns() + x;
            const bool overlayUnderline =
                index >= frame.underlineBegin && index < frame.underlineEnd;
            const bool hiddenByBlink =
                hasAttribute(cell.attributes, CellAttribute::Blink) &&
                !frame.textBlinkVisible;
            const Rgb8 foreground = colors.foreground;
            const SDL_Color ink{foreground.r, foreground.g, foreground.b, 255};
            if (overlayUnderline ||
                (!hiddenByBlink &&
                 hasAttribute(cell.attributes, CellAttribute::Underline))) {
                s.decorations.quad(left,
                                   top + std::max(0, s.cellHeight - 2),
                                   float(s.cellWidth),
                                   1,
                                   ink);
            }
            if (hiddenByBlink) {
                continue;
            }
            if (hasAttribute(cell.attributes, CellAttribute::Strike)) {
                s.decorations.quad(
                    left, top + s.cellHeight / 2, float(s.cellWidth), 1, ink);
            }
            const auto glyph =
                s.atlas->getOrInsert(cell.codePoint, cell.attributes);
            if (glyph.source.w == 0 || glyph.source.h == 0) {
                continue;
            }
            s.glyphs.quad(
                left + glyph.bearingX,
                top + s.atlas->ascent() - glyph.bearingY,
                float(glyph.source.w),
                float(glyph.source.h),
                ink,
                float(glyph.source.x) / s.textureWidth,
                float(glyph.source.y) / s.textureHeight,
                float(glyph.source.x + glyph.source.w) / s.textureWidth,
                float(glyph.source.y + glyph.source.h) / s.textureHeight);
        }
    }
    if (s.paneMode && columns > 0 && rows > 0) {
        // Pixels right of and below the last whole cell look like the cell
        // beside them, as the reserved scrollbar strip does.
        const float cellsRight = float(columns) * s.cellWidth;
        const float cellsBottom =
            float(rows) * s.cellHeight + frame.verticalOffsetPixels;
        auto color = [](Rgb8 c) { return SDL_Color{c.r, c.g, c.b, 255}; };
        if (float(textWidth) > cellsRight) {
            for (std::size_t y = 0; y < rows; ++y) {
                s.backgrounds.quad(cellsRight,
                                   float(y) * s.cellHeight +
                                       frame.verticalOffsetPixels,
                                   float(textWidth) - cellsRight,
                                   float(s.cellHeight),
                                   color(rowEdge[y]));
            }
        }
        if (float(textHeight) > cellsBottom) {
            for (std::size_t x = 0; x < columns; ++x) {
                s.backgrounds.quad(float(x) * s.cellWidth,
                                   cellsBottom,
                                   float(s.cellWidth),
                                   float(textHeight) - cellsBottom,
                                   color(columnEdge[x]));
            }
            if (float(textWidth) > cellsRight) {
                s.backgrounds.quad(cellsRight,
                                   cellsBottom,
                                   float(textWidth) - cellsRight,
                                   float(textHeight) - cellsBottom,
                                   color(columnEdge[columns - 1]));
            }
        }
    }
    check(SDL_SetRenderDrawColor(s.renderer.get(),
                                 palette.background.r,
                                 palette.background.g,
                                 palette.background.b,
                                 255));
    // SDL_RenderClear ignores the viewport and would wipe the other panes.
    struct ViewportReset {
        SDL_Renderer *renderer;
        bool active;
        ~ViewportReset() {
            if (active) {
                SDL_RenderSetViewport(renderer, nullptr);
            }
        }
    } viewportReset{s.renderer.get(), s.viewport.has_value()};
    if (s.viewport) {
        check(SDL_RenderSetViewport(s.renderer.get(), &*s.viewport));
        const SDL_Rect all{0, 0, width, height};
        check(SDL_RenderFillRect(s.renderer.get(), &all));
    }
    else {
        check(SDL_RenderClear(s.renderer.get()));
    }
    const SDL_Rect textClip{0, 0, textWidth, textHeight};
    check(SDL_RenderSetClipRect(s.renderer.get(), &textClip));
    // Restore clipping even if a draw fails. The backend owns the renderer.
    struct ClipReset {
        SDL_Renderer *renderer;
        ~ClipReset() {
            SDL_RenderSetClipRect(renderer, nullptr);
        }
    } clipReset{s.renderer.get()};
    s.backgrounds.draw(*s.renderer, nullptr);
    s.glyphs.draw(*s.renderer, s.atlas->texture());
    s.decorations.draw(*s.renderer, nullptr);

    const auto &cursor = frame.cursor;
    if (cursor.visible && cursor.column >= 0 && cursor.row >= 0 &&
        std::size_t(cursor.column) < columns &&
        std::size_t(cursor.row) < rows) {
        SDL_FRect rect{(float(cursor.column) + cursor.columnOffset) *
                           s.cellWidth,
                       (float(cursor.row) + cursor.rowOffset) * s.cellHeight +
                           frame.verticalOffsetPixels,
                       float(s.cellWidth),
                       float(s.cellHeight)};
        check(SDL_SetRenderDrawColor(s.renderer.get(),
                                     cursor.color.r,
                                     cursor.color.g,
                                     cursor.color.b,
                                     255));
        if (!cursor.focused) {
            // Inactive window: hollow outline, whatever the shape.
            check(SDL_RenderDrawRectF(s.renderer.get(), &rect));
        }
        else if (gliding && cursor.shape == CursorState::Shape::Block) {
            // Moving block: translucent so the text stays readable; the exact
            // inverted block returns when the glide settles.
            check(SDL_SetRenderDrawBlendMode(s.renderer.get(),
                                             SDL_BLENDMODE_BLEND));
            check(SDL_SetRenderDrawColor(s.renderer.get(),
                                         cursor.color.r,
                                         cursor.color.g,
                                         cursor.color.b,
                                         150));
            check(SDL_RenderFillRectF(s.renderer.get(), &rect));
            check(SDL_SetRenderDrawBlendMode(s.renderer.get(),
                                             SDL_BLENDMODE_NONE));
        }
        else if (cursor.shape != CursorState::Shape::Block) {
            if (cursor.shape == CursorState::Shape::Underline) {
                rect.h = float(std::min(2, s.cellHeight));
                rect.y += s.cellHeight - rect.h;
            }
            else {
                rect.w = float(std::min(2, s.cellWidth));
            }
            check(SDL_RenderFillRectF(s.renderer.get(), &rect));
        }
        // A focused block was already drawn by inverting its cell.
    }
    check(SDL_RenderSetClipRect(s.renderer.get(), nullptr));
    if (frame.scrollbar.visible && width > 0 && height > 0) {
        SDL_Rect track{textWidth, 0, width - textWidth, height};
        if (external) {
            // Outside the pane's viewport, in the gutter (window coordinates).
            check(SDL_RenderSetViewport(s.renderer.get(), nullptr));
            track = *s.externalTrack;
        }
        const int stripWidth = track.w;
        const int trackHeight = track.h;
        check(SDL_SetRenderDrawColor(s.renderer.get(), 25, 29, 36, 255));
        check(SDL_RenderFillRect(s.renderer.get(), &track));
        const float thumbHeight = std::clamp(
            float(trackHeight) * std::clamp(frame.scrollbar.size, 0.0f, 1.0f),
            float(std::min(16, trackHeight)),
            float(trackHeight));
        const float thumbY =
            float(track.y) + std::clamp(frame.scrollbar.position, 0.0f, 1.0f) *
                                 (float(trackHeight) - thumbHeight);
        // With wrapped-line marks the strip's right part is the thumb's
        // (docs/tasks/wrap-marker.md).
        const auto strip = wrapmark::stripLayout(
            stripWidth, !external && !frame.wrapMarks.empty());
        const SDL_FRect thumb{
            float(track.x + (external ? 1 : strip.thumbX)),
            thumbY,
            float(external ? std::max(0, stripWidth - 2) : strip.thumbWidth),
            thumbHeight};
        check(SDL_SetRenderDrawColor(s.renderer.get(), 110, 122, 142, 255));
        check(SDL_RenderFillRectF(s.renderer.get(), &thumb));
    }
    drawWrapMarks(s, frame, rows, textHeight, width, height, external);
}

void SdlBackend::beginFrame(Rgb8 background) {
    resetViewport();
    clear(background);
}

void SdlBackend::render(const RenderFrame &frame, const PaneArea &area) {
    auto &s = *impl_;
    const auto &r = area.rect;
    if (r.empty() || r.x < 0 || r.y < 0) {
        throw std::invalid_argument("Invalid pane area");
    }
    const auto &sb = area.scrollbar;
    if (!sb.empty() && (sb.x < 0 || sb.y < 0)) {
        throw std::invalid_argument("Invalid pane scrollbar area");
    }
    struct Restore {
        Impl &s;
        std::optional<SDL_Rect> viewport;
        ~Restore() {
            s.viewport = viewport;
            s.paneMode = false;
            s.externalTrack.reset();
        }
    } restore{s, s.viewport};
    s.viewport = SDL_Rect{r.x, r.y, r.w, r.h};
    s.paneMode = true;
    if (!sb.empty()) {
        s.externalTrack = SDL_Rect{sb.x, sb.y, sb.w, sb.h};
    }
    render(frame);
}

void SdlBackend::drawRect(const PixelBox &box, Rgb8 color) {
    auto &s = *impl_;
    if (box.empty()) {
        return;
    }
    check(SDL_RenderSetViewport(s.renderer.get(), nullptr));
    check(SDL_SetRenderDrawColor(
        s.renderer.get(), color.r, color.g, color.b, 255));
    const SDL_Rect rect{box.x, box.y, box.w, box.h};
    check(SDL_RenderFillRect(s.renderer.get(), &rect));
}

std::uint64_t SdlBackend::atlasGeneration() const noexcept {
    return impl_->atlas->generation();
}

void SdlBackend::setViewport(int x, int y, int width, int height) {
    if (width <= 0 || height <= 0 || x < 0 || y < 0) {
        throw std::invalid_argument("Invalid viewport");
    }
    impl_->viewport = SDL_Rect{x, y, width, height};
}
void SdlBackend::resetViewport() {
    impl_->viewport.reset();
}
void SdlBackend::clear(Rgb8 color) {
    auto &s = *impl_;
    check(SDL_RenderSetViewport(s.renderer.get(), nullptr));
    check(SDL_SetRenderDrawColor(
        s.renderer.get(), color.r, color.g, color.b, 255));
    check(SDL_RenderClear(s.renderer.get()));
}

void SdlBackend::present() {
    SDL_RenderPresent(impl_->renderer.get());
    impl_->atlas->nextGeneration();
}
void SdlBackend::setPixelSize(int pixelSize) {
    auto &s = *impl_;
    // Build the new atlas first: a failure leaves the old font in place. The
    // renderer (and with it the page's touch and input registration) is kept.
    auto atlas =
        std::make_unique<GlyphAtlas>(*s.renderer, s.fontPath, pixelSize);
    const int width = std::max(1, atlas->getOrInsert(U'M').advanceX);
    const int height = std::max(1, atlas->lineHeight());
    int textureWidth = 0, textureHeight = 0;
    check(SDL_QueryTexture(
        atlas->texture(), nullptr, nullptr, &textureWidth, &textureHeight));
    atlas->setGeneration(s.atlas->generation());
    s.atlas = std::move(atlas);
    s.cellWidth = width;
    s.cellHeight = height;
    s.textureWidth = textureWidth;
    s.textureHeight = textureHeight;
}

void SdlBackend::setReserveScrollbarStrip(bool reserve) {
    impl_->reserveStrip = reserve;
}

int SdlBackend::scrollbarStrip() noexcept {
    return scrollbarWidth;
}

int SdlBackend::cellWidth() const noexcept {
    return impl_->cellWidth;
}
int SdlBackend::cellHeight() const noexcept {
    return impl_->cellHeight;
}
int SdlBackend::columns(bool scrollbarVisible) const {
    int width, height;
    impl_->outputSize(width, height);
    return (scrollbarVisible ? std::max(0, width - scrollbarWidth) : width) /
           cellWidth();
}
int SdlBackend::rows() const {
    int width, height;
    impl_->outputSize(width, height);
    return height / cellHeight();
}
const char *SdlBackend::rendererName() const noexcept {
    return impl_->info.name;
}
