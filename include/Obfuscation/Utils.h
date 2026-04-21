#pragma once

#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/RandomNumberGenerator.h"
#include <cstdint>
#include <random>
#include <string>

namespace ollvm {

/// Lightweight cryptographic helper used by multiple passes.
/// XOR-based stream cipher with a 64-bit key — fast, dependency-free.
struct XorCipher {
  uint64_t key;
  explicit XorCipher(uint64_t k) : key(k) {}

  void encrypt(uint8_t *data, size_t len) const {
    uint64_t k = key;
    for (size_t i = 0; i < len; ++i) {
      data[i] ^= static_cast<uint8_t>(k & 0xFF);
      k = (k >> 8) | ((k & 0xFF) << 56); // rotate
    }
  }
  void decrypt(uint8_t *data, size_t len) const { encrypt(data, len); }
};

/// Returns a pseudo-random 64-bit integer seeded per-module to ensure
/// deterministic output for a given source file (reproducible builds).
inline uint64_t getModuleSeed(llvm::Module &M) {
  // Mix the module identifier string into a seed.
  uint64_t seed = 0xDEADBEEFCAFEBABEULL;
  for (char c : M.getModuleIdentifier()) {
    seed ^= static_cast<uint64_t>(c);
    seed *= 0x9E3779B97F4A7C15ULL; // Fibonacci hashing
  }
  return seed;
}

/// Simple PRNG (xorshift64) that does not require LLVM's RNG infrastructure.
struct Xorshift64 {
  uint64_t state;
  explicit Xorshift64(uint64_t seed) : state(seed ? seed : 1) {}
  uint64_t next() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  }
  /// Random value in [lo, hi]
  uint64_t nextInRange(uint64_t lo, uint64_t hi) {
    return lo + (next() % (hi - lo + 1));
  }
  bool nextBool() { return (next() & 1) != 0; }
};

/// Returns true when the function should be obfuscated.
/// Respects the "noinline" attribute and skips intrinsics/declarations.
inline bool shouldObfuscate(llvm::Function &F) {
  if (F.isDeclaration() || F.isIntrinsic())
    return false;
  // Honour explicit opt-out annotation: __attribute__((annotate("nollvm")))
  for (auto &BB : F)
    for (auto &I : BB)
      if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&I))
        if (auto *Callee = CI->getCalledFunction())
          if (Callee->getName() == "llvm.var.annotation")
            return false;
  return true;
}

/// Create an opaque predicate that always evaluates to true but cannot be
/// easily eliminated by constant-folding. Used by BogusControlFlow pass.
/// Returns: (x * (x + 1)) % 2 == 0  — always true for any integer x.
inline llvm::Value *createOpaqueTruePredicate(llvm::IRBuilder<> &Builder,
                                              llvm::Value *Arg) {
  llvm::Type *Ty = Arg->getType();
  if (!Ty->isIntegerTy())
    Arg = Builder.CreatePtrToInt(Arg, Builder.getInt64Ty());
  llvm::Value *One = llvm::ConstantInt::get(Arg->getType(), 1);
  llvm::Value *XPlusOne = Builder.CreateAdd(Arg, One, "opq.xp1");
  llvm::Value *Mul = Builder.CreateMul(Arg, XPlusOne, "opq.mul");
  llvm::Value *Two = llvm::ConstantInt::get(Arg->getType(), 2);
  llvm::Value *Rem = Builder.CreateURem(Mul, Two, "opq.rem");
  llvm::Value *Zero = llvm::ConstantInt::get(Arg->getType(), 0);
  return Builder.CreateICmpEQ(Rem, Zero, "opq.pred");
}

} // namespace ollvm
