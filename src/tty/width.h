#pragma once

namespace tty {

// How many terminal columns a code point takes: 0 (combining marks, joiners,
// variation selectors, controls), 1, or 2 (East Asian wide and fullwidth,
// emoji). A compact table, good enough for drawing; not a full wcwidth.
int cellWidth(char32_t codePoint);

} // namespace tty
