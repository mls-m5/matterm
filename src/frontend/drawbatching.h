#pragma once

#include <cstdint>

namespace frontend {
// Presentation-only A/B policy. Ingestion, parsing, transport and acks must not
// depend on this switch. Input/expose/resize can always request an immediate draw.
inline bool holdFloodDraw(bool enabled,
                          bool flooded,
                          bool immediate,
                          std::int64_t elapsedMs) {
    return enabled && flooded && !immediate && elapsedMs >= 0 && elapsedMs < 100;
}
} // namespace frontend
