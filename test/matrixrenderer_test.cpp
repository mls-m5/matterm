#include "matrixrenderer.h"

#include <type_traits>

static_assert(!std::is_constructible_v<FStringView, FString &&>);
static_assert(FStringView{}.size() == 0);
static_assert(FChar{}.codePoint == U' ');

int main() {
    FString cells(4);
    cells[3].codePoint = U'ä';
    const CellGridView grid{FStringView{cells}, 2, 2};

    if (grid.cells.size() != grid.columns * grid.rows ||
        grid.cells.data() != cells.data()) {
        return 1;
    }

    return grid.cells.data()[grid.columns + 1].codePoint == U'ä' ? 0 : 1;
}
