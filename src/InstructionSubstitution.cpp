/// InstructionSubstitution.cpp — Pass 3: 指令替换
///
/// Replaces common arithmetic / bitwise operations with longer but
/// semantically identical sequences.  Each substitution is applied with a
/// configurable probability so we don't bloat every instruction.
///
/// Substitution table (all identities hold for integer arithmetic):
///
///  ADD  a, b  →  SUB( NOT(NOT(a) + NOT(b) + 1), 0 )   // or simpler below
///             →  a - (-b)                              // negate-then-sub
///             →  a - (~b) - 1                          // via NOT
///
///  SUB  a, b  →  a + (~b) + 1                          // two's complement
///             →  a - (b ^ 0) (trivial, mix with next)
///
///  AND  a, b  →  ~(~a | ~b)                            // De Morgan
///
///  OR   a, b  →  ~(~a & ~b)                            // De Morgan
///
///  XOR  a, b  →  (a | b) & ~(a & b)                   // set diff
///             →  (a & ~b) | (~a & b)
///
/// All replacements are inserted in-place; the old instruction is removed.

#include "Obfuscation/InstructionSubstitution.h"
#include "Obfuscation/Utils.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"

#include <vector>

using namespace llvm;
using namespace ollvm;

// Probability (out of 4) that a given instruction is substituted.
static constexpr unsigned kSubstProb = 3; // 3/4 ≈ 75%

/// Build the replacement for ADD: a - (~b) - 1
static Value *subAddition(IRBuilder<> &B, Value *A, Value *Bv) {
  Value *NotB = B.CreateNot(Bv, "sub.notb");
  Value *Sub1 = B.CreateSub(A, NotB, "sub.s1");
  Value *One = ConstantInt::get(A->getType(), 1);
  return B.CreateSub(Sub1, One, "sub.add");
}

/// Build the replacement for SUB: a + (~b) + 1
static Value *subSubtraction(IRBuilder<> &B, Value *A, Value *Bv) {
  Value *NotB = B.CreateNot(Bv, "sub.notb");
  Value *Add1 = B.CreateAdd(A, NotB, "sub.a1");
  Value *One = ConstantInt::get(A->getType(), 1);
  return B.CreateAdd(Add1, One, "sub.sub");
}

/// Build the replacement for AND: ~(~a | ~b)
static Value *subAnd(IRBuilder<> &B, Value *A, Value *Bv) {
  Value *NotA = B.CreateNot(A, "sub.nota");
  Value *NotB = B.CreateNot(Bv, "sub.notb");
  Value *OrAB = B.CreateOr(NotA, NotB, "sub.or");
  return B.CreateNot(OrAB, "sub.and");
}

/// Build the replacement for OR: ~(~a & ~b)
static Value *subOr(IRBuilder<> &B, Value *A, Value *Bv) {
  Value *NotA = B.CreateNot(A, "sub.nota");
  Value *NotB = B.CreateNot(Bv, "sub.notb");
  Value *AndAB = B.CreateAnd(NotA, NotB, "sub.and");
  return B.CreateNot(AndAB, "sub.or");
}

/// Build the replacement for XOR: (a | b) & ~(a & b)
static Value *subXor(IRBuilder<> &B, Value *A, Value *Bv) {
  Value *OrAB = B.CreateOr(A, Bv, "sub.or");
  Value *AndAB = B.CreateAnd(A, Bv, "sub.and");
  Value *NotAnd = B.CreateNot(AndAB, "sub.nand");
  return B.CreateAnd(OrAB, NotAnd, "sub.xor");
}

PreservedAnalyses
InstructionSubstitutionPass::run(Function &F, FunctionAnalysisManager &) {
  if (!shouldObfuscate(F))
    return PreservedAnalyses::all();

  Xorshift64 Rng(getModuleSeed(*F.getParent()) ^ 0xC0DE0000ULL ^
                 (uint64_t)(uintptr_t)&F);

  bool Changed = false;

  // Collect instructions first to avoid iterator invalidation.
  std::vector<BinaryOperator *> WorkList;
  for (auto &BB : F)
    for (auto &I : BB)
      if (auto *BinOp = dyn_cast<BinaryOperator>(&I))
        WorkList.push_back(BinOp);

  for (auto *BinOp : WorkList) {
    // Only process integer operations.
    if (!BinOp->getType()->isIntegerTy())
      continue;
    // Skip if not selected by probability.
    if (Rng.next() % 4 >= kSubstProb)
      continue;

    Value *A = BinOp->getOperand(0);
    Value *Bv = BinOp->getOperand(1);
    IRBuilder<> Builder(BinOp);
    Value *Replacement = nullptr;

    switch (BinOp->getOpcode()) {
    case Instruction::Add:
      Replacement = subAddition(Builder, A, Bv);
      break;
    case Instruction::Sub:
      Replacement = subSubtraction(Builder, A, Bv);
      break;
    case Instruction::And:
      Replacement = subAnd(Builder, A, Bv);
      break;
    case Instruction::Or:
      Replacement = subOr(Builder, A, Bv);
      break;
    case Instruction::Xor:
      Replacement = subXor(Builder, A, Bv);
      break;
    default:
      continue;
    }

    if (Replacement) {
      BinOp->replaceAllUsesWith(Replacement);
      BinOp->eraseFromParent();
      Changed = true;
    }
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
