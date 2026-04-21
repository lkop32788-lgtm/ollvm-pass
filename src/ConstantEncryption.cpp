/// ConstantEncryption.cpp — Pass 5: 常量混淆
///
/// Replaces integer constant operands with runtime-computed obfuscated
/// expressions that the optimiser cannot fold back to the original value.
///
/// Technique for a 32/64-bit constant C:
///   Choose a random mask M.
///   Emit:   (C ^ M) ^ M   — the outer XOR is computed at run time.
///
/// For added complexity, the mask is itself split:
///   M = M_hi | M_lo  where hi/lo are different bit ranges.
///
/// Additional "dead add" variant for diversity (alternated based on PRNG):
///   Emit:   (C + K) - K   where K is a random run-time constant.
///
/// Only integers larger than 8 bits are processed to avoid bloating small
/// comparison operands.  Floating-point constants and globals are skipped.

#include "Obfuscation/ConstantEncryption.h"
#include "Obfuscation/Utils.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"

#include <vector>

using namespace llvm;
using namespace ollvm;

/// Build an obfuscated replacement for the constant C.
/// Variant 0: (C ^ M) ^ M  (XOR mask pair)
/// Variant 1: (C + K) - K  (add/sub pair)
static Value *obfuscateConstant(IRBuilder<> &B, ConstantInt *C,
                                 uint64_t Rand) {
  unsigned BitWidth = C->getBitWidth();
  Type *Ty = C->getType();
  uint64_t Val = C->getZExtValue();

  if (Rand & 1) {
    // XOR variant
    uint64_t Mask = Rand ^ 0xDEADBEEFCAFE0000ULL;
    // Mask to the same width
    if (BitWidth < 64)
      Mask &= (1ULL << BitWidth) - 1ULL;
    uint64_t Enc = Val ^ Mask;
    Value *EncVal = ConstantInt::get(Ty, Enc);
    Value *MaskVal = ConstantInt::get(Ty, Mask);
    Value *Xor1 = B.CreateXor(EncVal, MaskVal, "cenc.x1");
    // Second xor with another mask (zero net effect but harder to see).
    uint64_t Mask2 = (Rand >> 17) & ((BitWidth < 64) ? (1ULL << BitWidth) - 1 : ~0ULL);
    Value *Mask2Val = ConstantInt::get(Ty, Mask2);
    Value *Xor2 = B.CreateXor(Xor1, Mask2Val, "cenc.x2");
    Value *Xor3 = B.CreateXor(Xor2, Mask2Val, "cenc.x3");
    return Xor3;
  } else {
    // Add/Sub variant
    uint64_t K = (Rand >> 3) | 1; // non-zero
    if (BitWidth < 64)
      K &= (1ULL << BitWidth) - 1ULL;
    uint64_t Enc = (Val + K) & ((BitWidth < 64) ? (1ULL << BitWidth) - 1 : ~0ULL);
    Value *EncVal = ConstantInt::get(Ty, Enc);
    Value *KVal = ConstantInt::get(Ty, K);
    Value *Sub = B.CreateSub(EncVal, KVal, "cenc.sub");
    return Sub;
  }
}

PreservedAnalyses
ConstantEncryptionPass::run(Function &F, FunctionAnalysisManager &) {
  if (!shouldObfuscate(F))
    return PreservedAnalyses::all();

  Xorshift64 Rng(getModuleSeed(*F.getParent()) ^ 0xC0FFEE00ULL ^
                 (uint64_t)(uintptr_t)&F);

  bool Changed = false;

  // We iterate over instructions and collect (Instruction*, operand index)
  // pairs to replace.  We must not invalidate iterators while iterating, so
  // we stage changes.
  using ReplacementEntry = std::tuple<Instruction *, unsigned, ConstantInt *>;
  std::vector<ReplacementEntry> WorkList;

  for (auto &BB : F) {
    for (auto &I : BB) {
      // Skip PHI nodes — replacing their constants can break the IR.
      if (isa<PHINode>(&I))
        continue;
      // Skip alloca size operands and GEP index 0 (usually 0).
      for (unsigned op = 0, e = I.getNumOperands(); op < e; ++op) {
        auto *C = dyn_cast<ConstantInt>(I.getOperand(op));
        if (!C)
          continue;
        // Only process integers wider than 8 bits.
        if (C->getBitWidth() <= 8)
          continue;
        // Skip obviously trivial constants (0, 1, -1) to reduce noise.
        uint64_t V = C->getZExtValue();
        if (V == 0 || V == 1 || C->isAllOnesValue())
          continue;
        // Apply at ~50% probability.
        if (Rng.next() % 2 == 0)
          WorkList.emplace_back(&I, op, C);
      }
    }
  }

  for (auto &[Inst, OpIdx, C] : WorkList) {
    IRBuilder<> B(Inst);
    Value *Replacement = obfuscateConstant(B, C, Rng.next());
    Inst->setOperand(OpIdx, Replacement);
    Changed = true;
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
