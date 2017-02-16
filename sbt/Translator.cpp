#include "Translator.h"

#include "SBTError.h"
#include "Utils.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/FormatVariadic.h>

// LLVM internal instruction info
#define GET_INSTRINFO_ENUM
#include <lib/Target/RISCVMaster/RISCVMasterGenInstrInfo.inc>
#define GET_REGINFO_ENUM
#include <lib/Target/RISCVMaster/RISCVMasterGenRegisterInfo.inc>

using namespace llvm;

namespace sbt {

Translator::Translator(
  LLVMContext *Ctx,
  IRBuilder<> *Bldr,
  llvm::Module *Mod)
  :
  I32(Type::getInt32Ty(*Ctx)),
  I32Ptr(Type::getInt32PtrTy(*Ctx)),
  ZERO(ConstantInt::get(I32, 0)),
  Context(Ctx),
  Builder(Bldr),
  Module(Mod)
{
}

// MCInst register number to RISCV register number
unsigned Translator::RVReg(unsigned Reg)
{
  namespace RISCV = RISCVMaster;

  switch (Reg) {
    case RISCV::X0_32:  return 0;
    case RISCV::X1_32:  return 1;
    case RISCV::X2_32:  return 2;
    case RISCV::X3_32:  return 3;
    case RISCV::X4_32:  return 4;
    case RISCV::X5_32:  return 5;
    case RISCV::X6_32:  return 6;
    case RISCV::X7_32:  return 7;
    case RISCV::X8_32:  return 8;
    case RISCV::X9_32:  return 9;
    case RISCV::X10_32: return 10;
    case RISCV::X11_32: return 11;
    case RISCV::X12_32: return 12;
    case RISCV::X13_32: return 13;
    case RISCV::X14_32: return 14;
    case RISCV::X15_32: return 15;
    case RISCV::X16_32: return 16;
    case RISCV::X17_32: return 17;
    case RISCV::X18_32: return 18;
    case RISCV::X19_32: return 19;
    case RISCV::X20_32: return 20;
    case RISCV::X21_32: return 21;
    case RISCV::X22_32: return 22;
    case RISCV::X23_32: return 23;
    case RISCV::X24_32: return 24;
    case RISCV::X25_32: return 25;
    case RISCV::X26_32: return 26;
    case RISCV::X27_32: return 27;
    case RISCV::X28_32: return 28;
    case RISCV::X29_32: return 29;
    case RISCV::X30_32: return 30;
    case RISCV::X31_32: return 31;
    default: return 0x1000;
  }
}

Error Translator::translate(const llvm::MCInst &Inst)
{
  namespace RISCV = RISCVMaster;

  SBTError SE;
  First = nullptr;

#if SBT_DEBUG
  std::string SSS;
  raw_string_ostream SS(SSS);
#else
  raw_ostream &SS = nulls();
#endif
  SS << formatv("{0:X-4}:   ", CurAddr);

  switch (Inst.getOpcode())
  {
    case RISCV::ADD: {
      SS << "add\t";

      unsigned O = getRD(Inst, SS);
      Value *O1 = getReg(Inst, 1, SS);
      Value *O2 = getReg(Inst, 2, SS);

      Value *V = add(O1, O2);
      store(V, O);

      dbgprint(SS);
      break;
    }

    case RISCV::ADDI: {
      SS << "addi\t";

      unsigned O = getRD(Inst, SS);
      Value *O1 = getReg(Inst, 1, SS);
      Value *O2 = getRegOrImm(Inst, 2, SS);

      Value *V = add(O1, O2);
      store(V, O);

      dbgprint(SS);
      break;
    }

    case RISCV::AUIPC: {
      SS << "auipc\t";

      unsigned O = getRD(Inst, SS);
      Value *V = getImm(Inst, 1, SS);

      // Add PC (CurAddr) if V is not a relocation
      if (!isRelocation(V)) {
        V = add(V, ConstantInt::get(I32, CurAddr));
        // Get upper 20 bits only
        V = Builder->CreateAnd(V, ConstantInt::get(I32, 0xFFFFF000));
        updateFirst(V);
      }
      // Note: for relocations, the mask was already applied to the result

      store(V, O);

      dbgprint(SS);
      break;
    }

    case RISCV::ECALL: {
      SS << "ecall";

      if (Error E = handleSyscall())
        return E;

      dbgprint(SS);
      break;
    }

    case RISCV::JALR: {
      SS << "jalr\t";

      unsigned RD = getRD(Inst, SS);
      unsigned RS1 = getRegNum(1, Inst, SS);
      Value *Imm = getImm(Inst, 2, SS);

      // Check for 'return'
      if (RD == RV_ZERO &&
          RS1 == RV_RA &&
          isa<ConstantInt>(Imm) &&
          cast<ConstantInt>(Imm)->getValue() == 0)
      {
        Builder->CreateRet(load(RV_A0));
        break;
      }

      // Value *Target = add(RS1, Imm);
      // Target = Builder->CreateAnd(Target, ~1U);
      // updateFirst(Target);

      dbgprint(SS);
      break;
    }

    case RISCV::LW: {
      SS << "lw\t";

      unsigned O = getRD(Inst, SS);
      Value *RS1 = getReg(Inst, 1, SS);
      Value *Imm = getImm(Inst, 2, SS);

      Value *V = add(RS1, Imm);
      Value *Ptr = Builder->CreateCast(
        llvm::Instruction::CastOps::IntToPtr, V, I32Ptr);
      updateFirst(Ptr);
      V = Builder->CreateLoad(Ptr);
      updateFirst(V);

      store(V, O);

      dbgprint(SS);
      break;
    }

    case RISCV::SW: {
      SS << "sw\t";

      Value *RS1 = getReg(Inst, 0, SS);
      Value *RS2 = getReg(Inst, 1, SS);
      Value *Imm = getImm(Inst, 2, SS);

      Value *V = add(RS1, Imm);

      Value *Ptr = Builder->CreateCast(
        llvm::Instruction::CastOps::IntToPtr, V, I32Ptr);
      updateFirst(Ptr);
      V = Builder->CreateStore(RS2, Ptr);
      updateFirst(V);

      dbgprint(SS);
      break;
    }

    default: {
      SE << "Unknown instruction opcode: "
         << Inst.getOpcode() << "\n";
      return error(SE);
    }
  }

  return Error::success();
}

Error Translator::startModule()
{
  buildRegisterFile();

  if (Error E = buildShadowImage())
    return E;

  if (Error E = buildStack())
    return E;

  if (Error E = genSyscallHandler())
    return E;

  return Error::success();
}

Error Translator::finishModule()
{
  return Error::success();
}

Error Translator::startMain(StringRef Name, uint64_t Addr)
{
  // Create a function with no parameters
  FunctionType *FT =
    FunctionType::get(I32, !VAR_ARG);
  Function *F =
    Function::Create(FT, Function::ExternalLinkage, Name, Module);

  // BB
  Twine BBName = Twine("bb").concat(Twine::utohexstr(Addr));
  BasicBlock *BB = BasicBlock::Create(*Context, BBName, F);
  Builder->SetInsertPoint(BB);

  // Set stack pointer.

  std::vector<Value *> Idx = { ZERO, ConstantInt::get(I32, StackSize) };
  Value *V =
    Builder->CreateGEP(Stack, Idx);
  StackEnd = i8PtrToI32(V);

  store(StackEnd, RV_SP);

  return Error::success();
}

Error Translator::startFunction(StringRef Name, uint64_t Addr)
{
  // Create a function with no parameters
  FunctionType *FT =
    FunctionType::get(Type::getVoidTy(*Context), !VAR_ARG);
  Function *F =
    Function::Create(FT, Function::ExternalLinkage, Name, Module);

  // BB
  Twine BBName = Twine("bb").concat(Twine::utohexstr(Addr));
  BasicBlock *BB = BasicBlock::Create(*Context, BBName, F);
  Builder->SetInsertPoint(BB);

  return Error::success();
}

Error Translator::finishFunction()
{
  if (Builder->GetInsertBlock()->getTerminator() == nullptr)
    Builder->CreateRetVoid();
  return Error::success();
}

void Translator::buildRegisterFile()
{
  Constant *X0 = ConstantInt::get(I32, 0u);
  X[0] = new GlobalVariable(*Module, I32, CONSTANT,
    GlobalValue::ExternalLinkage, X0, IR_XREGNAME + "0");

  for (int I = 1; I < 32; ++I) {
    std::string S;
    raw_string_ostream SS(S);
    SS << IR_XREGNAME << I;
    X[I] = new GlobalVariable(*Module, I32, !CONSTANT,
        GlobalValue::ExternalLinkage, X0, SS.str());
  }
}

Error Translator::buildShadowImage()
{
  SBTError SE;

  // Assuming only one data section for now
  // Get Data Section
  ConstSectionPtr DS = nullptr;
  for (ConstSectionPtr Sec : CurObj->sections()) {
    if (Sec->section().isData() && !Sec->isText()) {
      DS = Sec;
      break;
    }
  }

  if (!DS) {
    SE << __FUNCTION__ << ": No Data Section found";
    return error(SE);
  }

  StringRef Bytes;
  std::error_code EC = DS->contents(Bytes);
  if (EC) {
    SE << __FUNCTION__ << ": failed to get section contents";
    return error(SE);
  }

  ArrayRef<uint8_t> ByteArray(
    reinterpret_cast<const uint8_t *>(Bytes.data()),
    Bytes.size());

  Constant *CDA = ConstantDataArray::get(*Context, ByteArray);

  ShadowImage =
    new GlobalVariable(*Module, CDA->getType(), !CONSTANT,
      GlobalValue::ExternalLinkage, CDA, "ShadowMemory");

  return Error::success();
}

Error Translator::genSyscallHandler()
{
  // Declare X86 syscall functions
  const size_t N = 5;
  FunctionType *FTX86SC[N];
  Function *FX86SC[N];
  std::vector<Type *> FArgs = { I32 };

  const std::string SCName = "syscall";
  for (size_t I = 0; I < N; I++) {
    std::string S = SCName;
    raw_string_ostream SS(S);
    SS << I;

    FTX86SC[I] = FunctionType::get(I32, FArgs, !VAR_ARG);
    FArgs.push_back(I32);

    FX86SC[I] = Function::Create(FTX86SC[I],
      Function::ExternalLinkage, SS.str(), Module);
  }

  // Build syscalls' info
  struct Syscall {
    int Args;
    int RV;
    int X86;

    Syscall(int A, int R, int X) :
      Args(A),
      RV(R),
      X86(X)
    {}
  };

  // Arg #, RISC-V Syscall #, X86 Syscall #
  const int X86_SYS_EXIT = 1;
  const std::vector<Syscall> SCV = {
    { 1, 93, 1 },  // EXIT
    { 3, 64, 4 }   // WRITE
  };

  const std::string BBPrefix = "bb_rvsc_";
  First = nullptr;

  // Define rv_syscall function
  FTRVSC = FunctionType::get(I32, { I32 }, !VAR_ARG);
  FRVSC =
    Function::Create(FTRVSC, Function::ExternalLinkage, "rv_syscall", Module);

  // Entry
  BasicBlock *BBEntry = BasicBlock::Create(*Context, BBPrefix + "entry", FRVSC);
  llvm::Argument &SC = *FRVSC->arg_begin();

  // Exit
  //
  // Return A0
  BasicBlock *BBExit = BasicBlock::Create(*Context,
    BBPrefix + "exit", FRVSC);
  Builder->SetInsertPoint(BBExit);
  Builder->CreateRet(load(RV_A0));

  // 2nd switch
  BasicBlock *BBSW2 = BasicBlock::Create(*Context,
    BBPrefix + "sw2", FRVSC, BBExit);

  // First Switch:
  // - switch based on RISC-V syscall number and:
  //   - set number of args
  //   - set X86 syscall number

  // Default case: call exit(99)
  BasicBlock *BBSW1Dfl = BasicBlock::Create(*Context,
    BBPrefix + "sw1_default", FRVSC, BBSW2);
  Builder->SetInsertPoint(BBSW1Dfl);
  store(ConstantInt::get(I32, 1), RV_T0);
  store(ConstantInt::get(I32, X86_SYS_EXIT), RV_A7);
  store(ConstantInt::get(I32, 99), RV_A0);
  Builder->CreateBr(BBSW2);

  Builder->SetInsertPoint(BBEntry);
  SwitchInst *SW1 = Builder->CreateSwitch(&SC, BBSW1Dfl);

  // Other cases
  auto addSW1Case = [&](const Syscall &S) {
    std::string SSS = BBPrefix;
    raw_string_ostream SS(SSS);
    SS << "sw1_case_" << S.RV;

    BasicBlock *BB = BasicBlock::Create(*Context, SS.str(), FRVSC, BBSW2);
    Builder->SetInsertPoint(BB);
    store(ConstantInt::get(I32, S.Args), RV_T0);
    store(ConstantInt::get(I32, S.X86), RV_A7);
    Builder->CreateBr(BBSW2);
    SW1->addCase(ConstantInt::get(I32, S.RV), BB);
  };

  for (const Syscall &S : SCV)
    addSW1Case(S);

  // Second Switch:
  // - switch based on syscall's number of args
  //   and make the call

  auto getSW2CaseBB = [&](size_t Val) {
    std::string SSS = BBPrefix;
    raw_string_ostream SS(SSS);
    SS << "sw2_case_" << Val;

    BasicBlock *BB = BasicBlock::Create(*Context, SS.str(), FRVSC, BBExit);
    Builder->SetInsertPoint(BB);

    // Set args
    std::vector<Value *> Args = { load(RV_A7) };
    for (size_t I = 0; I < Val; I++)
      Args.push_back(load(RV_A0 + I));

    // Make the syscall
    Value *V = Builder->CreateCall(FX86SC[Val], Args);
    store(V, RV_A0);
    Builder->CreateBr(BBExit);
    return BB;
  };

  BasicBlock *SW2Case0 = getSW2CaseBB(0);

  Builder->SetInsertPoint(BBSW2);
  SwitchInst *SW2 = Builder->CreateSwitch(load(RV_T0), SW2Case0);
  SW2->addCase(ConstantInt::get(I32, 0), SW2Case0);
  for (size_t I = 1; I < N; I++)
    SW2->addCase(ConstantInt::get(I32, I), getSW2CaseBB(I));

  return Error::success();
}

Error Translator::handleSyscall()
{
  Value *SC = load(RV_A7);
  std::vector<Value *> Args = { SC };
  Value *V = Builder->CreateCall(FRVSC, Args);
  updateFirst(V);
  store(V, RV_A0);

  return Error::success();
}

Error Translator::buildStack()
{
  std::string Bytes(StackSize, 'S');

  ArrayRef<uint8_t> ByteArray(
    reinterpret_cast<const uint8_t *>(Bytes.data()),
    StackSize);

  Constant *CDA = ConstantDataArray::get(*Context, ByteArray);

  Stack =
    new GlobalVariable(*Module, CDA->getType(), !CONSTANT,
      GlobalValue::ExternalLinkage, CDA, "Stack");

  return Error::success();
}

#if SBT_DEBUG
static std::string getMDName(const llvm::StringRef &S)
{
  std::string SSS;
  raw_string_ostream SS(SSS);
  SS << '_';
  for (char C : S) {
    switch (C) {
      case ' ':
      case ':':
      case ',':
      case '%':
      case '(':
      case ')':
      case '\t':
        SS << '_';
        break;

      case '=':
        SS << "eq";
        break;

      default:
        SS << C;
    }
  }
  return SS.str();
}

void Translator::dbgprint(llvm::raw_string_ostream &SS)
{
  DBGS << SS.str() << "\n";
  MDNode *N = MDNode::get(*Context,
    MDString::get(*Context, "RISC-V Instruction"));
  First->setMetadata(getMDName(SS.str()), N);
}
#endif

} // sbt
