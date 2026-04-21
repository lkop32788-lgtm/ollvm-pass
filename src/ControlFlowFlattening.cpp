/// ControlFlowFlattening.cpp — Pass 1: 控制流平坦化
///
/// Transforms the CFG of every eligible function into a switch-dispatch loop
/// so that the original control-flow edges become invisible to static analysis.
///
/// Algorithm:
///   1. Collect all basic blocks except the entry block.
///   2. Split the entry block at its terminator to create a dedicated
///      "pre-header" and a loop header containing the dispatch switch.
///   3. Assign each original block a random case number (the "state" value).
///   4. Redirect every terminator that targets an original block to instead
///      store the corresponding state into a stack slot and jump back to the
///      loop header.
///   5. Wrap everything in:  entry → loop_header (switch(state) { ... })
///
/// This preserves full functionality because the switch faithfully executes
/// every block in the same order as the original control flow.

#include "Obfuscation/ControlFlowFlattening.h"
#include "Obfuscation/Utils.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"

#include <vector>

/// Sentinel state value used when a successor block is not found in the
/// dispatch table.  The dispatch switch's default case is Unreachable, so
/// this value should never be loaded at runtime.
static constexpr uint32_t kUnknownState = 0xDEADBEEFu;

using namespace llvm;
using namespace ollvm;

/// Promote allocas to SSA form before flattening so that phi-nodes do not
/// reference blocks that will be reordered.  We do this manually (demote back
/// to alloca) because the full mem2reg pass can't run on modified CFG yet.
static void demotePhiNodesToStack(Function &F) {
  // Collect all PHI nodes that will become problematic.
  SmallVector<PHINode *, 16> Phis;
  for (auto &BB : F)
    for (auto &I : BB)
      if (auto *PN = dyn_cast<PHINode>(&I))
        Phis.push_back(PN);

  for (auto *PN : Phis) {
    // Insert alloca in the entry block.
    IRBuilder<> AllocaBuilder(&F.getEntryBlock(),
                              F.getEntryBlock().getFirstInsertionPt());
    AllocaInst *Slot =
        AllocaBuilder.CreateAlloca(PN->getType(), nullptr, PN->getName() + ".slot");

    // For each incoming value, store before the predecessor's terminator.
    for (unsigned i = 0, e = PN->getNumIncomingValues(); i < e; ++i) {
      IRBuilder<> StoreBuilder(PN->getIncomingBlock(i)->getTerminator());
      StoreBuilder.CreateStore(PN->getIncomingValue(i), Slot);
    }

    // Replace uses of PHI with a load.
    IRBuilder<> LoadBuilder(PN);
    LoadInst *Load = LoadBuilder.CreateLoad(PN->getType(), Slot,
                                            PN->getName() + ".reload");
    PN->replaceAllUsesWith(Load);
    PN->eraseFromParent();
  }
}

PreservedAnalyses
ControlFlowFlatteningPass::run(Function &F, FunctionAnalysisManager &) {
  if (!shouldObfuscate(F))
    return PreservedAnalyses::all();

  // Skip tiny functions — flattening adds overhead that is not worth it.
  if (F.size() <= 2)
    return PreservedAnalyses::all();

  // Step 0: Demote PHI nodes to memory so we can freely reorder blocks.
  demotePhiNodesToStack(F);

  LLVMContext &Ctx = F.getContext();
  Xorshift64 Rng(getModuleSeed(*F.getParent()) ^ (uint64_t)(uintptr_t)&F);

  // Step 1: Collect blocks (skip entry block — it will remain the prologue).
  BasicBlock *EntryBB = &F.getEntryBlock();

  // We need at least one more block to flatten.
  if (std::next(F.begin()) == F.end())
    return PreservedAnalyses::all();

  // Split entry block so that all original instructions stay in a new block
  // ("entry_body"), and the current entry block just jumps to the loop header.
  // entry  →  loop_header (switch(state)) → case blocks → loop_header
  //
  // If the entry block's terminator is itself a branch, we need to split
  // BEFORE it to capture the "real" first instructions.
  Instruction *EntrySplitPt = EntryBB->getTerminator();
  BasicBlock *EntryBody = nullptr;
  // Only split if there are non-terminator instructions in the entry block.
  if (EntrySplitPt != &EntryBB->front()) {
    EntryBody = EntryBB->splitBasicBlock(EntrySplitPt, "cff.entry_body");
  } else {
    EntryBody = EntryBB; // degenerate: entry block is only a terminator
  }

  // Collect all blocks that will become switch cases (everything except
  // the pre-entry block and the to-be-created dispatch block).
  std::vector<BasicBlock *> Blocks;
  for (auto &BB : F) {
    if (&BB == EntryBB)
      continue;
    Blocks.push_back(&BB);
  }

  if (Blocks.empty())
    return PreservedAnalyses::all();

  // Step 2: Create the dispatch block (loop header).
  // Insert an alloca for the "state" variable in the entry block.
  IRBuilder<> AllocaBuilder(EntryBB, EntryBB->getFirstInsertionPt());
  Type *Int32Ty = Type::getInt32Ty(Ctx);
  AllocaInst *StateSlot =
      AllocaBuilder.CreateAlloca(Int32Ty, nullptr, "cff.state");

  // Assign a random case number to each block.
  std::vector<uint32_t> CaseNums(Blocks.size());
  for (size_t i = 0; i < Blocks.size(); ++i)
    CaseNums[i] = static_cast<uint32_t>(Rng.next() & 0x7FFFFFFF);

  // The initial state is the case number of EntryBody (first block after entry).
  uint32_t EntryBodyCase = CaseNums[0]; // Blocks[0] == EntryBody typically

  // Create the loop header block right after the entry block.
  BasicBlock *LoopHeader =
      BasicBlock::Create(Ctx, "cff.dispatch", &F, EntryBody);

  // Create the default block (unreachable — should never be reached).
  BasicBlock *DefaultBB = BasicBlock::Create(Ctx, "cff.default", &F);
  IRBuilder<> DefaultBuilder(DefaultBB);
  DefaultBuilder.CreateUnreachable();

  // Patch the original entry block's terminator to:
  //   store initial_state; jump to loop_header
  {
    Instruction *OldTerm = EntryBB->getTerminator();
    IRBuilder<> B(OldTerm);
    B.CreateStore(ConstantInt::get(Int32Ty, EntryBodyCase), StateSlot);
    B.CreateBr(LoopHeader);
    OldTerm->eraseFromParent();
  }

  // Build the dispatch switch in LoopHeader.
  IRBuilder<> DispatchBuilder(LoopHeader);
  LoadInst *StateLoad =
      DispatchBuilder.CreateLoad(Int32Ty, StateSlot, "cff.state.load");
  SwitchInst *Switch =
      DispatchBuilder.CreateSwitch(StateLoad, DefaultBB, Blocks.size());

  // For each block, add a case and redirect its terminator to update state
  // and jump back to the loop header.
  for (size_t i = 0; i < Blocks.size(); ++i) {
    BasicBlock *BB = Blocks[i];
    uint32_t MyCase = CaseNums[i];

    Switch->addCase(ConstantInt::get(Int32Ty, MyCase), BB);

    // Walk the terminator of BB and replace successor references with
    // "store next_state; jump to loop_header".
    Instruction *Term = BB->getTerminator();

    if (auto *BI = dyn_cast<BranchInst>(Term)) {
      if (BI->isUnconditional()) {
        BasicBlock *Succ = BI->getSuccessor(0);
        // Find Succ's case number (or default if it's not in our list).
        IRBuilder<> B(Term);
        int succIdx = -1;
        for (size_t j = 0; j < Blocks.size(); ++j)
          if (Blocks[j] == Succ)
            succIdx = (int)j;
        if (succIdx >= 0)
          B.CreateStore(ConstantInt::get(Int32Ty, CaseNums[succIdx]), StateSlot);
        else
          B.CreateStore(ConstantInt::get(Int32Ty, kUnknownState), StateSlot);
        B.CreateBr(LoopHeader);
        Term->eraseFromParent();
      } else {
        // Conditional branch: keep the condition but replace successors.
        BasicBlock *TrueBB = BI->getSuccessor(0);
        BasicBlock *FalseBB = BI->getSuccessor(1);
        // Create two small "set-state-and-jump" blocks.
        auto makeStateBlock = [&](BasicBlock *Succ,
                                  const char *Name) -> BasicBlock * {
          BasicBlock *StateBlock = BasicBlock::Create(Ctx, Name, &F);
          IRBuilder<> B2(StateBlock);
          int succIdx = -1;
          for (size_t j = 0; j < Blocks.size(); ++j)
            if (Blocks[j] == Succ)
              succIdx = (int)j;
          if (succIdx >= 0)
            B2.CreateStore(ConstantInt::get(Int32Ty, CaseNums[succIdx]),
                           StateSlot);
          else
            B2.CreateStore(ConstantInt::get(Int32Ty, kUnknownState), StateSlot);
          B2.CreateBr(LoopHeader);
          return StateBlock;
        };
        BasicBlock *TrueState = makeStateBlock(TrueBB, "cff.true");
        BasicBlock *FalseState = makeStateBlock(FalseBB, "cff.false");
        IRBuilder<> B(Term);
        B.CreateCondBr(BI->getCondition(), TrueState, FalseState);
        Term->eraseFromParent();
      }
    }
    // Return / Unreachable terminators are left as-is — they exit the function.
  }

  return PreservedAnalyses::none();
}
