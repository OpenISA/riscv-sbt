#include "Translator.h"

#include "SBTError.h"
#include "Utils.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ValueSymbolTable.h>
#include <llvm/IR/Type.h>
#include <llvm/MC/MCDisassembler/MCDisassembler.h>
#include <llvm/MC/MCInst.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/MemoryBuffer.h>

// LLVM internal instruction info
#define GET_INSTRINFO_ENUM
#include <llvm/Target/RISCVMaster/RISCVMasterGenInstrInfo.inc>

#include <algorithm>
#include <vector>

#undef NDEBUG
#include <cassert>

using namespace llvm;

namespace sbt {

Translator::Translator(
  LLVMContext* ctx,
  IRBuilder<>* bldr,
  llvm::Module* mod)
  :
  _ctx(ctx),
  _builder(bldr),
  _module(mod)
{
  initLLVMConstants(*_ctx);
}


llvm::Error Translator::translateSection(ConstSectionPtr Sec)
{
  assert(!inFunc());

  uint64_t SectionAddr = Sec->address();
  uint64_t SectionSize = Sec->size();

  // relocatable object?
  uint64_t ELFOffset = SectionAddr;
  if (SectionAddr == 0)
    ELFOffset = Sec->getELFOffset();

  // skip non code sections
  if (!Sec->isText())
    return Error::success();

  // print section info
  DBGS << formatv("Section {0}: Addr={1:X+4}, ELFOffs={2:X+4}, Size={3:X+4}\n",
      Sec->name(), SectionAddr, ELFOffset, SectionSize);

  State = ST_DFL;
  setCurSection(Sec);

  // get relocations
  const ConstRelocationPtrVec Relocs = CurObj->relocs();
  auto RI = Relocs.cbegin();
  auto RE = Relocs.cend();
  setRelocIters(RI, RE);

  // get section bytes
  StringRef BytesStr;
  if (Sec->contents(BytesStr)) {
    SBTError SE;
    SE << "Failed to get Section Contents";
    return error(SE);
  }
  CurSectionBytes = llvm::ArrayRef<uint8_t>(
    reinterpret_cast<const uint8_t *>(BytesStr.data()), BytesStr.size());

  // for each symbol
  const ConstSymbolPtrVec &Symbols = Sec->symbols();
  size_t SN = Symbols.size();
  for (size_t SI = 0; SI != SN; ++SI)
    if (auto E = translateSymbol(SI))
      return E;

  return Error::success();
}


llvm::Error Translator::translateSymbol(size_t SI)
{
  const ConstSymbolPtrVec &Symbols = CurSection->symbols();
  size_t SN = Symbols.size();
  uint64_t SectionSize = CurSection->size();
  ConstSymbolPtr Sym = Symbols[SI];

  uint64_t Start = Sym->address();
  volatile uint64_t End;  // XXX gcc bug: need to make it volatile
  if (SI == SN - 1)
    End = SectionSize;
  else
    End = Symbols[SI + 1]->address();

  // XXX for now, translate only instructions that appear after a
  // function or global symbol
  const StringRef &SymbolName = Sym->name();
  if (Sym->type() == object::SymbolRef::ST_Function ||
      Sym->flags() & object::SymbolRef::SF_Global)
  {
    if (SymbolName == "main") {
      if (auto E = startMain(SymbolName, Start))
        return E;
    } else {
      if (auto E = startFunction(SymbolName, Start))
        return E;
    }
  }

  // skip section bytes until a function like symbol is found
  if (!inFunc())
    return Error::success();

  DBGS << SymbolName << ":\n";
  return translateInstrs(Start, End);
}


llvm::Error Translator::translateInstrs(uint64_t Begin, uint64_t End)
{
  DBGS << __FUNCTION__ << formatv("({0:X+4}, {1:X+4})\n", Begin, End);

  uint64_t SectionAddr = CurSection->address();

  // for each instruction
  uint64_t Size;
  for (uint64_t Addr = Begin; Addr < End; Addr += Size) {
    setCurAddr(Addr);

    // disasm
    const uint8_t* RawBytes = &CurSectionBytes[Addr];
    uint32_t RawInst = *reinterpret_cast<const uint32_t*>(RawBytes);

    // consider 0 bytes as end-of-section padding
    if (State == ST_PADDING) {
      if (RawInst != 0) {
        SBTError SE;
        SE << "Found non-zero byte in zero-padding area";
        return error(SE);
      }
      continue;
    } else if (RawInst == 0) {
      State = ST_PADDING;
      continue;
    }

    MCInst Inst;
    MCDisassembler::DecodeStatus st =
      DisAsm->getInstruction(Inst, Size,
        CurSectionBytes.slice(Addr),
        SectionAddr + Addr, DBGS, nulls());
    if (st == MCDisassembler::DecodeStatus::Success) {
#if SBT_DEBUG
      DBGS << llvm::formatv("{0:X-4}: ", Addr);
      InstPrinter->printInst(&Inst, DBGS, "", *STI);
      DBGS << "\n";
#endif
      // translate
      if (auto E = translate(Inst))
        return E;
    // failed to disasm
    } else {
      SBTError SE;
      SE << "Invalid instruction encoding at address ";
      SE << formatv("{0:X-4}", Addr);
      SE << formatv(": {0:X-8}", RawInst);
      return error(SE);
    }
  }

  return Error::success();
}


llvm::Error Translator::translate(const llvm::MCInst &Inst)
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

  if (NextBB && CurAddr == NextBB) {
    BasicBlock **BB = BBMap[CurAddr];
    assert(BB && "BasicBlock not found!");
    if (!BrWasLast)
      Builder->CreateBr(*BB);
    Builder->SetInsertPoint(*BB);

    auto Iter = BBMap.lower_bound(CurAddr + 4);
    if (Iter != BBMap.end())
      updateNextBB(Iter->key);
  }

  Error E = noError();
  BrWasLast = false;

  switch (Inst.getOpcode())
  {
    // ALU Ops
    case RISCV::ADD:
      E = translateALUOp(Inst, ADD, AF_NONE, SS);
      break;
    case RISCV::ADDI:
      E = translateALUOp(Inst, ADD, AF_IMM, SS);
      break;
    case RISCV::AND:
      E = translateALUOp(Inst, AND, AF_NONE, SS);
      break;
    case RISCV::ANDI:
      E = translateALUOp(Inst, AND, AF_IMM, SS);
      break;
    case RISCV::MUL:
      E = translateALUOp(Inst, MUL, AF_NONE, SS);
      break;
    case RISCV::OR:
      E = translateALUOp(Inst, OR, AF_NONE, SS);
      break;
    case RISCV::ORI:
      E = translateALUOp(Inst, OR, AF_IMM, SS);
      break;
    case RISCV::SLL:
      E = translateALUOp(Inst, SLL, AF_NONE, SS);
      break;
    case RISCV::SLLI:
      E = translateALUOp(Inst, SLL, AF_IMM, SS);
      break;
    case RISCV::SLT:
      E = translateALUOp(Inst, SLT, AF_NONE, SS);
      break;
    case RISCV::SLTU:
      E = translateALUOp(Inst, SLT, AF_UNSIGNED, SS);
      break;
    case RISCV::SLTI:
      E = translateALUOp(Inst, SLT, AF_IMM, SS);
      break;
    case RISCV::SLTIU:
      E = translateALUOp(Inst, SLT, AF_IMM | AF_UNSIGNED, SS);
      break;
    case RISCV::SRA:
      E = translateALUOp(Inst, SRA, AF_NONE, SS);
      break;
    case RISCV::SRAI:
      E = translateALUOp(Inst, SRA, AF_IMM, SS);
      break;
    case RISCV::SRL:
      E = translateALUOp(Inst, SRL, AF_NONE, SS);
      break;
    case RISCV::SRLI:
      E = translateALUOp(Inst, SRL, AF_IMM, SS);
      break;
    case RISCV::SUB:
      E = translateALUOp(Inst, SUB, AF_NONE, SS);
      break;
    case RISCV::XOR:
      E = translateALUOp(Inst, XOR, AF_NONE, SS);
      break;
    case RISCV::XORI:
      E = translateALUOp(Inst, XOR, AF_IMM, SS);
      break;

    // UI
    case RISCV::AUIPC:
      E = translateUI(Inst, AUIPC, SS);
      break;
    case RISCV::LUI:
      E = translateUI(Inst, LUI, SS);
      break;

    // Branch
    case RISCV::BEQ:
      E = translateBranch(Inst, BEQ, SS);
      break;
    case RISCV::BNE:
      E = translateBranch(Inst, BNE, SS);
      break;
    case RISCV::BGE:
      E = translateBranch(Inst, BGE, SS);
      break;
    case RISCV::BGEU:
      E = translateBranch(Inst, BGEU, SS);
      break;
    case RISCV::BLT:
      E = translateBranch(Inst, BLT, SS);
      break;
    case RISCV::BLTU:
      E = translateBranch(Inst, BLTU, SS);
      break;
    // Jump
    case RISCV::JAL:
      E = translateBranch(Inst, JAL, SS);
      break;
    case RISCV::JALR:
      E = translateBranch(Inst, JALR, SS);
      break;

    // ecall
    case RISCV::ECALL:
      SS << "ecall";
      E = handleSyscall();
      break;

    // ebreak
    case RISCV::EBREAK:
      SS << "ebreak";
      nop();
      break;

    // Load
    case RISCV::LB:
      E = translateLoad(Inst, S8, SS);
      break;
    case RISCV::LBU:
      E = translateLoad(Inst, U8, SS);
      break;
    case RISCV::LH:
      E = translateLoad(Inst, S16, SS);
      break;
    case RISCV::LHU:
      E = translateLoad(Inst, U16, SS);
      break;
    case RISCV::LW:
      E = translateLoad(Inst, U32, SS);
      break;

    // Store
    case RISCV::SB:
      E = translateStore(Inst, U8, SS);
      break;
    case RISCV::SH:
      E = translateStore(Inst, U16, SS);
      break;
    case RISCV::SW:
      E = translateStore(Inst, U32, SS);
      break;

    // fence
    case RISCV::FENCE:
      E = translateFence(Inst, false, SS);
      break;
    case RISCV::FENCEI:
      E = translateFence(Inst, true, SS);
      break;

    // system
    case RISCV::CSRRW:
      E = translateCSR(Inst, RW, false, SS);
      break;
    case RISCV::CSRRWI:
      E = translateCSR(Inst, RW, true, SS);
      break;
    case RISCV::CSRRS:
      E = translateCSR(Inst, RS, false, SS);
      break;
    case RISCV::CSRRSI:
      E = translateCSR(Inst, RS, true, SS);
      break;
    case RISCV::CSRRC:
      E = translateCSR(Inst, RC, false, SS);
      break;
    case RISCV::CSRRCI:
      E = translateCSR(Inst, RC, true, SS);
      break;

    default:
      SE << "Unknown instruction opcode: " << Inst.getOpcode();
      return error(SE);
  }

  if (E)
    return E;

  if (!First)
    First = load(RV_A0);

  dbgprint(SS);
  InstrMap(CurAddr, std::move(First));
  return Error::success();
}


Error Translator::startModule()
{
  if (auto E = declRegisterFile())
    return E;

  if (auto E = buildShadowImage())
    return E;

  if (auto E = buildStack())
    return E;

  declSyscallHandler();

  auto ExpF = createFunction("rv32_icaller");
  if (!ExpF)
    return ExpF.takeError();
  ICaller = ExpF.get();

  FunctionType *FT = FunctionType::get(I32, !VAR_ARG);
  GetCycles = Function::Create(FT,
    Function::ExternalLinkage, "get_cycles", Module);

  GetTime = Function::Create(FT,
    Function::ExternalLinkage, "get_time", Module);

  InstRet = Function::Create(FT,
    Function::ExternalLinkage, "get_instret", Module);

  return Error::success();
}

Error Translator::genSCHandler()
{
  if (auto E = buildRegisterFile())
    return E;

  if (Error E = genSyscallHandler())
    return E;

  return Error::success();
}

llvm::Error Translator::genICaller()
{
  PointerType *Ty = VoidFun->getPointerTo();
  Value *Target = nullptr;

  // basic blocks
  BasicBlock *BBPrev = Builder->GetInsertBlock();
  BasicBlock *BBBeg = BasicBlock::Create(*Context, "begin", ICaller);
  BasicBlock *BBDfl = BasicBlock::Create(*Context, "default", ICaller);
  BasicBlock *BBEnd = BasicBlock::Create(*Context, "end", ICaller);

  // default:
  // t1 = nullptr;
  Builder->SetInsertPoint(BBDfl);
  store(ZERO, RV_T1);
  Builder->CreateBr(BBEnd);

  // end: call t1
  Builder->SetInsertPoint(BBEnd);
  Target = load(RV_T1);
  Target = Builder->CreateIntToPtr(Target, Ty);
  Builder->CreateCall(Target);
  Builder->CreateRetVoid();

  // switch
  Builder->SetInsertPoint(BBBeg);
  Target = load(RV_T1);
  SwitchInst *SW = Builder->CreateSwitch(Target, BBDfl, FunTable.size());

  // cases
  // case fun: t1 = realFunAddress;
  for (Function *F : FunTable) {
    uint64_t *Addr = FunMap[F];
    assert(Addr && "Indirect called function not found!");
    std::string CaseStr = "case_" + F->getName().str();
    Value *Sym = Module->getValueSymbolTable().lookup(F->getName());

    BasicBlock *Dest = BasicBlock::Create(*Context, CaseStr, ICaller, BBDfl);
    Builder->SetInsertPoint(Dest);
    Sym = Builder->CreatePtrToInt(Sym, I32);
    store(Sym, RV_T1);
    Builder->CreateBr(BBEnd);

    Builder->SetInsertPoint(BBBeg);
    SW->addCase(ConstantInt::get(I32, *Addr), Dest);
  }

  Builder->SetInsertPoint(BBPrev);
  return Error::success();
}

Error Translator::finishModule()
{
  return genICaller();
}

Error Translator::startMain(StringRef Name, uint64_t Addr)
{
  if (auto E = finishFunction())
    return E;

  // Create a function with no parameters
  FunctionType *FT =
    FunctionType::get(I32, !VAR_ARG);
  Function *F =
    Function::Create(FT, Function::ExternalLinkage, Name, Module);
  CurFunc = F;

  // BB
  BasicBlock *BB = SBTBasicBlock::create(*Context, Addr, F);
  BBMap(Addr, std::move(BB));
  Builder->SetInsertPoint(BB);

  // Set stack pointer.

  std::vector<Value *> Idx = { ZERO, ConstantInt::get(I32, StackSize) };
  Value *V =
    Builder->CreateGEP(Stack, Idx);
  StackEnd = i8PtrToI32(V);

  store(StackEnd, RV_SP);

  // if (auto E = startup())
  //  return E;

  // init syscall module
  F = Function::Create(VoidFun,
    Function::ExternalLinkage, "syscall_init", Module);
  Builder->CreateCall(F);

  InMain = true;

  return Error::success();
}


llvm::Expected<llvm::Function *>
Translator::createFunction(llvm::StringRef Name)
{
  Function *F = Module->getFunction(Name);
  if (F)
    return F;

  // Create a function with no parameters
  FunctionType *FT =
    FunctionType::get(Type::getVoidTy(*Context), !VAR_ARG);
  F = Function::Create(FT, Function::ExternalLinkage, Name, Module);

  return F;
}


Error Translator::startFunction(StringRef Name, uint64_t Addr)
{
  if (auto E = finishFunction())
    return E;

  auto ExpF = createFunction(Name);
  if (!ExpF)
    return ExpF.takeError();
  Function *F = ExpF.get();
  CurFunc = F;
  FunTable.push_back(F);
  FunMap(F, std::move(Addr));

  // BB
  BasicBlock **BBPtr = BBMap[Addr];
  BasicBlock *BB;
  if (!BBPtr) {
    BB = SBTBasicBlock::create(*Context, Addr, F);
    BBMap(Addr, std::move(BB));
  } else {
    BB = *BBPtr;
    BB->removeFromParent();
    BB->insertInto(F);
  }
  Builder->SetInsertPoint(BB);

  return Error::success();
}

Error Translator::finishFunction()
{
  if (!CurFunc)
    return Error::success();
  CurFunc = nullptr;

  if (Builder->GetInsertBlock()->getTerminator() == nullptr)
    Builder->CreateRetVoid();
  InMain = false;
  return Error::success();
}

Error Translator::declOrBuildRegisterFile(bool decl)
{
  Constant *X0 = ConstantInt::get(I32, 0u);
  X[0] = new GlobalVariable(*Module, I32, CONSTANT,
    GlobalValue::ExternalLinkage, decl? nullptr : X0, IR_XREGNAME + "0");

  for (int I = 1; I < 32; ++I) {
    std::string S;
    raw_string_ostream SS(S);
    SS << IR_XREGNAME << I;
    X[I] = new GlobalVariable(*Module, I32, !CONSTANT,
        GlobalValue::ExternalLinkage, decl? nullptr : X0, SS.str());
  }

  return Error::success();
}

Error Translator::buildShadowImage()
{
  SBTError SE;

  std::vector<uint8_t> Vec;
  for (ConstSectionPtr Sec : CurObj->sections()) {
    // Skip non text/data sections
    if (!Sec->isText() && !Sec->isData() && !Sec->isBSS() && !Sec->isCommon())
      continue;

    StringRef Bytes;
    std::string Z;
    // .bss/.common
    if (Sec->isBSS() || Sec->isCommon()) {
      Z = std::string(Sec->size(), 0);
      Bytes = Z;
    // others
    } else {
      // Read contents
      if (Sec->contents(Bytes)) {
        SE  << __FUNCTION__ << ": failed to get section ["
            << Sec->name() << "] contents";
        return error(SE);
      }
    }

    // Align all sections
    while (Vec.size() % 4 != 0)
      Vec.push_back(0);

    // Set Shadow Offset of Section
    Sec->shadowOffs(Vec.size());

    // Append to vector
    for (size_t I = 0; I < Bytes.size(); I++)
      Vec.push_back(Bytes[I]);
  }

  // Create the ShadowImage
  Constant *CDA = ConstantDataArray::get(*Context, Vec);
  ShadowImage =
    new GlobalVariable(*Module, CDA->getType(), !CONSTANT,
      GlobalValue::ExternalLinkage, CDA, "ShadowMemory");

  return Error::success();
}

void Translator::declSyscallHandler()
{
  FTRVSC = FunctionType::get(I32, { I32 }, !VAR_ARG);
  FRVSC =
    Function::Create(FTRVSC, Function::ExternalLinkage, "rv_syscall", Module);
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

  declSyscallHandler();

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


llvm::Expected<llvm::Value *>
Translator::handleRelocation(llvm::raw_ostream &SS)
{
  LastImm.IsSym = false;

  // No more relocations exist
  if (RI == RE)
    return nullptr;

  // Check if there is a relocation for current address
  auto CurReloc = *RI;
  if (CurReloc->offset() != CurAddr)
    return nullptr;

  ConstSymbolPtr Sym = CurReloc->symbol();
  uint64_t Type = CurReloc->type();
  bool IsLO = false;
  uint64_t Mask;
  ConstSymbolPtr RealSym = Sym;

  switch (Type) {
    case llvm::ELF::R_RISCV_PCREL_HI20:
    case llvm::ELF::R_RISCV_HI20:
      break;

    // This rellocation has PC info only
    // Symbol info is present on the PCREL_HI20 Reloc
    case llvm::ELF::R_RISCV_PCREL_LO12_I:
      IsLO = true;
      RealSym = (**RLast).symbol();
      break;

    case llvm::ELF::R_RISCV_LO12_I:
      IsLO = true;
      break;

    default:
      DBGS << "Relocation Type: " << Type << '\n';
      llvm_unreachable("unknown relocation");
  }

  // Set Symbol Relocation info
  SymbolReloc SR;
  SR.IsValid = true;
  SR.Name = RealSym->name();
  SR.Addr = RealSym->address();
  SR.Val = SR.Addr;
  SR.Sec = RealSym->section();

  // Note: !SR.Sec && SR.Addr == External Symbol
  assert((SR.Sec || !SR.Addr) && "No section found for relocation");
  if (SR.Sec) {
    assert(SR.Addr < SR.Sec->size() && "Out of bounds relocation");
    SR.Val += SR.Sec->shadowOffs();
  }

  // Increment relocation iterator
  RLast = RI;
  do {
    ++RI;
  } while (RI != RE && (**RI).offset() == CurAddr);

  // Write relocation string
  if (IsLO) {
    Mask = 0xFFF;
    SS << "%lo(";
  } else {
    Mask = 0xFFFFF000;
    SS << "%hi(";
  }
  SS << SR.Name << ") = " << SR.Addr;

  Value *V = nullptr;
  bool IsFunction =
    SR.Sec && SR.Sec->isText() &&
    RealSym->type() == llvm::object::SymbolRef::ST_Function;

  // special case: external functions: return our handler instead
  if (SR.isExternal()) {
    auto ExpF = import(SR.Name);
    if (!ExpF)
      return ExpF.takeError();
    uint64_t Addr = ExpF.get();
    Addr &= Mask;
    V = ConstantInt::get(I32, Addr);

  // special case: internal function
  } else if (IsFunction) {
    uint64_t Addr = SR.Addr;
    Addr &= Mask;
    V = ConstantInt::get(I32, Addr);

  // add the relocation offset to ShadowImage to get the final address
  } else if (SR.Sec) {
    // get char * to memory
    std::vector<llvm::Value *> Idx = { ZERO,
      llvm::ConstantInt::get(I32, SR.Val) };
    V = Builder->CreateGEP(ShadowImage, Idx);
    updateFirst(V);

    V = i8PtrToI32(V);

    // Finally, get only the upper or lower part of the result
    V = Builder->CreateAnd(V, llvm::ConstantInt::get(I32, Mask));
    updateFirst(V);

  } else
    llvm_unreachable("Failed to resolve relocation");

  LastImm.IsSym = true;
  LastImm.SymRel = std::move(SR);
  return V;
}


llvm::Error Translator::translateALUOp(
  const llvm::MCInst &Inst,
  ALUOp Op,
  uint32_t Flags,
  llvm::raw_string_ostream &SS)
{
  bool HasImm = Flags & AF_IMM;
  bool IsUnsigned = Flags & AF_UNSIGNED;

  switch (Op) {
    case ADD: SS << "add";  break;
    case AND: SS << "and";  break;
    case MUL: SS << "mul";  break;
    case OR:  SS << "or";   break;
    case SLL: SS << "sll";  break;
    case SLT: SS << "slt";  break;
    case SRA: SS << "sra";  break;
    case SRL: SS << "srl";  break;
    case SUB: SS << "sub";  break;
    case XOR: SS << "xor";  break;
  }
  if (HasImm)
    SS << "i";
  if (IsUnsigned)
    SS << "u";
  SS << '\t';

  unsigned O = getRD(Inst, SS);
  Value *O1 = getReg(Inst, 1, SS);
  Value *O2;
  if (HasImm) {
    auto ExpImm = getImm(Inst, 2, SS);
    if (!ExpImm)
      return ExpImm.takeError();
    O2 = ExpImm.get();
  } else
    O2 = getReg(Inst, 2, SS);

  Value *V;

  switch (Op) {
    case ADD:
      V = add(O1, O2);
      break;

    case AND:
      V = Builder->CreateAnd(O1, O2);
      break;

    case MUL:
      V = Builder->CreateMul(O1, O2);
      break;

    case OR:
      V = Builder->CreateOr(O1, O2);
      break;

    case SLL:
      V = Builder->CreateShl(O1, O2);
      break;

    case SLT:
      if (IsUnsigned)
        V = Builder->CreateICmpULT(O1, O2);
      else
        V = Builder->CreateICmpSLT(O1, O2);
      updateFirst(V);
      V = Builder->CreateZExt(V, I32);
      break;

    case SRA:
      V = Builder->CreateAShr(O1, O2);
      break;

    case SRL:
      V = Builder->CreateLShr(O1, O2);
      break;

    case SUB:
      V = Builder->CreateSub(O1, O2);
      break;

    case XOR:
      V = Builder->CreateXor(O1, O2);
      break;
  }
  updateFirst(V);

  store(V, O);

  return Error::success();
}


Error Translator::translateLoad(
  const llvm::MCInst &Inst,
  IntType IT,
  llvm::raw_string_ostream &SS)
{
  switch (IT) {
    case S8:
      SS << "lb";
      break;

    case U8:
      SS << "lbu";
      break;

    case S16:
      SS << "lh";
      break;

    case U16:
      SS << "lhu";
      break;

    case U32:
      SS << "lw";
      break;
  }
  SS << '\t';

  unsigned O = getRD(Inst, SS);
  Value *RS1 = getReg(Inst, 1, SS);
  auto ExpImm = getImm(Inst, 2, SS);
  if (!ExpImm)
    return ExpImm.takeError();
  Value *Imm = ExpImm.get();

  Value *V = add(RS1, Imm);

  Value *Ptr;
  switch (IT) {
    case S8:
    case U8:
      Ptr = Builder->CreateCast(
        llvm::Instruction::CastOps::IntToPtr, V, I8Ptr);
      break;

    case S16:
    case U16:
      Ptr = Builder->CreateCast(
        llvm::Instruction::CastOps::IntToPtr, V, I16Ptr);
      break;

    case U32:
      Ptr = Builder->CreateCast(
        llvm::Instruction::CastOps::IntToPtr, V, I32Ptr);
      break;
  }
  updateFirst(Ptr);

  V = Builder->CreateLoad(Ptr);
  updateFirst(V);

  // to int32
  switch (IT) {
    case S8:
    case S16:
      V = Builder->CreateSExt(V, I32);
      updateFirst(V);
      break;

    case U8:
    case U16:
      V = Builder->CreateZExt(V, I32);
      updateFirst(V);
      break;

    case U32:
      break;
  }
  store(V, O);

  return Error::success();
}


Error Translator::translateStore(
  const llvm::MCInst &Inst,
  IntType IT,
  llvm::raw_string_ostream &SS)
{
  switch (IT) {
    case U8:
      SS << "sb";
      break;

    case U16:
      SS << "sh";
      break;

    case U32:
      SS << "sw";
      break;

    default:
      llvm_unreachable("Unknown store type!");
  }
  SS << '\t';


  Value *RS1 = getReg(Inst, 0, SS);
  Value *RS2 = getReg(Inst, 1, SS);
  auto ExpImm = getImm(Inst, 2, SS);
  if (!ExpImm)
    return ExpImm.takeError();
  Value *Imm = ExpImm.get();

  Value *V = add(RS1, Imm);

  Value *Ptr;
  switch (IT) {
    case U8:
      Ptr = Builder->CreateCast(
        llvm::Instruction::CastOps::IntToPtr, V, I8Ptr);
      updateFirst(Ptr);
      V = Builder->CreateTruncOrBitCast(RS2, I8);
      updateFirst(V);
      break;

    case U16:
      Ptr = Builder->CreateCast(
        llvm::Instruction::CastOps::IntToPtr, V, I16Ptr);
      updateFirst(Ptr);
      V = Builder->CreateTruncOrBitCast(RS2, I16);
      updateFirst(V);
      break;

    case U32:
      Ptr = Builder->CreateCast(
        llvm::Instruction::CastOps::IntToPtr, V, I32Ptr);
      updateFirst(Ptr);
      V = RS2;
      break;

    default:
      llvm_unreachable("Unknown store type!");
  }

  V = Builder->CreateStore(V, Ptr);
  updateFirst(V);

  return Error::success();
}


llvm::Error Translator::translateBranch(
  const llvm::MCInst &Inst,
  BranchType BT,
  llvm::raw_string_ostream &SS)
{
  // Inst print
  switch (BT) {
    case JAL:   SS << "jal";  break;
    case JALR:  SS << "jalr"; break;
    case BEQ:   SS << "beq";  break;
    case BNE:   SS << "bne";  break;
    case BGE:   SS << "bge";  break;
    case BGEU:  SS << "bgeu";  break;
    case BLT:   SS << "blt";  break;
    case BLTU:  SS << "bltu"; break;
  }
  SS << '\t';

  Error E = noError();

  // Get Operands
  unsigned O0N = getRegNum(0, Inst, SS);
  Value *O0 = nullptr;
  unsigned O1N = 0;
  Value *O1 = nullptr;
  Value *O2 = nullptr;
  Value *JReg = nullptr;
  Value *JImm = nullptr;
  unsigned LinkReg = 0;
  switch (BT) {
    case JAL:
      LinkReg = O0N;
      if (!exp(getImm(Inst, 1, SS), O1, E))
        return E;
      JImm = O1;
      break;

    case JALR:
      LinkReg = O0N;
      O1N = getRegNum(1, Inst, nulls());
      O1 = getReg(Inst, 1, SS);
      if (!exp(getImm(Inst, 2, SS), O2, E))
        return E;
      JReg = O1;
      JImm = O2;
      break;

    case BEQ:
    case BNE:
    case BGE:
    case BGEU:
    case BLT:
    case BLTU:
      O0 = getReg(Inst, 0, SS);
      O1 = getReg(Inst, 1, SS);
      if (!exp(getImm(Inst, 2, SS), O2, E))
        return E;
      JImm = O2;
      break;
  }

  int64_t JImmI = 0;
  bool JImmIsZeroImm = false;
  if (isa<ConstantInt>(JImm)) {
    JImmI = cast<ConstantInt>(JImm)->getValue().getSExtValue();
    JImmIsZeroImm = JImmI == 0 && !LastImm.IsSym;
  }
  Value *V = nullptr;

  // Return?
  if (BT == JALR &&
      O0N == RV_ZERO &&
      O1N == RV_RA &&
      JImmIsZeroImm)
  {
    if (InMain)
      V = Builder->CreateRet(load(RV_A0));
    else
      V = Builder->CreateRetVoid();
    updateFirst(V);
    return Error::success();
  }

  // Get target
  uint64_t Target = 0;
  bool IsSymbol = LastImm.IsSym;
  SymbolReloc SR;

  enum Action {
    JUMP_TO_SYMBOL,
    JUMP_TO_OFFS,
    IJUMP,
    CALL_SYMBOL,
    CALL_OFFS,
    CALL_EXT,
    ICALL
  };
  Action Act;

  bool IsCall = (BT == JAL || BT == JALR) &&
    LinkReg == RV_RA;
  BrWasLast = !IsCall;

  // JALR
  //
  // TODO Set Target LSB to zero
  if (BT == JALR) {
    // No base reg
    if (O1N == RV_ZERO) {
      llvm_unreachable("Unexpected JALR with base register equal to zero!");

    // No immediate
    } else if (JImmIsZeroImm) {
      V = JReg;
      if (IsCall)
        Act = ICALL;
      else
        Act = IJUMP;

    // Base + Offset
    } else {
      V = Builder->CreateAdd(JReg, JImm);
      updateFirst(V);
      if (IsCall)
        Act = ICALL;
      else
        Act = IJUMP;
    }

  // JAL to Symbol
  } else if (BT == JAL && IsSymbol) {
    SR = LastImm.SymRel;
    if (IsCall)
      Act = CALL_SYMBOL;
    else
      Act = JUMP_TO_SYMBOL;

  // JAL/Branches to PC offsets
  //
  // Add PC
  } else {
    Target = JImmI + CurAddr;
    if (IsCall)
      Act = CALL_OFFS;
    else
      Act = JUMP_TO_OFFS;
  }

  if (Act == CALL_SYMBOL || Act == JUMP_TO_SYMBOL) {
    if (SR.isExternal()) {
      V = JImm;
      assert(IsCall && "Jump to external module!");
      Act = CALL_EXT;
    } else {
      assert(SR.Sec && SR.Sec->isText() && "Jump to non Text Section!");
      Target = SR.Addr;
      if (IsCall)
        Act = CALL_OFFS;
      else
        Act = JUMP_TO_OFFS;
    }
  }

  // Evaluate condition
  Value *Cond = nullptr;
  switch (BT) {
    case BEQ:
      Cond = Builder->CreateICmpEQ(O0, O1);
      break;

    case BNE:
      Cond = Builder->CreateICmpNE(O0, O1);
      break;

    case BGE:
      Cond = Builder->CreateICmpSGE(O0, O1);
      break;

    case BGEU:
      Cond = Builder->CreateICmpUGE(O0, O1);
      break;

    case BLT:
      Cond = Builder->CreateICmpSLT(O0, O1);
      break;

    case BLTU:
      Cond = Builder->CreateICmpULT(O0, O1);
      break;

    case JAL:
    case JALR:
      break;
  }
  if (Cond)
    updateFirst(Cond);

  switch (Act) {
    case JUMP_TO_SYMBOL:
      llvm_unreachable("JUMP_TO_SYMBOL should become JUMP_TO_OFFS");
    case CALL_SYMBOL:
      llvm_unreachable("CALL_SYMBOL should become CALL_OFFS");

    case CALL_OFFS:
      if (auto E = handleCall(Target))
        return E;
      break;

    case JUMP_TO_OFFS:
      if (auto E = handleJumpToOffs(Target, Cond, LinkReg))
        return E;
      break;

    case ICALL:
      if (auto E = handleICall(V))
        return E;
      break;

    case CALL_EXT:
      if (auto E = handleCallExt(V))
        return E;
      break;

    case IJUMP:
      if (auto E = handleIJump(V, LinkReg))
        return E;
      break;
  }

  return Error::success();
}


llvm::Error Translator::handleCall(uint64_t Target)
{
  // Find function
  // Get symbol by offset
  ConstSectionPtr Sec = CurObj->lookupSection(".text");
  assert(Sec && ".text section not found!");
  ConstSymbolPtr Sym = Sec->lookup(Target);
  assert(Sym &&
      (Sym->type() == llvm::object::SymbolRef::ST_Function ||
        (Sym->flags() & llvm::object::SymbolRef::SF_Global)) &&
    "Target function not found!");

  auto ExpF = createFunction(Sym->name());
  if (!ExpF)
    return ExpF.takeError();
  Function *F = ExpF.get();
  Value *V = Builder->CreateCall(F);
  updateFirst(V);
  return Error::success();
}


llvm::Error Translator::handleICall(llvm::Value *Target)
{
  store(Target, RV_T1);
  Value *V = Builder->CreateCall(ICaller);
  updateFirst(V);

  return Error::success();
}


llvm::Error Translator::handleCallExt(llvm::Value *Target)
{
  FunctionType *FT = FunctionType::get(Builder->getVoidTy(), !VAR_ARG);
  Value *V = Builder->CreateIntToPtr(Target, FT->getPointerTo());
  updateFirst(V);
  V = Builder->CreateCall(V);
  updateFirst(V);

  return Error::success();
}


llvm::Error Translator::handleJumpToOffs(
  uint64_t Target,
  llvm::Value *Cond,
  unsigned LinkReg)
{
  // Get current function
  const Function *CF = Builder->GetInsertBlock()->getParent();
  Function *F = Module->getFunction(CF->getName());

  // Next BB
  uint64_t NextInstrAddr = CurAddr + 4;

  // Link
  bool IsCall = LinkReg != RV_ZERO;
  if (!Cond && IsCall)
    store(ConstantInt::get(I32, NextInstrAddr), LinkReg);

  // Target BB
  assert(Target != CurAddr && "Unexpected jump to self instruction!");

  BasicBlock *BB = nullptr;
  auto Iter = BBMap.lower_bound(Target);
  bool NeedToTranslateBB = false;
  BasicBlock* entryBB = &F->getEntryBlock();
  BasicBlock* PredBB = nullptr;
  uint64_t BBEnd = 0;

  // Jump forward
  if (Target > CurAddr) {
    BasicBlock *BeforeBB = nullptr;
    if (Iter != BBMap.end())
      BeforeBB = Iter->val;

    // BB already exists
    if (Target == Iter->key)
      BB = Iter->val;
    // Need to create new BB
    else {
      BB = SBTBasicBlock::create(*Context, Target, F, BeforeBB);
      BBMap(Target, std::move(BB));

      updateNextBB(Target);
    }

  // jump backward
  } else {
    if (Iter == BBMap.end()) {
      assert(!BBMap.empty() && "BBMap is empty!");
      BB = (--Iter)->val;
    // BB entry matches target address
    } else if (Target == Iter->key)
      BB = Iter->val;
    // target BB is probably the previous one
    else if (Iter != BBMap.begin())
      BB = (--Iter)->val;
    // target BB is the first one
    else
      BB = Iter->val;

    // check bounds
    uint64_t Begin = Iter->key;
    uint64_t End = Iter->key + BB->size() * InstrSize;
    bool InBound = Target >= Begin && Target < End;

    // need to translate target
    if (!InBound) {
      BBEnd = Iter->key;
      PredBB = Iter->val;
      BB = SBTBasicBlock::create(*Context, Target, F, BB);
      BBMap(Target, std::move(BB));
      NeedToTranslateBB = true;
    // need to split BB?
    } else if (Iter->key != Target)
      BB = splitBB(BB, Target);
  }

  // need NextBB?
  bool NeedNextBB = !IsCall;

  BasicBlock *BeforeBB = nullptr;
  BasicBlock *Next = nullptr;
  if (NeedNextBB) {
    auto P = BBMap[NextInstrAddr];
    if (P)
      Next = *P;
    else {
      Iter = BBMap.lower_bound(NextInstrAddr);
      if (Iter != BBMap.end())
        BeforeBB = Iter->val;

      Next = SBTBasicBlock::create(*Context, NextInstrAddr, F, BeforeBB);
      BBMap(NextInstrAddr, std::move(Next));
    }

    updateNextBB(NextInstrAddr);
  }

  // Branch
  Value *V;
  if (Cond)
    V = Builder->CreateCondBr(Cond, BB, Next);
  else
    V = Builder->CreateBr(BB);
  updateFirst(V);

  // Use next BB
  if (NeedNextBB)
    Builder->SetInsertPoint(Next);

  // need to translate target BB?
  if (NeedToTranslateBB) {
    DBGS << "NeedToTranslateBB\n";

    // save insert point
    BasicBlock* PrevBB = Builder->GetInsertBlock();

    // add branch to correct function entry BB
    if (entryBB->getName() != "entry") {
      BasicBlock* newEntryBB =
        SBTBasicBlock::create(*Context, "entry", F, BB);
      Builder->SetInsertPoint(newEntryBB);
      Builder->CreateBr(entryBB);
    }

    // translate BB
    Builder->SetInsertPoint(BB);
    // translate up to the next BB
    if (auto E = translateInstrs(Target, BBEnd))
      return E;

    // link to the next BB if there is no terminator
    DBGS << "XBB=" << Builder->GetInsertBlock()->getName() << nl;
    DBGS << "TBB=" << PredBB->getName() << nl;
    if (Builder->GetInsertBlock()->getTerminator() == nullptr)
      Builder->CreateBr(PredBB);
    BrWasLast = true;

    // restore insert point
    Builder->SetInsertPoint(PrevBB);
  }

  DBGS << "BB=" << Builder->GetInsertBlock()->getName() << nl;
  return Error::success();
}


llvm::BasicBlock *Translator::splitBB(
  llvm::BasicBlock *BB,
  uint64_t Addr)
{
  auto Res = InstrMap[Addr];
  assert(Res && "Instruction not found!");

  Instruction *TgtInstr = *Res;

  BasicBlock::iterator I, E;
  for (I = BB->begin(), E = BB->end(); I != E; ++I) {
    if (&*I == TgtInstr)
      break;
  }
  assert(I != E);

  BasicBlock *BB2;
  if (BB->getTerminator()) {
    BB2 = BB->splitBasicBlock(I, SBTBasicBlock::getBBName(Addr));
    BBMap(Addr, std::move(BB2));
    return BB2;
  }

  // Insert dummy terminator
  assert(Builder->GetInsertBlock() == BB);
  Instruction *Instr = Builder->CreateRetVoid();
  BB2 = BB->splitBasicBlock(I, SBTBasicBlock::getBBName(Addr));
  BBMap(Addr, std::move(BB2));
  Instr->eraseFromParent();
  Builder->SetInsertPoint(BB2, BB2->end());

  return BB2;
}


llvm::Error Translator::handleIJump(
  llvm::Value *Target,
  unsigned LinkReg)
{
  llvm_unreachable("IJump support not implemented yet!");
}


llvm::Error Translator::startup()
{
  FunctionType *FT = FunctionType::get(Type::getVoidTy(*Context), !VAR_ARG);
  Function *F =
    Function::Create(FT, Function::PrivateLinkage, "rv32_startup", Module);

  // BB
  BasicBlock *BB = BasicBlock::Create(*Context, "bb_startup", F);
  BasicBlock *MainBB = Builder->GetInsertBlock();

  Builder->CreateCall(F);
  Builder->SetInsertPoint(BB);
  Builder->CreateRetVoid();
  Builder->SetInsertPoint(MainBB);

  return Error::success();
}

llvm::Expected<uint64_t> Translator::import(llvm::StringRef Func)
{
  std::string RV32Func = "rv32_" + Func.str();

  // check if the function was already processed
  if (Function *F = Module->getFunction(RV32Func))
    return *FunMap[F];

  // Load LibC module
  if (!LCModule) {
    if (!LIBC_BC) {
      SBTError SE;
      SE << "libc.bc file not found";
      return error(SE);
    }

    auto Res = MemoryBuffer::getFile(*LIBC_BC);
    if (!Res)
      return errorCodeToError(Res.getError());
    MemoryBufferRef Buf = **Res;

    auto ExpMod = parseBitcodeFile(Buf, *Context);
    if (!ExpMod)
      return ExpMod.takeError();
    LCModule = std::move(*ExpMod);
  }

  // lookup function
  Function *LF = LCModule->getFunction(Func);
  if (!LF) {
    SBTError SE;
    SE << "Function not found: " << Func;
    return error(SE);
  }
  FunctionType *FT = LF->getFunctionType();

  // declare imported function in our module
  Function *IF =
    Function::Create(FT, GlobalValue::ExternalLinkage, Func, Module);

  // create our caller to the external function
  FunctionType *VFT = FunctionType::get(Builder->getVoidTy(), !VAR_ARG);
  Function *F =
    Function::Create(VFT, GlobalValue::PrivateLinkage, RV32Func, Module);

  BasicBlock *BB = BasicBlock::Create(*Context, "entry", F);
  BasicBlock *PrevBB = Builder->GetInsertBlock();
  Builder->SetInsertPoint(BB);

  OnScopeExit RestoreInsertPoint(
    [this, PrevBB]() {
      Builder->SetInsertPoint(PrevBB);
    });

  unsigned NumParams = FT->getNumParams();
  assert(NumParams < 9 &&
      "External functions with more than 8 arguments are not supported!");

  // build Args
  std::vector<Value *> Args;
  unsigned Reg = RV_A0;
  unsigned I = 0;
  for (; I < NumParams; I++) {
    Value *V = load(Reg++);
    Type *Ty = FT->getParamType(I);

    // need to cast?
    if (Ty != I32)
      V = Builder->CreateBitOrPointerCast(V, Ty);

    Args.push_back(V);
  }

  // VarArgs: passing 4 extra args for now
  if (FT->isVarArg()) {
    unsigned N = MIN(I + 4, 8);
    for (; I < N; I++)
      Args.push_back(load(Reg++));
  }

  // call the Function
  Value *V;
  if (FT->getReturnType()->isVoidTy()) {
    V = Builder->CreateCall(IF, Args);
    updateFirst(V);
  } else {
    V = Builder->CreateCall(IF, Args, IF->getName());
    updateFirst(V);
    store(V, RV_A0);
  }

  V = Builder->CreateRetVoid();
  updateFirst(V);

  if (!ExtFunAddr)
    ExtFunAddr = CurSection->size();
  FunTable.push_back(F);
  FunMap(F, std::move(ExtFunAddr));
  uint64_t Ret = ExtFunAddr;
  ExtFunAddr += 4;

  return Ret;
}


llvm::Error Translator::translateUI(
  const llvm::MCInst &Inst,
  UIOp UOP,
  llvm::raw_string_ostream &SS)
{
  switch (UOP) {
    case AUIPC: SS << "auipc";  break;
    case LUI:   SS << "lui";    break;
  }
  SS << '\t';

  unsigned O = getRD(Inst, SS);
  auto ExpImm = getImm(Inst, 1, SS);
  if (!ExpImm)
    return ExpImm.takeError();
  Value *Imm = ExpImm.get();
  Value *V;

  if (LastImm.IsSym)
    V = Imm;
  else {
    // get upper immediate
    V = Builder->CreateShl(Imm, ConstantInt::get(I32, 12));
    updateFirst(V);

    // Add PC (CurAddr)
    if (UOP == AUIPC) {
      V = add(V, ConstantInt::get(I32, CurAddr));
      updateFirst(V);
    }
  }

  store(V, O);

  return Error::success();
}


llvm::Error Translator::translateFence(
  const llvm::MCInst &Inst,
  bool FI,
  llvm::raw_string_ostream &SS)
{
  if (FI) {
    SS << "fence.i";
    nop();
    return Error::success();
  }

  SS << "fence";
  AtomicOrdering Order = llvm::AtomicOrdering::AcquireRelease;
  SynchronizationScope Scope = llvm::SynchronizationScope::CrossThread;

  Value *V;
  V = Builder->CreateFence(Order, Scope);
  updateFirst(V);

  return Error::success();
}

llvm::Error Translator::translateCSR(
  const llvm::MCInst &Inst,
  CSROp Op,
  bool Imm,
  llvm::raw_string_ostream &SS)
{
  switch (Op) {
    case RW:
      assert(false && "No CSR write support for base I instructions!");
      break;

    case RS:
      SS << "csrrs";
      break;

    case RC:
      SS << "csrrc";
      break;
  }
  if (Imm)
    SS << "i";
  SS << '\t';

  unsigned RD = getRegNum(0, Inst, SS);
  uint64_t CSR = Inst.getOperand(1).getImm();
  uint64_t Mask;
  if (Imm)
    Mask = RV_A0; // Inst.getOperand(2).getImm();
  else
    Mask = getRegNum(2, Inst, SS);
  assert(Mask == RV_ZERO && "No CSR write support for base I instructions!");
  SS << llvm::formatv("0x{0:X-4} = ", CSR);

  Value *V = ConstantInt::get(I32, 0);
  switch (CSR) {
    case RDCYCLE:
      SS << "RDCYCLE";
      V = Builder->CreateCall(GetCycles);
      updateFirst(V);
      break;
    case RDCYCLEH:
      SS << "RDCYCLEH";
      break;
    case RDTIME:
      SS << "RDTIME";
      V = Builder->CreateCall(GetTime);
      updateFirst(V);
      break;
    case RDTIMEH:
      SS << "RDTIMEH";
      break;
    case RDINSTRET:
      SS << "RDINSTRET";
      V = Builder->CreateCall(InstRet);
      updateFirst(V);
      break;
    case RDINSTRETH:
      SS << "RDINSTRETH";
      break;
    default:
      assert(false && "Not implemented!");
      break;
  }

  store(V, RD);
  return Error::success();
}

llvm::Error Translator::translate(const std::string& file)
{
  // parse object file
  auto expObj = create<Object>(file);
  if (!expObj)
    return expObj.takeError();
  _obj = &expObj.get();

  // start module
  if (auto err = _translator->startModule())
    return err;

  // translate each section
  for (ConstSectionPtr sec : obj->sections()) {
    if (auto err = _translator->translateSection(sec))
      return err;

    // finish any pending function
    if (Error err = _translator->finishFunction())
      return err;
  }

  // finish module
  if (auto err = _translator->finishModule())
    return err;

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
