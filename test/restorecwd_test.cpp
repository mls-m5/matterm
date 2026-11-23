#include "mls-unit-test/unittest.h"
#include "restorecwd.h"

using namespace frontend;

TEST_SUIT_BEGIN(RestoreCwd)

TEST_CASE("A local daemon gets the saved folder only when it exists here") {
    EXPECT_EQ(cwdToRequest("/home/a", false, true), std::string("/home/a"));
    EXPECT_EQ(cwdToRequest("/gone", false, false), std::string());
    EXPECT_EQ(cwdToRequest("", false, true), std::string());
}

TEST_CASE("A daemon on another computer gets the saved folder whatever this "
          "computer has") {
    EXPECT_EQ(cwdToRequest("/srv/work", true, false), std::string("/srv/work"));
    EXPECT_EQ(cwdToRequest("/srv/work", true, true), std::string("/srv/work"));
    EXPECT_EQ(cwdToRequest("", true, false), std::string());
}

TEST_CASE("The note about a missing folder comes from where the terminal "
          "really runs") {
    EXPECT(restoredElsewhere("/srv/work", "/home/b"));
    EXPECT(!restoredElsewhere("/srv/work", "/srv/work"));
    EXPECT(!restoredElsewhere("/srv/work", "")); // Not known: no false note.
    EXPECT(!restoredElsewhere("", "/home/b"));
}

TEST_SUIT_END
