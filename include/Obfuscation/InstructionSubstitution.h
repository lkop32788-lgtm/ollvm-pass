#pragma once

#include "llvm/IR/PassManager.h"

namespace ollvm {

/// Instruction Substitution Pass (指令替换)
///
/// Replaces common arithmetic and bitwise instructions with semantically
/// equivalent but less obvious sequences, e.g.:
///   a + b  →  a - (~b) - 1         (add via not+sub)
///   a - b  →  a + (~b) + 1         (sub via not+add)
///   a & b  →  ~(~a | ~b)           (De Morgan)
///   a | b  →  ~(~a & ~b)
///   a ^ b  →  (a | b) & ~(a & b)   (XOR via OR/AND)
///
/// Substitutions are applied at a configurable probability to avoid
/// bloating the binary too aggressively.
struct InstructionSubstitutionPass
    : public llvm::PassInfoMixin<InstructionSubstitutionPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return false; }
};

} // namespace ollvm
