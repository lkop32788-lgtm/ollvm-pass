/// BogusControlFlow.cpp — Pass 2: 虚假控制流
///
/// For each qualifying basic block in a function, this pass:
///   1. Splits the block into a "test" portion and an "original" portion.
///   2. Inserts an opaque predicate (always true) before the split point.
///   3. Connects the true branch to the original continuation.
///   4. Clones the original continuation as a "junk" block connected to the
///      false branch — the junk block is never actually executed.
///
/// The net effect is that the CFG appears to have twice as many edges, and
/// decompilers produce badly mis-attributed code for the dead clones.

#include "Obfuscation/BogusControlFlow.h"
#include "Obfuscation/Utils.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <vector>

using namespace llvm;
using namespace ollvm;

/// Returns a clone of BB with all instructions duplicated but operands
/// left pointing at the originals (so the clone is dead but looks real).
static BasicBlock *cloneBlockAsJunk(BasicBlock *BB, Function &F,
                                    BasicBlock *RealSuccessor) {
  LLVMContext &Ctx = F.getContext();
  BasicBlock *Junk = BasicBlock::Create(Ctx, BB->getName() + ".junk", &F);

  ValueToValueMapTy VMap;
  for (auto &I : *BB) {
    if (isa<PHINode>(&I))
      continue; // skip phi nodes in the clone
    Instruction *Clone = I.clone();
    Clone->setName(I.getName() + ".junk");
    Junk->getInstList().push_back(Clone);
    VMap[&I] = Clone;
  }

  // Fix up operands of the clone to use the cloned values where available.
  for (auto &I : *Junk) {
    for (unsigned op = 0, e = I.getNumOperands(); op < e; ++op) {
      Value *V = I.getOperand(op);
      if (VMap.count(V))
        I.setOperand(op, VMap[V]);
    }
  }

  // Replace the terminator with an unconditional branch to RealSuccessor
  // so the block is syntactically valid.
  if (!Junk->empty() && isa<TerminatorInst>(Junk->back()))
    Junk->back().eraseFromParent();
  IRBuilder<> B(Junk);
  B.CreateBr(RealSuccessor);
  return Junk;
}

PreservedAnalyses BogusControlFlowPass::run(Function &F,
                                            FunctionAnalysisManager &) {
  if (!shouldObfuscate(F))
    return PreservedAnalyses::all();
  if (F.size() < 2)
    return PreservedAnalyses::all();

  Xorshift64 Rng(getModuleSeed(*F.getParent()) ^ 0xBEEF0000ULL ^
                 (uint64_t)(uintptr_t)&F);

  // Collect candidates — we'll modify blocks while iterating, so snapshot.
  std::vector<BasicBlock *> Candidates;
  for (auto &BB : F) {
    // Skip small blocks (only a terminator) and landing pads.
    if (BB.size() < 2 || BB.isLandingPad())
      continue;
    Candidates.push_back(&BB);
  }

  // Apply bogus CF to ~60% of blocks selected randomly to keep size growth
  // manageable.
  for (BasicBlock *BB : Candidates) {
    if (!Rng.nextBool())
      continue; // ~50% chance

    // Find a good split point: after the first non-phi instruction so that
    // phi nodes stay in the head of the block.
    Instruction *SplitBefore = nullptr;
    for (auto &I : *BB) {
      if (!isa<PHINode>(&I) && !I.isTerminator()) {
        SplitBefore = &I;
        break;
      }
    }
    if (!SplitBefore)
      continue;

    // Split: BB → [BB_head] → [BB_tail]
    BasicBlock *BBTail = BB->splitBasicBlock(SplitBefore, BB->getName() + ".bcf_tail");

    // The original branch at the end of BB_head now goes to BBTail.
    // We want to replace it with:  if (opaque_true) goto BBTail else goto JunkBB
    Instruction *OldBranch = BB->getTerminator();
    assert(isa<BranchInst>(OldBranch) && cast<BranchInst>(OldBranch)->isUnconditional());

    // Create junk block branching to BBTail so the program is valid.
    BasicBlock *JunkBB = cloneBlockAsJunk(BBTail, F, BBTail);

    // Build the opaque predicate using a dummy integer argument.
    IRBuilder<> Builder(OldBranch);
    // Use a pointer-derived integer as the unpredictable seed.
    Value *Seed = Builder.CreatePtrToInt(
        BB->getFirstNonPHI(), Builder.getInt64Ty(), "bcf.seed");
    Value *OpaqPred = createOpaqueTruePredicate(Builder, Seed);
    Builder.CreateCondBr(OpaqPred, BBTail, JunkBB);
    OldBranch->eraseFromParent();
  }

  return PreservedAnalyses::none();
}
