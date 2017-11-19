#include "Options.h"

#include "Constants.h"

#undef ENABLE_DBGS
#define ENABLE_DBGS 1
#include "Debug.h"

namespace sbt {

static std::string regs2str(Options::Regs regs)
{
    switch (regs) {
        case Options::Regs::GLOBALS:
            return "globals";
        case Options::Regs::LOCALS:
            return "locals";
    }
    return "???";
}

void Options::dump() const
{
    DBGS << "Options:\n";
    DBGS << "regs=" << regs2str(regs()) << nl;
    DBGS << "useLibC=" << useLibC() << nl;
}

}