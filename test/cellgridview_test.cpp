#include "cellgridview.h"
#include "mls-unit-test/unittest.h"

#include <type_traits>

static_assert(!std::is_constructible_v<FStringView, FString &&>);
static_assert(FStringView{}.size() == 0);
static_assert(FChar{}.codePoint == U' ');

TEST_SUIT_BEGIN(CellGridView)

TEST_CASE("Cell grid borrows contiguous row-major cells") {
    FString cells(4);
    cells[3].codePoint = U'ä';
    const CellGridView grid{FStringView{cells}, 2, 2};

    EXPECT_EQ(grid.cells.size(), grid.columns * grid.rows);
    EXPECT_EQ(grid.cells.data(), cells.data());
    EXPECT(grid.cells.data()[grid.columns + 1].codePoint == U'ä');
}

TEST_SUIT_END
