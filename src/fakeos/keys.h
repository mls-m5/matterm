#pragma once

// The key decoder moved to the shared tty layer (src/tty/keys.h).
#include "tty/keys.h"

namespace fakeos {

using Key = tty::Key;
using KeyDecoder = tty::KeyDecoder;

} // namespace fakeos
