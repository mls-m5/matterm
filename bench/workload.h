#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace benchmark {
// No random generation, files, or profiler scopes in the timed workload.
// Output assumes a grid at least 80 columns by 24 rows for comparable results.
std::string workload(std::string_view name, std::size_t approximateBytes);
} // namespace benchmark
