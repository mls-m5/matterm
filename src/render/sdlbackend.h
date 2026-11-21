#pragma once

#include "renderbackend.h"
#include "rgb8.h"

#include <memory>
#include <string>

struct SDL_Window;

// A rectangle in drawable pixels (this header knows nothing of src/frontend).
struct PixelBox {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const noexcept {
        return w <= 0 || h <= 0;
    }
};

// Where one pane of a window is drawn. `rect` is the pane (whole cells plus the
// leftover pixels, which are painted like the cell beside them). With a
// non-empty `scrollbar` (the half of the gutter next to the pane) the pane's
// scrollbar is drawn there and takes no columns from the program; with an empty
// one the strip inside `rect` is used as for a single terminal.
struct PaneArea {
    PixelBox rect;
    PixelBox scrollbar;
};

// Owns the SDL renderer and atlas. The borrowed window must outlive this
// object. Construct, use, and destroy on the GUI thread, with SDL/SDL_ttf
// initialized.
class SdlBackend final : public RenderBackend {
public:
    SdlBackend(SDL_Window &window,
               const std::string &fontPath,
               int pixelSize = 24,
               bool software = false);
    ~SdlBackend() override;
    SdlBackend(const SdlBackend &) = delete;
    SdlBackend &operator=(const SdlBackend &) = delete;
    SdlBackend(SdlBackend &&) = delete;
    SdlBackend &operator=(SdlBackend &&) = delete;

    void render(const RenderFrame &frame) override;
    void present() override;

    // A frame of several panes: beginFrame (clears the whole drawable once, no
    // viewport), then render(frame, area) for each pane, drawRect for the
    // dividers and any other chrome, then present(). render(frame) alone still
    // clears and draws one terminal as before. Throws std::invalid_argument for
    // an empty or negative area.
    void beginFrame(Rgb8 background);
    void render(const RenderFrame &frame, const PaneArea &area);
    // A filled rectangle in drawable pixels (a divider gutter), ignoring any
    // viewport.
    void drawRect(const PixelBox &box, Rgb8 color);
    // Grows by one at every present(); see GlyphAtlas::generation().
    std::uint64_t atlasGeneration() const noexcept;

    // Switch to another size of the same font without recreating the renderer.
    // Throws and keeps the old font if the new atlas cannot be built.
    void setPixelSize(int pixelSize);

    // Draw into a rectangle of the drawable (device pixels) instead of all of
    // it: one pane of a window with several. columns(), rows() and render()
    // then work on the rectangle (render() fills only it, with the palette
    // background; coordinates inside are relative to it). Throws
    // std::invalid_argument for an empty rectangle or a negative origin and
    // keeps the old setting. resetViewport() returns to the whole drawable.
    void setViewport(int x, int y, int width, int height);
    void resetViewport();
    // Fills the whole drawable, ignoring any viewport: the gaps between panes.
    void clear(Rgb8 color);

    // Drawable-pixel metrics; placement in RenderFrame is always in cells.
    int cellWidth() const noexcept;
    int cellHeight() const noexcept;
    // For frontends that reserve the scrollbar strip in their column count
    // (columns(true)): without a visible scrollbar, extend each row's last cell
    // background over the strip so it looks like part of the terminal. Off by
    // default (then the strip is only used while a scrollbar is visible).
    void setReserveScrollbarStrip(bool reserve);
    // Width of the strip reserved at the right edge for the scrollbar.
    static int scrollbarStrip() noexcept;
    // Reserve the scrollbar strip when computing text width if requested.
    int columns(bool scrollbarVisible = false) const;
    int rows() const;
    const char *rendererName() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
