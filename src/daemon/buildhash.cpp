#include "control.h"
#include "version.h"

#ifndef MATTERM_BUILD_HASH
#define MATTERM_BUILD_HASH "unknown"
#endif

namespace mtd {

Version ownVersion() {
    Version v;
    v.format = kControlFormat;
    v.breaking = kControlBreaking;
    v.additive = kControlAdditive;
    v.hash = MATTERM_BUILD_HASH;
    return v;
}

std::string welcomeBuild(std::string_view program, const Version &v) {
    return std::string(program) + " " + toString(v);
}

} // namespace mtd
