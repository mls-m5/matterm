#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>

class TerminalMemory;

namespace demo {
void printBanner(std::ostream &output);
// Replace history with a static welcome screen at the current stored width.
void welcome(TerminalMemory &memory, std::size_t visibleRows);
void appendNumberedRow(TerminalMemory &memory, std::uint64_t number);
} // namespace demo
