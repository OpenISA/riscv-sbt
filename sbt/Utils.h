#ifndef SBT_UTILS_H
#define SBT_UTILS_H

#include "SBTError.h"

#include <llvm/Support/raw_ostream.h>

#include <vector>

namespace llvm {
namespace object {
class ObjectFile;
class SectionRef;
}
}

namespace sbt {

llvm::raw_ostream &logs(bool error = false);

#ifndef NDEBUG
static llvm::raw_ostream &DBGS = llvm::outs();
#else
static llvm::raw_ostream &DBGS = llvm::nulls();
#endif

static inline llvm::Error error(SBTError &SE)
{
  return llvm::make_error<SBTError>(std::move(SE));
}

uint64_t getELFOffset(const llvm::object::SectionRef &Section);

typedef std::vector<std::pair<uint64_t, llvm::StringRef>> SymbolVec;

// get all symbols present in Obj that belong to this Section
llvm::Expected<SymbolVec> getSymbolsList(
  const llvm::object::ObjectFile *Obj,
  const llvm::object::SectionRef &Section);

} // sbt

#endif
