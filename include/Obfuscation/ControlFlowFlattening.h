#pragma once

#include "llvm/IR/PassManager.h"

namespace ollvm {

/// Control-Flow Flattening Pass (控制流平坦化)
///
/// Transforms every function body into a single dispatch loop:
///
///   int state = entry_state;
///   while (true) {
///     switch (state) {
///       case BB_0: ...; state = next; break;
///       case BB_1: ...; state = next; break;
///       ...
///     }
///   }
///
/// This makes static analysis and CFG recovery significantly harder without
/// altering runtime behaviour.
struct ControlFlowFlatteningPass
    : public llvm::PassInfoMixin<ControlFlowFlatteningPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return false; }
};

} // namespace ollvm
