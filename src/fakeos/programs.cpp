#include "programs.h"

namespace fakeos {

const std::vector<ProgramInfo> &programs() {
    static const std::vector<ProgramInfo> table = {
        {"fire", makeFire, "watch a fire burn"},
        {"gradient", makeGradient, "slow colors"},
        {"intro", makeIntro, "the opening animation"},
    };
    return table;
}

const ProgramInfo *findProgram(const std::string &name) {
    for (const auto &p : programs()) {
        if (name == p.name) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace fakeos
