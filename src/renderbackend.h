#pragma once

#include "renderframe.h"

class RenderBackend {
public:
    virtual ~RenderBackend() = default;

    // Draw a complete frame; do not retain any borrowed frame data.
    virtual void render(const RenderFrame &frame) = 0;
    virtual void present() = 0;
};
