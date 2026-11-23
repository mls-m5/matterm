#pragma once

// The cell buffer and its VT output moved to the shared tty layer
// (src/tty/); these names stay for the programs and tests that use them.
#include "tty/canvas.h"
#include "tty/color.h"
#include "tty/utf8.h"
#include "types.h"

namespace fakeos {

using Cell = tty::Cell;
using AsciiCanvas = tty::Canvas;
using tty::appendUtf8;
using tty::decodeUtf8;
using tty::nearestAnsi16;
using tty::nearestXterm256;

} // namespace fakeos
