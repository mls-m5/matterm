#pragma once

#include <chrono>

namespace protocol {

// Time is passed in or read from an injected clock (milliseconds since any
// fixed origin), so state machines have no hidden clock and tests are
// deterministic.
using Time = std::chrono::milliseconds;

} // namespace protocol
