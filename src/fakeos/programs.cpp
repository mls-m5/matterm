#include "programs.h"

namespace fakeos {

const std::vector<ProgramInfo> &programs() {
    static const std::vector<ProgramInfo> table = {
        {"fire", makeFire, "watch a fire burn"},
        {"gradient", makeGradient, "slow colors"},
        {"intro", makeIntro, "the opening animation"},
        {"less", makePager, "page through text"},
        {"top", makeTop, "watch the processes"},
        {"crumble", makeCrumble, "a word falls apart"},
        {"matrix", makeMatrix, "follow the white rabbit"},
        {"crash", makeCrash, "a machine without a disk"},
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
