#ifndef SBT_CONTEXT_H
#define SBT_CONTEXT_H

#include "Constants.h"
#include "Map.h"
#include "Pointer.h"
#include "Types.h"
#include "Utils.h"

#include <llvm/IR/IRBuilder.h>

namespace sbt {

class Builder;
class Disassembler;
class Function;
class SBTRelocation;
class SBTSection;
class Stack;
class Syscall;
class Translator;
class XRegister;
class XRegisters;

/**
 * SBT "context".
 *
 * This class is used to pass data/class instances that may be used
 * through several distinct but related classes.
 */
class Context
{
public:
  using FunctionPtr = Pointer<Function>;

  /**
   * Build context.
   *
   * @param ctx LLVM context
   * @param mod LLVM module
   * @param bld LLVM IR builder
   */
  Context(
    llvm::LLVMContext* ctx,
    llvm::Module* mod,
    llvm::IRBuilder<>* bld)
    :
    ctx(ctx),
    module(mod),
    builder(bld),
    t(*ctx)
  {
    c.init(ctx);
  }

  /**
   * Destructor.
   *
   * (out of line to allow forward declaration of most members)
   */
  ~Context();

  // program scope stuff

  // llvm
  llvm::LLVMContext* ctx;
  llvm::Module* module;
  llvm::IRBuilder<>* builder;
  // translator
  Translator* translator = nullptr;
  // disassembler
  Disassembler* disasm = nullptr;
  // constants
  Constants c;
  // types
  Types t;
  // x registers
  ArrayPtr<XRegisters, XRegister> x;
  // syscall handler
  Syscall* syscall = nullptr;
  // stack
  Stack* stack = nullptr;
  // flags
  // inside C main function?
  bool inMain = false;
  // function maps
  Map<std::string, FunctionPtr>* _func = nullptr;
  Map<uint64_t, Function*>* _funcByAddr = nullptr;

  // get function by name
  Function* func(const std::string& name, bool assertNotNull = true) const
  {
    FunctionPtr* f = (*_func)[name];
    if (assertNotNull)
      xassert(f);
    else if (!f)
      return nullptr;
    return &**f;
  }

  // get function by guest address
  Function* funcByAddr(uint64_t addr, bool assertNotNull = true) const
  {
    Function** f = (*_funcByAddr)[addr];
    if (assertNotNull)
      xassert(f);
    else if (!f)
      return nullptr;
    return *f;
  }

  // add function to maps
  void addFunc(FunctionPtr&& f);

  // module scope
  // (module == object file)

  // object
  // ConstObjectPtr obj = nullptr;
  // shadow image
  llvm::GlobalVariable* shadowImage = nullptr;

  // section scope
  // (e.g.: .text)

  // section
  SBTSection* sec = nullptr;
  // relocator
  SBTRelocation* reloc = nullptr;
  // builder
  Builder* bld = nullptr;
  // function
  Function* f = nullptr;

  // function scope

  // flags
  // last instruction was a branch?
  bool brWasLast = false;
};

}

#endif
