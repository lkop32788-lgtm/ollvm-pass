#pragma once

#include "llvm/IR/PassManager.h"

namespace ollvm {

/// Constant Encryption / Integer Splitting Pass (常量混淆)
///
/// Replaces integer constant operands with runtime-computed expressions that
/// yield the same value but are invisible to static analysis, e.g.:
///
///   x = 0x1234;
/// becomes:
///   x = (seed ^ enc_val) >> shift;   // decrypts to 0x1234 at runtime
///
/// For larger immediates a two-part split is used:
///   x = (hi_part << 16) | lo_part;
///
/// Only constants larger than a threshold (default 8-bit) are processed to
/// keep the overhead reasonable for kernel code.
struct ConstantEncryptionPass
    : public llvm::PassInfoMixin<ConstantEncryptionPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return false; }
};

} // namespace ollvm
