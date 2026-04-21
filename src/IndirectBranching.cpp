/// IndirectBranching.cpp — Pass 6: 间接调用混淆
///
/// Converts direct call instructions to module-internal functions into
/// indirect calls through an obfuscated function-pointer table.
///
/// The dispatch table is an array of encrypted function pointers stored in a
/// special linker section (".ollvm$iat").  At module startup (or before first
/// call), each entry is decrypted and stored into a module-level alloca
/// (simulated via a global, because module-level allocas don't exist in LLVM).
///
/// High-level transformation for  call @foo(args...):
///
///   @ollvm.dispatch.foo = internal global i8* encrypt(@foo)
///
///   ; before the call site:
///   %ptr = load i8*, @ollvm.dispatch.foo
///   %real = xor %ptr, KEY            ; decrypt
///   %fn = inttoptr %real to FooTy*
///   call %fn(args...)
///
/// This breaks static call-graph reconstruction and prevents signature-based
/// detection of known function call patterns.

#include "Obfuscation/IndirectBranching.h"
#include "Obfuscation/Utils.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <unordered_map>
#include <vector>

using namespace llvm;
using namespace ollvm;

namespace {

/// Returns the XOR-encrypted bitcast of a function pointer constant.
static Constant *encryptFunctionPointer(Function *F, uint64_t Key,
                                        LLVMContext &Ctx) {
  // Cast function pointer to i64 at compile time.
  Type *I64Ty = Type::getInt64Ty(Ctx);
  Constant *FPtr = ConstantExpr::getPtrToInt(F, I64Ty);
  Constant *KeyConst = ConstantInt::get(I64Ty, Key);
  return ConstantExpr::getXor(FPtr, KeyConst);
}

} // namespace

PreservedAnalyses IndirectBranchingPass::run(Module &M,
                                              ModuleAnalysisManager &) {
  LLVMContext &Ctx = M.getContext();
  Type *I64Ty = Type::getInt64Ty(Ctx);

  Xorshift64 Rng(getModuleSeed(M) ^ 0x1AD1BA7E00000000ULL);

  // Map from target Function* → (dispatch global, key).
  std::unordered_map<Function *, std::pair<GlobalVariable *, uint64_t>>
      DispatchTable;

  // Collect all direct call instructions targeting module-internal functions.
  using CallEntry = std::pair<CallInst *, Function *>;
  std::vector<CallEntry> Calls;

  for (auto &F : M) {
    if (F.isDeclaration() || !shouldObfuscate(F))
      continue;
    for (auto &BB : F) {
      for (auto &I : BB) {
        auto *CI = dyn_cast<CallInst>(&I);
        if (!CI)
          continue;
        Function *Callee = CI->getCalledFunction();
        if (!Callee)
          continue;
        // Only indirect-ify calls to internal (non-external) functions.
        if (Callee->isDeclaration())
          continue;
        // Skip intrinsics.
        if (Callee->isIntrinsic())
          continue;
        // Skip the current function (self-recursion handled elsewhere).
        if (Callee == &F)
          continue;
        Calls.emplace_back(CI, Callee);
      }
    }
  }

  if (Calls.empty())
    return PreservedAnalyses::all();

  // For each unique callee, create an encrypted dispatch global.
  for (auto &[CI, Callee] : Calls) {
    if (DispatchTable.count(Callee))
      continue;

    uint64_t Key = Rng.next();
    Constant *EncPtr = encryptFunctionPointer(Callee, Key, Ctx);

    // Store as an i64 global.
    GlobalVariable *DispGV = new GlobalVariable(
        M, I64Ty, /*isConstant=*/false, GlobalValue::InternalLinkage, EncPtr,
        ".ollvm.disp." + Callee->getName().str());
    DispGV->setSection(".ollvm$iat");
    DispGV->setAlignment(MaybeAlign(8));

    DispatchTable[Callee] = {DispGV, Key};
  }

  // Replace each direct call with an indirect call through the dispatch table.
  bool Changed = false;
  for (auto &[CI, Callee] : Calls) {
    auto It = DispatchTable.find(Callee);
    if (It == DispatchTable.end())
      continue;

    auto [DispGV, Key] = It->second;
    IRBuilder<> B(CI);

    // Load the encrypted pointer.
    Value *EncVal = B.CreateLoad(I64Ty, DispGV, "ibc.enc");
    // Decrypt: XOR with the key.
    Value *KeyVal = ConstantInt::get(I64Ty, Key);
    Value *DecVal = B.CreateXor(EncVal, KeyVal, "ibc.dec");
    // Cast back to the function pointer type.
    FunctionType *FTy = Callee->getFunctionType();
    PointerType *FPtrTy = PointerType::get(FTy, 0);
    Value *FPtr = B.CreateIntToPtr(DecVal, FPtrTy, "ibc.fptr");

    // Build new call with same arguments and attributes.
    SmallVector<Value *, 8> Args(CI->args());
    CallInst *NewCall = B.CreateCall(FTy, FPtr, Args);
    NewCall->setCallingConv(CI->getCallingConv());
    NewCall->setAttributes(CI->getAttributes());
    if (!CI->getType()->isVoidTy())
      CI->replaceAllUsesWith(NewCall);
    CI->eraseFromParent();
    Changed = true;
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
