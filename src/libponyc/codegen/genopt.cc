#include "genopt.h"

#include <string.h>

#include "llvm_config_begin.h"

#include <llvm/IR/Module.h>
#include <llvm/IR/AbstractCallSite.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/DebugInfo.h>

#include <llvm/IR/PassManager.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>

#include <llvm/Target/TargetMachine.h>
#include <llvm/Analysis/Lint.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/StripSymbols.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/ADT/SmallSet.h>

#include <llvm-c/DebugInfo.h>

#include "llvm_config_end.h"

#include "../../libponyrt/mem/heap.h"
#include "ponyassert.h"

using namespace llvm;
using namespace llvm::legacy;

static void print_transform(compile_t* c, Instruction* i, const char* s)
{
  errors_t* errors = c->opt->check.errors;

  if((c == NULL) || !c->opt->print_stats)
    return;

  /* Starting with LLVM 3.7.0-final getDebugLog may return a
   * DebugLoc without a valid underlying MDNode* for instructions
   * that have no direct source location, instead of returning 0
   * for getLine().
   */
  while(!i->getDebugLoc())
  {
    i = i->getNextNode();

    if(i == nullptr)
      return;
  }

  DebugLoc loc = i->getDebugLoc();

  DILocation* location = loc.get();
  DIScope* scope = location->getScope();
  DILocation* at = location->getInlinedAt();

  if(at != NULL)
  {
    DIScope* scope_at = at->getScope();

    errorf(errors, NULL, "[%s] %s:%u:%u@%s:%u:%u: %s",
      i->getParent()->getParent()->getName().str().c_str(),
      scope->getFilename().str().c_str(), loc.getLine(), loc.getCol(),
      scope_at->getFilename().str().c_str(), at->getLine(),
      at->getColumn(), s);
  }
  else {
    errorf(errors, NULL, "[%s] %s:%u:%u: %s",
      i->getParent()->getParent()->getName().str().c_str(),
      scope->getFilename().str().c_str(), loc.getLine(), loc.getCol(), s);
  }
}

// Pass to move Pony heap allocations to the stack.
class HeapToStack : public PassInfoMixin<HeapToStack>
{
public:
  compile_t* c;

  HeapToStack(compile_t* compiler) : PassInfoMixin<HeapToStack>()
  {
    c = compiler;
  }

  PreservedAnalyses run(Function &f, FunctionAnalysisManager &am)
  {
    DominatorTree& dt = am.getResult<DominatorTreeAnalysis>(f);
    BasicBlock& entry = f.getEntryBlock();
    IRBuilder<> builder(&entry, entry.begin());

    bool changed = false;

    for(auto block = f.begin(), end = f.end(); block != end; ++block)
    {
      for(auto iter = block->begin(), end = block->end(); iter != end; )
      {
        Instruction* inst = &(*(iter++));

        if(runOnInstruction(builder, inst, dt, f))
          changed = true;
      }
    }

    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }

  bool runOnInstruction(IRBuilder<>& builder, Instruction* inst,
    DominatorTree& dt, Function& f)
  {
    auto call_base = dyn_cast<CallBase>(inst);
    if (!call_base)
    {
      return false;
    }

    CallBase& call = *call_base;

    Function* fun = call.getCalledFunction();

    if(fun == NULL)
      return false;

    bool small = false;

    if(fun->getName().compare("pony_alloc") == 0)
    {
      // Nothing.
    } else if(fun->getName().compare("pony_alloc_small") == 0) {
      small = true;
    } else {
      return false;
    }

    Value* size = call.getArgOperand(1);
    c->opt->check.stats.heap_alloc++;
    ConstantInt* int_size = dyn_cast_or_null<ConstantInt>(size);

    if(int_size == NULL)
    {
      print_transform(c, inst, "variable size allocation");
      return false;
    }

    uint64_t alloc_size = int_size->getZExtValue();

    if(small)
    {
      // Convert a heap index to a size.
      int_size = ConstantInt::get(builder.getInt64Ty(),
        ((int64_t)1) << (alloc_size + HEAP_MINBITS));
    } else {
      if(alloc_size > 1024)
      {
        print_transform(c, inst, "large allocation");
        return false;
      }
    }

    SmallVector<CallInst*, 4> tail;
    SmallVector<Instruction*, 4> new_calls;

    if(!canStackAlloc(inst, dt, tail, new_calls))
      return false;

    for(auto iter = tail.begin(), end = tail.end(); iter != end; ++iter)
      (*iter)->setTailCall(false);

    // TODO: for variable size alloca, don't insert at the beginning.
    Instruction* begin = &(*call.getCaller()->getEntryBlock().begin());

    unsigned int align = target_is_ilp32(c->opt->triple) ? 4 : 8;

    AllocaInst* replace = new AllocaInst(builder.getInt8Ty(), 0, int_size,
      Align(align), "", begin);

    replace->setDebugLoc(inst->getDebugLoc());

    inst->replaceAllUsesWith(replace);

    auto invoke = dyn_cast<InvokeInst>(static_cast<Instruction*>(&call));
    if (invoke)
    {
      BranchInst::Create(invoke->getNormalDest(), invoke);
      invoke->getUnwindDest()->removePredecessor(call.getParent());
    }

    inst->eraseFromParent();

    for(auto new_call: new_calls)
    {
      // Force constructor inlining to see if fields can be stack-allocated.
      InlineFunctionInfo ifi{};

      auto new_call_base = dyn_cast<CallBase>(new_call);
      if (new_call_base)
      {
        InlineFunction(*new_call_base, ifi);
      }
    }

    print_transform(c, replace, "stack allocation");
    c->opt->check.stats.heap_alloc--;
    c->opt->check.stats.stack_alloc++;

    return true;
  }

  bool canStackAlloc(Instruction* alloc, DominatorTree& dt,
    SmallVector<CallInst*, 4>& tail, SmallVector<Instruction*, 4>& new_calls)
  {
    // This is based on the pass in the LDC compiler.
    SmallVector<Use*, 16> work;
    SmallSet<Use*, 16> visited;

    for(auto iter = alloc->use_begin(), end = alloc->use_end();
      iter != end; ++iter)
    {
      Use* use = &(*iter);
      visited.insert(use);
      work.push_back(use);
    }

    while(!work.empty())
    {
      Use* use = work.pop_back_val();
      Instruction* inst = cast<Instruction>(use->getUser());
      Value* value = use->get();

      switch(inst->getOpcode())
      {
        case Instruction::Call:
        case Instruction::Invoke:
        {
          // Record any calls that are tail calls so they can be marked as
          // "tail false" if we do a HeapToStack change. Accessing `alloca`
          // memory from a call that is marked as "tail" is unsafe and can
          // result in incorrect optimizations down the road.
          //
          // See: https://github.com/ponylang/ponyc/issues/4340
          //
          // Technically we don't need to do this for every call, just calls
          // that touch alloca'd memory. However, without doing some alias
          // analysis at this point, our next best bet is to simply mark
          // every call as "not tail" if we do any HeapToStack change. It's
          // "the safest" thing to do.
          //
          // N.B. the contents of the `tail` list will only be set to
          // "tail false" if we return `true` from this `canStackAlloc`
          // function.
          auto ci = dyn_cast<CallInst>(inst);
          if (ci && ci->isTailCall())
          {
            tail.push_back(ci);
          }

          auto call_base = dyn_cast<CallBase>(inst);
          if (!call_base)
          {
            return false;
          }

          CallBase& call = *call_base;

          if(call.onlyReadsMemory())
          {
            // If the function is readnone or readonly, and the return value
            // isn't and does not contain a pointer, it isn't captured.
            Type* type = inst->getType();

            if(type->isVoidTy() ||
              type->isFPOrFPVectorTy() ||
              type->isIntOrIntVectorTy()
              )
            {
              // Doesn't return any pointers, so it isn't captured.
              break;
            }
          }

          if(inst->getMetadata("pony.newcall") != NULL)
            new_calls.push_back(inst);

          auto first = call.arg_begin();

          for(auto iter = first, end = call.arg_end(); iter != end; ++iter)
          {
            if(iter->get() == value)
            {
              // If it's not marked as nocapture, it's captured.
              if(!call.doesNotCapture((unsigned)(iter - first)))
              {
                print_transform(c, alloc, "captured allocation");
                print_transform(c, inst, "captured here (call arg)");
                return false;
              }
            }
          }
          break;
        }

        case Instruction::Load:
          break;

        case Instruction::Store:
        {
          // If we store the pointer, it is captured.
          // TODO: if we store the pointer in something that is stack allocated
          // is it still captured?
          if(value == inst->getOperand(0))
          {
            print_transform(c, alloc, "captured allocation");
            print_transform(c, inst, "captured here (store)");
            return false;
          }
          break;
        }

        case Instruction::BitCast:
        case Instruction::GetElementPtr:
        case Instruction::PHI:
        case Instruction::Select:
        {
          // If a derived pointer can be reused, it can't be stack allocated.
          if(canBeReused(inst, alloc, dt))
            return false;

          // Check that the new value isn't captured.
          for(auto iter = inst->use_begin(), end = inst->use_end();
            iter != end; ++iter)
          {
            Use* use = &(*iter);

            if(visited.insert(use).second)
              work.push_back(use);
          }
          break;
        }

        default:
        {
          // If it's anything else, assume it's captured just in case.
          print_transform(c, alloc, "captured allocation");
          print_transform(c, inst, "captured here (unknown)");
          return false;
        }
      }
    }

    return true;
  }

  bool canBeReused(Instruction* def, Instruction* alloc, DominatorTree& dt)
  {
    if(def->use_empty() || !dt.dominates(def, alloc))
      return false;

    BasicBlock* def_block = def->getParent();
    BasicBlock* alloc_block = alloc->getParent();

    SmallSet<User*, 16> users;
    SmallSet<BasicBlock*, 16> user_blocks;

    for(auto iter = def->use_begin(), end = def->use_end();
      iter != end; ++iter)
    {
      Use* use = &(*iter);
      Instruction* user = cast<Instruction>(use->getUser());
      BasicBlock* user_block = user->getParent();

      if((alloc_block != user_block) && dt.dominates(alloc_block, user_block))
      {
        print_transform(c, alloc, "captured allocation");
        print_transform(c, def, "captured here (dominated reuse)");
        return true;
      }

      if(!isa<PHINode>(user))
      {
        users.insert(user);
        user_blocks.insert(user_block);
      }
    }

    typedef std::pair<BasicBlock*, Instruction*> Work;
    SmallVector<Work, 16> work;
    SmallSet<BasicBlock*, 16> visited;

    Instruction* start = alloc->getNextNode();
    work.push_back(Work(alloc_block, start));

    while(!work.empty())
    {
      Work w = work.pop_back_val();
      BasicBlock* bb = w.first;
      Instruction* inst = w.second;

      if(user_blocks.count(bb))
      {
        if((bb != def_block) && (bb != alloc_block))
        {
          print_transform(c, alloc, "captured allocation");
          print_transform(c, def,
            "captured here (block contains neither alloc nor def)");
          return true;
        }

        while(inst != nullptr)
        {
          if((inst == def) || (inst == alloc))
            break;

          if(users.count(inst))
          {
            print_transform(c, alloc, "captured allocation");
            print_transform(c, inst, "captured here (reused)");
            return true;
          }

          inst = inst->getNextNode();
        }
      }
      else if((bb == def_block) || ((bb == alloc_block) && (inst != start)))
      {
        continue;
      }

      Instruction* term = bb->getTerminator();
      unsigned count = term->getNumSuccessors();

      for(unsigned i = 0; i < count; i++)
      {
        BasicBlock* successor = term->getSuccessor(i);
        inst = &successor->front();
        bool found = false;

        while(isa<PHINode>(inst))
        {
          if(def == cast<PHINode>(inst)->getIncomingValueForBlock(bb))
          {
            print_transform(c, alloc, "captured allocation");
            print_transform(c, inst, "captured here (phi use)");
            return true;
          }

          if(def == inst)
            found = true;

          inst = inst->getNextNode();
        }

        if(!found &&
          visited.insert(successor).second &&
          dt.dominates(def_block, successor))
        {
          work.push_back(Work(successor, inst));
        }
      }
    }

    return false;
  }

  void getAnalysisUsage(AnalysisUsage& use) const
  {
    use.addRequired<DominatorTreeWrapperPass>();
  }
};

// Pass to replace pony_ctx calls in a dispatch function by the context passed
// to the function.
class DispatchPonyCtx : public PassInfoMixin<DispatchPonyCtx>
{
public:

  DispatchPonyCtx() : PassInfoMixin<DispatchPonyCtx>()
  {}

  PreservedAnalyses run(Function &f, FunctionAnalysisManager &am)
  {
    // Check if we're in a Dispatch function.
    StringRef name = f.getName();
    if(name.size() < 10 || name.rfind("_Dispatch") != name.size() - 9)
      return PreservedAnalyses::all();

    pony_assert(f.arg_size() > 0);

    Value* ctx = &(*f.arg_begin());

    bool changed = false;

    for(auto block = f.begin(), end = f.end(); block != end; ++block)
    {
      for(auto iter = block->begin(), end = block->end(); iter != end; ++iter)
      {
        Instruction* inst = &(*iter);

        if(runOnInstruction(inst, ctx))
          changed = true;
      }
    }

    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }

  bool runOnInstruction(Instruction* inst, Value* ctx)
  {
    auto call_base = dyn_cast<CallBase>(inst);
    if (!call_base)
    {
      return false;
    }

    CallBase& call = *call_base;
    Function* fun = call.getCalledFunction();

    if(fun == NULL)
      return false;

    if(fun->getName().compare("pony_ctx") != 0)
      return false;

    inst->replaceAllUsesWith(ctx);

    return true;
  }
};

// A pass to group message sends in the same BasicBlock.
class MergeMessageSend : public PassInfoMixin<MergeMessageSend>
{
public:
  struct MsgFnGroup
  {
    BasicBlock::iterator alloc, trace, done, send;
  };

  enum MsgKind
  {
    MsgNone,
    MsgNoTrace,
    MsgTrace
  };

  compile_t* c;
  Function* send_next_fn;
  Function* msg_chain_fn;
  Function* sendv_single_fn;

  MergeMessageSend(compile_t* compiler) : PassInfoMixin<MergeMessageSend>()
  {
    c = compiler;
    send_next_fn = nullptr;
    msg_chain_fn = nullptr;
    sendv_single_fn = nullptr;
  }

  void doInitialization(Module& m)
  {
    if (send_next_fn != NULL) return;

    send_next_fn = m.getFunction("pony_send_next");
    msg_chain_fn = m.getFunction("pony_chain");
    sendv_single_fn = m.getFunction("pony_sendv_single");

    if(send_next_fn == NULL)
      send_next_fn = declareTraceNextFn(m);

    if(msg_chain_fn == NULL)
      msg_chain_fn = declareMsgChainFn(m);

    return;
  }

  PreservedAnalyses run(Function &f, FunctionAnalysisManager &am)
  {
    doInitialization(*f.getParent());

    bool changed = false;
    for (auto block = f.begin(), end = f.end(); block != end; ++block)
    {
      changed = changed || runOnBasicBlock(*block);
    }

    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }

  bool runOnBasicBlock(BasicBlock& b)
  {
    auto start = b.begin();
    auto end = b.end();
    bool changed = false;
    SmallVector<CallInst*, 16> sends;

    // This pass is written with the assumption that it is operating on send
    // code instruction sequences that look like they do when coming fresh out
    // of LLVM IR generation in the CodeGen pass.
    // Thus, it is not safe to run this pass on blocks where it has already run,
    // or more specifically, blocks stitched together from combined blocks where
    // the pass has been run on some of the constituent blocks.
    // In such cases, this pass may move interleaved instructions around in
    // unexpected ways that break LLVM IR rules.
    // So here we check for the presence of any call instructions to
    // pony_send_next or pony_chain, which will only be present if this pass
    // has already run, because this is the only place where they are produced.
    // If we encounter such a call instruction in the basic block,
    // we will bail out without modifying anything in the basic block.
    bool already_ran = findCallTo(
      std::vector<StringRef>{"pony_send_next", "pony_chain"},
      start, end, true).second >= 0;
    if(already_ran)
      return false;

    while(start != end)
    {
      MsgFnGroup first;
      MsgKind first_kind = findMsgSend(start, end, true, first);

      switch(first_kind)
      {
        case MsgNone:
          start = end;
          break;

        default:
        {
          sends.push_back(cast<CallInst>(&(*first.send)));
          bool stop = false;

          while(!stop)
          {
            MsgFnGroup next;
            MsgKind next_kind = findMsgSend(std::next(first.send), end, false,
              next);

            switch(next_kind)
            {
              case MsgNone:
                stop = true;
                break;

              default:
                sends.push_back(cast<CallInst>(&(*next.send)));
                mergeMsgSend(first, next, first_kind, next_kind);
                first = next;
                first_kind = next_kind;
                changed = true;
                break;
            }
          }

          sends.push_back(nullptr);
          start = std::next(first.send);
          break;
        }
      }
    }

    if(!sends.empty())
      makeMsgChains(sends);

    return changed;
  }

  MsgKind findMsgSend(BasicBlock::iterator start, BasicBlock::iterator end,
    bool can_pass_writes, MsgFnGroup& out_calls)
  {
    auto alloc = findCallTo("pony_alloc_msg", start, end, can_pass_writes);

    if(alloc == end)
      return MsgNone;

    decltype(alloc) trace;
    size_t fn_index;

    std::tie(trace, fn_index) =
      findCallTo(std::vector<StringRef>{"pony_gc_send", "pony_sendv",
        "pony_sendv_single"}, std::next(alloc), end, true);

    switch(fn_index)
    {
      case (size_t)-1:
        return MsgNone;

      case 0:
        break;

      default:
        out_calls.alloc = alloc;
        out_calls.send = trace;
        return MsgNoTrace;
    }

    auto done = findCallTo("pony_send_done", std::next(trace), end, true);

    if(done == end)
      return MsgNone;

    auto send = findCallTo(std::vector<StringRef>{"pony_sendv",
      "pony_sendv_single"}, std::next(done), end, true).first;

    if(send == end)
      return MsgNone;

    out_calls.alloc = alloc;
    out_calls.trace = trace;
    out_calls.done = done;
    out_calls.send = send;
    return MsgTrace;
  }

  BasicBlock::iterator findCallTo(StringRef name,
    BasicBlock::iterator start, BasicBlock::iterator end, bool can_pass_writes)
  {
    return findCallTo(std::vector<StringRef>{name}, start, end, can_pass_writes).first;
  }

  std::pair<BasicBlock::iterator, size_t> findCallTo(
      std::vector<StringRef> const& names, BasicBlock::iterator start,
    BasicBlock::iterator end, bool can_pass_writes)
  {
    for(auto iter = start; iter != end; ++iter)
    {
      if(iter->getMetadata("pony.msgsend") != NULL)
      {
        auto call_base = dyn_cast<CallBase>(&*iter);
        if (!call_base)
          continue;

        CallBase& call = *call_base;

        Function* fun = call.getCalledFunction();
        pony_assert(fun != NULL);

        for(size_t i = 0; i < names.size(); i++)
        {
          if(fun->getName().compare(names[i]) == 0)
            return {iter, i};
        }
      }

      if(!can_pass_writes && iter->mayWriteToMemory())
        break;
    }

    return {end, -1};
  }

  void mergeMsgSend(MsgFnGroup& first, MsgFnGroup& next, MsgKind first_kind,
    MsgKind& next_kind)
  {
    pony_assert((first_kind != MsgNone) && (next_kind != MsgNone));

    if(first_kind == MsgNoTrace)
    {
      auto src = first.send;
      auto dst = next.send;

      while(1)
      {
        auto prev = std::prev(src);
        src->moveBefore(&(*dst));

        if(prev->getMetadata("pony.msgsend") == NULL)
          break;

        auto call_base = dyn_cast<CallBase>(&*prev);
        if (!call_base)
          break;
        CallBase& call = *call_base;

        auto fun = call.getCalledFunction();
        pony_assert(fun != NULL);

        if(fun->getName().compare("pony_alloc_msg") == 0)
          break;

        dst = src;
        src = prev;
      }
    } else {
      auto iter = first.alloc;

      while(iter != first.trace)
      {
        auto& inst = (*iter++);
        inst.moveBefore(&(*next.alloc));
      }

      if(next_kind == MsgNoTrace)
      {
        auto first_send_post = std::next(first.send);

        while(iter != first_send_post)
        {
          auto& inst = *(iter++);
          if(inst.getOpcode() == Instruction::Call)
            inst.moveBefore(&(*next.send));
        }

        next.trace = first.trace;
        next.done = first.done;
        next_kind = MsgTrace;
      } else {
        while(iter != first.done)
        {
          auto& inst = *(iter++);
          if(inst.getOpcode() == Instruction::Call)
            inst.moveBefore(&(*next.trace));
        }

        iter++;
        first.done->eraseFromParent();

        auto call_base = dyn_cast<CallBase>(&*next.trace);
        CallBase& call = *call_base;

        call.setCalledFunction(send_next_fn);

        auto first_send_post = std::next(first.send);
        auto next_done_post = std::next(next.done);

        while(iter != first_send_post)
        {
          auto& inst = *(iter++);
          if(inst.getOpcode() == Instruction::Call)
            inst.moveBefore(&(*next_done_post));
        }

        next.trace = first.trace;
      }
    }
  }

  bool makeMsgChains(SmallVector<CallInst*, 16> const& sends)
  {
    if(sends.size() < 2)
      return false;

    auto iter = std::begin(sends);
    auto next = std::next(iter);
    Value* chain_start = nullptr;
    bool is_single = false;
    bool changed = false;

    while(true)
    {
      pony_assert(*iter != nullptr);

      if((next == std::end(sends)) || (*next == nullptr))
      {
        if(chain_start != nullptr)
        {
          (*iter)->setArgOperand(2, chain_start);
          chain_start = nullptr;

          if(is_single)
          {
            pony_assert(sendv_single_fn != nullptr);
            (*iter)->setCalledFunction(sendv_single_fn);
            is_single = false;
          }
        }

        if(next == std::end(sends))
          break;

        iter = std::next(next);

        if(iter == std::end(sends))
          break;

        next = std::next(iter);
        continue;
      }

      auto prev_msg = (*iter)->getArgOperand(2);
      auto next_msg = (*next)->getArgOperand(2);

      pony_assert(prev_msg == (*iter)->getArgOperand(3));
      pony_assert(next_msg == (*next)->getArgOperand(3));

      if((*iter)->getArgOperand(1) == (*next)->getArgOperand(1))
      {
        if((*iter)->getCalledFunction() == sendv_single_fn)
          is_single = true;

        auto chain_call = CallInst::Create(msg_chain_fn, {prev_msg, next_msg},
          "", *iter);
        chain_call->setTailCall();
        (*iter)->eraseFromParent();

        if(chain_start == nullptr)
        {
          chain_start = prev_msg;
          changed = true;
        }
      } else if(chain_start != nullptr) {
        (*iter)->setArgOperand(2, chain_start);
        chain_start = nullptr;

        if(is_single)
        {
          pony_assert(sendv_single_fn != nullptr);
          (*iter)->setCalledFunction(sendv_single_fn);
          is_single = false;
        }
      }

      iter = next;
      next = std::next(iter);
    }

    return changed;
  }

  Function* declareTraceNextFn(Module& m)
  {
    FunctionType* fn_type = FunctionType::get(unwrap(c->void_type),
      {unwrap(c->ptr)}, false);
    Function* fn = Function::Create(fn_type, Function::ExternalLinkage,
      "pony_send_next", &m);

    fn->addFnAttr(Attribute::NoUnwind);
    return fn;
  }

  Function* declareMsgChainFn(Module& m)
  {
    FunctionType* fn_type = FunctionType::get(unwrap(c->void_type),
      {unwrap(c->ptr), unwrap(c->ptr)}, false);
    Function* fn = Function::Create(fn_type, Function::ExternalLinkage,
      "pony_chain", &m);

    unsigned functionIndex = AttributeList::FunctionIndex;

    fn->addFnAttr(Attribute::NoUnwind);
    fn->setOnlyAccessesArgMemory();
    fn->addParamAttr(1, Attribute::NoCapture);
    fn->addParamAttr(2, Attribute::ReadNone);
    return fn;
  }
};

static void optimise(compile_t* c, bool pony_specific)
{
  // Most of this is the standard ceremony for running standard LLVM passes.
  // See <https://llvm.org/docs/NewPassManager.html> for details.

  PassBuilder PB;
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;

  // Wire in the Pony-specific passes if requested.
  if(pony_specific)
  {
    PB.registerPeepholeEPCallback(
      [&](FunctionPassManager &fpm, OptimizationLevel level) {
        if(level.getSpeedupLevel() >= 2) {
          fpm.addPass(HeapToStack(c));
        }
      }
    );
    PB.registerScalarOptimizerLateEPCallback(
      [&](FunctionPassManager &fpm, OptimizationLevel level) {
        if(level.getSpeedupLevel() >= 2) {
          fpm.addPass(DispatchPonyCtx());
          fpm.addPass(MergeMessageSend(c));
        }
      }
    );
  }

  // Add a linting pass at the start, if requested.
  if (c->opt->lint_llvm) {
    PB.registerOptimizerEarlyEPCallback(
      [&](ModulePassManager &mpm, OptimizationLevel level) {
        mpm.addPass(createModuleToFunctionPassAdaptor(LintPass()));
      }
    );
  }

  // There is a problem with optimised debug info in certain cases. This is
  // due to unknown bugs in the way ponyc is generating debug info. When they
  // are found and fixed, an optimised build should not always strip debug
  // info.
  if(c->opt->release)
    c->opt->strip_debug = true;

  // Add a debug-info-stripping pass at the end, if requested.
  if(c->opt->strip_debug) {
    PB.registerOptimizerLastEPCallback(
      [&](ModulePassManager &mpm, OptimizationLevel level) {
        mpm.addPass(StripSymbolsPass());
      }
    );
  }

  // Enable the default alias analysis pipeline.
  FAM.registerPass([&] { return PB.buildDefaultAAPipeline(); });

  // Wire in all of the analysis managers.
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  // Create the top-level module pass manager using the default LLVM pipeline.
  // Choose the appropriate optimization level based on if we're in a release.
  ModulePassManager MPM;

  if (c->opt->release) {
    MPM = PB.buildPerModuleDefaultPipeline(OptimizationLevel::O3);
  } else {
    MPM = PB.buildO0DefaultPipeline(OptimizationLevel::O0);
  }

  // Run the passes.
  MPM.run(*unwrap(c->module), MAM);
}

bool genopt(compile_t* c, bool pony_specific)
{
  errors_t* errors = c->opt->check.errors;

  // Finalise the debug info.
  LLVMDIBuilderFinalize(c->di);
  optimise(c, pony_specific);

  if(c->opt->verify)
  {
    if(c->opt->verbosity >= VERBOSITY_MINIMAL)
      fprintf(stderr, "Verifying\n");

    char* msg = NULL;

    if(LLVMVerifyModule(c->module, LLVMPrintMessageAction, &msg) != 0)
    {
      errorf(errors, NULL, "Module verification failed: %s", msg);
      errorf_continue(errors, NULL,
        "Please file an issue ticket. Use --noverify to bypass this error.");
      LLVMDisposeMessage(msg);
      return false;
    }

    if(msg != NULL)
      LLVMDisposeMessage(msg);
  }

  return true;
}

bool target_is_linux(char* t)
{
  Triple triple = Triple(t);

  return triple.isOSLinux();
}

bool target_is_bsd(char* t)
{
  Triple triple = Triple(t);

  return triple.isOSDragonFly() || triple.isOSFreeBSD() || triple.isOSOpenBSD();
}

bool target_is_freebsd(char* t)
{
  Triple triple = Triple(t);

  return triple.isOSFreeBSD();
}

bool target_is_dragonfly(char* t)
{
  Triple triple = Triple(t);

  return triple.isOSDragonFly();
}

bool target_is_openbsd(char* t)
{
  Triple triple = Triple(t);

  return triple.isOSOpenBSD();
}

bool target_is_macosx(char* t)
{
  Triple triple = Triple(t);

  return triple.isMacOSX();
}

bool target_is_windows(char* t)
{
  Triple triple = Triple(t);

  return triple.isOSWindows();
}

bool target_is_posix(char* t)
{
  Triple triple = Triple(t);

  return triple.isMacOSX() || triple.isOSFreeBSD() || triple.isOSLinux()
    || triple.isOSDragonFly() || triple.isOSOpenBSD();
}

bool target_is_x86(char* t)
{
  Triple triple = Triple(t);

  std::string s = Triple::getArchTypePrefix(triple.getArch()).str();
  const char* arch = s.c_str();

  return !strcmp("x86", arch);
}

bool target_is_arm(char* t)
{
  Triple triple = Triple(t);

  std::string s = Triple::getArchTypePrefix(triple.getArch()).str();
  const char* arch = s.c_str();

  return (!strcmp("arm", arch) || !strcmp("aarch64", arch));
}

bool target_is_arm32(char* t)
{
  Triple triple = Triple(t);
  std::string s = Triple::getArchTypePrefix(triple.getArch()).str();
  const char* arch = s.c_str();
  return !strcmp("arm", arch) && target_is_ilp32(t);
}

bool target_is_riscv(char* t)
{
  Triple triple = Triple(t);
  std::string s = Triple::getArchTypePrefix(triple.getArch()).str();
  const char* arch = s.c_str();
  return (!strcmp("riscv32", arch) || !strcmp("riscv64", arch));
}

// This function is used to safeguard against potential oversights on the size
// of Bool on any future port to PPC32.
// We do not currently support compilation to PPC. It could work, but no
// guarantees.
bool target_is_ppc(char* t)
{
  Triple triple = Triple(t);

  std::string s = Triple::getArchTypePrefix(triple.getArch()).str();
  const char* arch = s.c_str();

  return !strcmp("ppc", arch);
}

bool target_is_lp64(char* t)
{
  Triple triple = Triple(t);

  return triple.isArch64Bit() && !triple.isOSWindows();
}

bool target_is_llp64(char* t)
{
  Triple triple = Triple(t);

  return triple.isArch64Bit() && triple.isOSWindows();
}

bool target_is_ilp32(char* t)
{
  Triple triple = Triple(t);

  return triple.isArch32Bit();
}

bool target_is_native128(char* t)
{
  Triple triple = Triple(t);

  return !triple.isArch32Bit() && !triple.isKnownWindowsMSVCEnvironment();
}

bool target_is_bigendian(char* t)
{
  Triple triple = Triple(t);

  return !triple.isLittleEndian();
}

bool target_is_littleendian(char* t)
{
  Triple triple = Triple(t);

  return triple.isLittleEndian();
}
