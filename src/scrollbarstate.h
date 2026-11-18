#pragma once

// Backend-independent scrollbar proportions. The backend chooses pixel
// geometry.
struct ScrollbarState {
    bool visible = false;
    float position = 0; // 0 = top, 1 = bottom of the thumb's available travel.
    float size = 1;     // Visible fraction of the document.
};
