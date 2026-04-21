#pragma once

#include "llvm/IR/PassManager.h"

namespace ollvm {

/// Bogus Control-Flow Pass (虚假控制流)
///
/// For each basic block, inserts an opaque predicate that always evaluates
/// to true before branching to the real successor.  A dead clone of the
/// block is appended as the "false" branch, filled with junk instructions.
///
/// This inflates the apparent complexity of the CFG and confuses automated
/// decompilers without changing observable behaviour.
struct BogusControlFlowPass
    : public llvm::PassInfoMixin<BogusControlFlowPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return false; }
};

} // namespace ollvm
