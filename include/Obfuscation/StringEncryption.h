#pragma once

#include "llvm/IR/PassManager.h"

namespace ollvm {

/// String Encryption Pass (字符串加密)
///
/// Locates every string literal that is referenced by the module, replaces
/// the plain-text constant with an encrypted byte array in a special section,
/// and inserts a tiny inline decryption stub that recovers the original
/// content at runtime before first use.
///
/// The decryption stub uses a rolling-XOR cipher keyed per-string so that
/// identical plaintext strings still produce different ciphertext, thwarting
/// simple string-table searches in kernel memory scanners.
///
/// Special care is taken to:
///  • Keep the decrypted string in a stack-local buffer (not patching the
///    read-only section) so the same encrypted copy can be decoded many times.
///  • Not allocate heap memory (important for early-boot kernel drivers).
struct StringEncryptionPass
    : public llvm::PassInfoMixin<StringEncryptionPass> {
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
  static bool isRequired() { return false; }
};

} // namespace ollvm
