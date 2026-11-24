#include "emptyshell.h"
#include "mls-unit-test/unittest.h"

using namespace frontend;

namespace {
ShellFacts fresh() {
    ShellFacts f;
    f.panesInTab = 1;
    f.probeKnown = true;
    return f;
}
} // namespace

TEST_SUIT_BEGIN(EmptyShell)

TEST_CASE("A tab with one fresh shell is empty") {
    EXPECT(isEmptyShell(fresh()));
}

TEST_CASE("Anything typed makes it not empty, even a single key or a paste") {
    ShellFacts f = fresh();
    f.inputBytes = 1;
    EXPECT(!isEmptyShell(f));
    f.inputBytes = 4096;
    EXPECT(!isEmptyShell(f));
}

TEST_CASE("A running program, an unknown program, scrollback or a selection "
          "make it not empty") {
    ShellFacts busy = fresh();
    busy.busy = true;
    EXPECT(!isEmptyShell(busy));
    ShellFacts unknown = fresh();
    unknown.probeKnown = false; // The daemon did not say: do not guess.
    EXPECT(!isEmptyShell(unknown));
    ShellFacts scrolled = fresh();
    scrolled.scrollbackRows = 1;
    EXPECT(!isEmptyShell(scrolled));
    ShellFacts selecting = fresh();
    selecting.selecting = true;
    EXPECT(!isEmptyShell(selecting));
}

TEST_CASE("Only a tab with a single pane can be an empty shell") {
    ShellFacts f = fresh();
    f.panesInTab = 2;
    EXPECT(!isEmptyShell(f));
    f.panesInTab = 0;
    EXPECT(!isEmptyShell(f));
}

TEST_CASE("Pane-scope emptiness permits a split without weakening typed, unknown or busy guards") {
    auto facts = fresh();
    facts.panesInTab = 3;
    EXPECT(!isEmptyShell(facts));
    EXPECT(isEmptyShell(facts, false));
    facts.inputBytes = 1;
    EXPECT(!isEmptyShell(facts, false));
    facts.inputBytes = 0; facts.probeKnown = false;
    EXPECT(!isEmptyShell(facts, false));
    facts.probeKnown = true; facts.busy = true;
    EXPECT(!isEmptyShell(facts, false));
}
TEST_SUIT_END
