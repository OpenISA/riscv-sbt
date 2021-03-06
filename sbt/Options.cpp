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
        case Options::Regs::ABI:
            return "abi";
    }
    xunreachable("invalid regs");
}

void Options::dump() const
{
    DBGS << "Options:\n";
    DBGS << "regs=" << regs2str(regs()) << nl;
    DBGS << "useLibC=" << useLibC() << nl;
    DBGS << "stackSize=" << stackSize() << nl;
    DBGS << "syncFRegs=" << syncFRegs() << nl;
    DBGS << "a2s=" << a2s() << nl;
    DBGS << "syncOnExternalCalls=" << syncOnExternalCalls() << nl;
    DBGS << "commentedAsm=" << commentedAsm() << nl;
    DBGS << "symBoundsCheck=" << symBoundsCheck() << nl;
    DBGS << "enableFCSR=" << enableFCSR() << nl;
    DBGS << "enableFCVTValidation=" << enableFCVTValidation() << nl;
    DBGS << "hardFloatABI=" << hardFloatABI() << nl;
    DBGS << "optStack=" << optStack() << nl;
    DBGS << "icallIntOnly=" << icallIntOnly() << nl;
    DBGS << "logFile=" << logFile() << nl;
}

}
