#pragma once

#include "renderbackend.h"

#include <memory>
#include <string>

struct SDL_Window;

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

    // Switch to another size of the same font without recreating the renderer.
    // Throws and keeps the old font if the new atlas cannot be built.
    void setPixelSize(int pixelSize);

    // Drawable-pixel metrics; placement in RenderFrame is always in cells.
    int cellWidth() const noexcept;
    int cellHeight() const noexcept;
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
