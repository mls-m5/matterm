#pragma once

// `matterm --window-test`, the transparency part (docs/tasks/csd-window.md step 1): can a window of
// this desktop be see-through? Implemented in transparencyprobe.cpp, which alone includes
// SDL_syswm.h (it pulls in Xlib, whose macros such as `None` break other headers).
#include <iosfwd>

namespace borderless {

void probeTransparency(std::ostream &out);

} // namespace borderless
