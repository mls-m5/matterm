#pragma once

#include "fstringview.h"

#include <cstddef>

struct CellGridView {
    // Row-major, tightly packed: cells.size() == columns * rows.
    FStringView cells;
    std::size_t columns = 0;
    std::size_t rows = 0;
};
