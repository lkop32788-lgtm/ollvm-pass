#pragma once

#include "llvm/IR/PassManager.h"

namespace ollvm {

/// Indirect Branching / Call Obfuscation Pass (间接调用混淆)
///
/// Converts direct call instructions into indirect calls via a function
/// pointer stored in a module-level, obfuscated dispatch table.
///
/// Additionally, direct branches between basic blocks may be replaced with
/// an indirect jump through a computed address so that the static call
/// graph becomes incomplete and signature-based detectors fail to trace
/// the execution path.
///
/// The dispatch table entries are encrypted at compile time and decrypted
/// lazily on first call using the same XOR rolling cipher employed by the
/// StringEncryption pass, keeping the kernel-safe no-heap constraint.
struct IndirectBranchingPass
    : public llvm::PassInfoMixin<IndirectBranchingPass> {
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
  static bool isRequired() { return false; }
};

} // namespace ollvm
