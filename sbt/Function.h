#ifndef SBT_FUNCTION_H
#define SBT_FUNCTION_H

#include "BasicBlock.h"
#include "Context.h"
#include "FRegister.h"
#include "Map.h"
#include "Object.h"
#include "Pointer.h"
#include "XRegister.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

#include <map>
#include <memory>
#include <vector>


namespace llvm {
class BasicBlock;
class Function;
class Instruction;
class Module;
}

namespace sbt {

class SBTSection;

class Function
{
public:
    /**
     * ctor.
     *
     * @param ctx
     * @param name
     * @param sec section of translated binary that contains the function
     * @param addr function address
     * @param end guest memory address of first byte after function's last byte
     *
     * Note: section/end may be null/0 when the function does not correspond
     *             to a guest one.
     */
    Function(
        Context* ctx,
        const std::string& name,
        SBTSection* sec = nullptr,
        uint64_t addr = Constants::INVALID_ADDR,
        uint64_t end = Constants::INVALID_ADDR)
        :
        _ctx(ctx),
        _name(name),
        _sec(sec),
        _addr(addr),
        _end(end),
        _regsMode(_ctx->opts->regs())
    {}

    /**
     * Create the function.
     *
     * @param ft function type
     * @param linkage function linkage
     */
    void create(
        llvm::FunctionType* ft = nullptr,
        llvm::Function::LinkageTypes linkage = llvm::Function::ExternalLinkage);

    // translate function
    llvm::Error translate();


    // getters

    const std::string& name() const
    {
        return _name;
    }

    // llvm function pointer
    llvm::Function* func() const
    {
        return _f;
    }

    uint64_t addr() const
    {
        return _addr;
    }

    // BB helpers

    /**
     * Create a new BB inside this function and add it to the map.
     *
     * @param addr BB start address
     * @param beforeBB place new BB before another one, or at the end if null.
     */
    BasicBlock* newBB(uint64_t addr, BasicBlock* beforeBB = nullptr)
    {
        _bbMap.upsert(addr, BasicBlockPtr(
            new BasicBlock(_ctx, addr, _f,
                beforeBB? beforeBB->bb() : nullptr)));
        return &**_bbMap[addr];
    }

    /**
     * Add BB to map.
     */
    BasicBlock* addBB(uint64_t addr, BasicBlockPtr&& bb)
    {
        _bbMap.upsert(addr, std::move(bb));
        return &**_bbMap[addr];
    }

    /**
     * Create a new untracked BB.
     *
     * @param addr BB address. This will be part of its name and is also used
     *             to place it correctly among other untracked BBs.
     * @param name BB name suffix
     */
    BasicBlock* newUBB(uint64_t addr, const std::string& name)
    {
        const std::string bbname = BasicBlock::getBBName(addr) + "_" + name;
        BasicBlock* beforeBB = lowerBoundBB(addr + Constants::INSTRUCTION_SIZE);
        BasicBlock* bb = new BasicBlock(_ctx, bbname, _f,
            beforeBB? beforeBB->bb() : nullptr);
        bb->untracked(true);
        BasicBlockPtr bbptr(bb);

        std::vector<BasicBlockPtr>* p = _ubbMap[addr];
        // first on this addr
        if (!p) {
            std::vector<BasicBlockPtr> vec;
            vec.emplace_back(std::move(bbptr));
            _ubbMap.upsert(addr, std::move(vec));
        } else
            p->emplace_back(std::move(bbptr));
        return bb;
    }

    /**
     * Lookup BB by address.
     */
    BasicBlock* findBB(uint64_t addr)
    {
        BasicBlockPtr* ptr = _bbMap[addr];
        if (!ptr)
            return nullptr;
        return &**ptr;
    }

    /**
     * Get lower bound BB.
     */
    BasicBlock* lowerBoundBB(uint64_t addr)
    {
        auto it = _bbMap.lower_bound(addr);
        if (it == _bbMap.end())
            return nullptr;
        return &*it->val;
    }

    /**
     * Get back edge BB.
     */
    BasicBlock* getBackBB(uint64_t addr)
    {
        auto it = _bbMap.lower_bound(addr);

        // last
        if (it == _bbMap.end()) {
            xassert(!_bbMap.empty() && "empty bbMap!");
            --it;
        // exact match
        } else if (it->key == addr)
            ;
        // previous
        else if (it != _bbMap.begin())
            --it;
        // else: first

        return &*it->val;
    }

    /**
     * Get the address of the next BB.
     */
    uint64_t nextBBAddr(uint64_t curAddr)
    {
        auto it = _bbMap.lower_bound(curAddr + Constants::INSTRUCTION_SIZE);
        if (it != _bbMap.end())
            return it->key;
        return Constants::INVALID_ADDR;
    }

    // update next basic block address
    void updateNextBB(uint64_t addr)
    {
        if (_nextBB <= addr || addr < _nextBB)
            _nextBB = addr;
    }

    // set/update BB end
    void setEnd(uint64_t end)
    {
        _end = end;
    }

    /**
     * Transfer BBs to another function.
     *
     * @param from transfer BBs from this address until the end of the
     *             function.
     * @param to function to transfer the BBs to.
     */
    void transferBBs(uint64_t from, Function* to);

    /**
     * Translate instructions at given address range.
     *
     * @param st starting address
     * @param end end address (after last instruction)
     */
    llvm::Error translateInstrs(uint64_t st, uint64_t end);

    // was function terminated?
    bool terminated() const;

    /**
     * Get local register.
     */
    Register& getReg(size_t i)
    {
        if (locals() || abi()) {
            xassert(_regs);
            return _regs->getReg(i);
        } else
            return _ctx->x->getReg(i);
    }

    /**
     * Get local float register.
     */
    Register& getFReg(size_t i)
    {
        if (locals() || abi()) {
            xassert(_fregs);
            return _fregs->getReg(i);
        } else
            return _ctx->f->getReg(i);
    }

    bool globals() const {
        return _regsMode == Options::Regs::GLOBALS;
    }

    bool locals() const {
        return _regsMode == Options::Regs::LOCALS;
    }

    bool abi() const {
        return _regsMode == Options::Regs::ABI;
    }

    bool localRegs() const {
        return locals() || abi();
    }

    /**
     * Erase unused local registers.
     */
    void cleanRegs();

    enum SyncFlags {
        S_CALL          = 0x01,
        S_CALL_RETURNED = 0x02,
        S_FUNC_START    = 0x04,
        S_FUNC_RETURN   = 0x08,
        S_LOAD          = 0x10,
        S_ABI           = 0x20,
        S_RET_REGS_ONLY = 0x40,
        S_XREG          = 0x80
    };

    // sync local register file
    void loadRegisters(int syncFlags = 0);
    void storeRegisters(int syncFlags = 0);
    void freturn();

    void setCFAOffs(llvm::Value* v);
    llvm::Value* getSpilledAddress(llvm::Constant* imm, Register::Type t);

    // setup argc/argv
    void copyArgv();

    // look up function by address
    static Function* getByAddr(
        Context* ctx,
        uint64_t addr,
        ConstSectionPtr sec = nullptr);

    static bool isFunction(
        Context* ctx,
        uint64_t addr,
        ConstSectionPtr sec = nullptr);

    void addIndBr(llvm::IndirectBrInst* ibr) {
        _indBrs.push_back(ibr);
    }

    void addIndBB(BasicBlock* ibb) {
        _indBBs.push_back(ibb);
    }

    void processIndirectBranches();

private:
    Context* _ctx;
    std::string _name;
    SBTSection* _sec;
    uint64_t _addr;
    uint64_t _end;
    Options::Regs _regsMode;

    llvm::Function* _f = nullptr;
    // address of next basic block
    uint64_t _nextBB = 0;
    Map<uint64_t, BasicBlockPtr> _bbMap;

    // untracked BBs map: <addr, BBs>
    // addr: guest address where the BBs belong to
    Map<uint64_t, std::vector<BasicBlockPtr>> _ubbMap;

    Function* _nextf = nullptr;

    std::unique_ptr<XRegisters> _regs;
    std::unique_ptr<FRegisters> _fregs;

    // indirect branches
    std::vector<llvm::IndirectBrInst*> _indBrs;
    std::vector<BasicBlock*> _indBBs;

    // spill data
    static const int64_t INVALID_CFA = ~0ll;
    int64_t _cfaOffs = INVALID_CFA;


    class Spill {
    public:
        Spill(Context* ctx, BasicBlock* bb, int64_t idx, Register::Type t);

        Register::Type regType() const {
            return _rty;
        }

        llvm::Value* val() const {
            return _i32;
        }

    private:
        llvm::Value* _var;
        llvm::Value* _i32;
        Register::Type _rty;
    };

    std::map<int64_t, Spill> _spillMap;

    // methods

    llvm::Error startMain();
    llvm::Error start();
    llvm::Error finish();

    void spillInit();
};

using FunctionPtr = Pointer<Function>;

}

#endif
