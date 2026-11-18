#include "cellgridview.h"
#include "mls-unit-test/unittest.h"

#include <type_traits>

static_assert(!std::is_constructible_v<FStringView, FString &&>);
static_assert(FStringView{}.size() == 0);
static_assert(FChar{}.codePoint == U' ');

TEST_SUIT_BEGIN(CellGridView)

TEST_CASE("Cell grid borrows independent rows without copying") {
    FString first(2);
    FString second(2);
    second[1].codePoint = U'ä';
    const FStringView rows[]{FStringView(first), FStringView(second)};
    const CellGridView grid{rows, 2, 2};

    EXPECT_EQ(grid.columns(), std::size_t(2));
    EXPECT_EQ(grid.rows(), std::size_t(2));
    EXPECT_EQ(grid.row(0).data(), first.data());
    EXPECT_EQ(grid.row(1).data(), second.data());
    EXPECT_EQ(grid.row(1).size(), std::size_t(2));
    EXPECT(grid.row(1).data()[1].codePoint == U'ä');
    EXPECT_THROW(grid.row(2), std::out_of_range);
}

TEST_CASE("Default grid is empty") {
    const CellGridView grid;
    EXPECT_EQ(grid.columns(), std::size_t(0));
    EXPECT_EQ(grid.rows(), std::size_t(0));
    EXPECT_THROW(grid.row(0), std::out_of_range);
}

TEST_SUIT_END
