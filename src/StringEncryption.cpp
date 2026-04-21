/// StringEncryption.cpp — Pass 4: 字符串加密
///
/// Finds all ConstantDataArray / ConstantDataSequential values that look like
/// string literals (i8 arrays with a null terminator), encrypts them with a
/// per-string rolling-XOR key, stores the ciphertext as a new global in the
/// .rdata section, and replaces every use of the original literal with a call
/// to an inline decryption helper that:
///   • Allocates a stack buffer.
///   • XOR-decrypts into the buffer on first use.
///   • Returns a pointer to the stack buffer.
///
/// Important for Windows kernel drivers:
///   • NO heap allocation (no ExAllocatePool).
///   • Stack buffer uses alloca — safe at any IRQL below DISPATCH_LEVEL.
///   • Works with both narrow (char) and wide (wchar_t) strings.
///
/// Encrypted globals are placed in a named section (".ollvm$str") so the
/// linker can group them and the memory layout is predictable.

#include "Obfuscation/StringEncryption.h"
#include "Obfuscation/Utils.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"

#include <string>
#include <vector>

using namespace llvm;
using namespace ollvm;

namespace {

/// Encrypt `src` bytes with a 64-bit rolling-XOR key and return the result.
static std::vector<uint8_t> encryptBytes(const std::vector<uint8_t> &src,
                                         uint64_t key) {
  std::vector<uint8_t> out(src.size());
  uint64_t k = key;
  for (size_t i = 0; i < src.size(); ++i) {
    out[i] = src[i] ^ static_cast<uint8_t>(k & 0xFF);
    k = (k >> 8) | ((k & 0xFF) << 56);
  }
  return out;
}

/// Return true if `GV` looks like a string literal: i8 or i16 array
/// ending with a zero element.
static bool isStringLiteral(const GlobalVariable *GV) {
  if (!GV->isConstant() || !GV->hasInitializer())
    return false;
  auto *CDA = dyn_cast<ConstantDataArray>(GV->getInitializer());
  if (!CDA)
    return false;
  Type *ElemTy = CDA->getType()->getElementType();
  if (!ElemTy->isIntegerTy(8) && !ElemTy->isIntegerTy(16))
    return false;
  // Check null terminator.
  unsigned numElems = CDA->getNumElements();
  if (numElems == 0)
    return false;
  uint64_t lastVal = CDA->getElementAsInteger(numElems - 1);
  return lastVal == 0;
}

/// Build a function that decrypts the given ciphertext global into a
/// stack buffer and returns a pointer to it.  The function is marked
/// always_inline so the LLVM back-end inlines it at every call site.
static Function *buildDecryptorFunction(Module &M,
                                        GlobalVariable *EncryptedGV,
                                        uint64_t Key, size_t ByteLen,
                                        Type *ElemTy) {
  LLVMContext &Ctx = M.getContext();
  Type *I8Ty = Type::getInt8Ty(Ctx);
  Type *I64Ty = Type::getInt64Ty(Ctx);

  // Function type: returns i8* (pointer to decrypted string on stack).
  FunctionType *FTy =
      FunctionType::get(PointerType::get(ElemTy, 0), /*isVarArg=*/false);
  Function *Fn = Function::Create(FTy, GlobalValue::InternalLinkage,
                                  EncryptedGV->getName() + ".decrypt", M);
  Fn->addFnAttr(Attribute::AlwaysInline);
  Fn->addFnAttr(Attribute::NoUnwind);

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", Fn);
  IRBuilder<> B(Entry);

  // alloca for the output buffer.
  size_t NumElems = ByteLen / (ElemTy->isIntegerTy(16) ? 2 : 1);
  AllocaInst *Buf = B.CreateAlloca(ElemTy, B.getInt32(NumElems), "str.buf");

  // Cast encrypted global to i8* for byte-level access.
  Value *EncPtr = ConstantExpr::getBitCast(
      EncryptedGV, PointerType::get(I8Ty, 0));

  // Emit the decrypt loop as a small unrolled sequence (< 256 bytes).
  // For longer strings, emit a counted loop.
  uint64_t K = Key;
  if (ByteLen <= 128) {
    // Unrolled version — best for kernel code (no branches in decrypt).
    Value *BufI8 = B.CreateBitCast(Buf, PointerType::get(I8Ty, 0));
    for (size_t i = 0; i < ByteLen; ++i) {
      uint8_t KeyByte = static_cast<uint8_t>(K & 0xFF);
      K = (K >> 8) | ((K & 0xFF) << 56);

      Value *SrcPtr = B.CreateGEP(I8Ty, EncPtr,
                                   ConstantInt::get(I64Ty, i), "ep");
      Value *Encrypted = B.CreateLoad(I8Ty, SrcPtr, "enc");
      Value *Decrypted = B.CreateXor(
          Encrypted, ConstantInt::get(I8Ty, KeyByte), "dec");
      Value *DstPtr = B.CreateGEP(I8Ty, BufI8,
                                   ConstantInt::get(I64Ty, i), "dp");
      B.CreateStore(Decrypted, DstPtr);
    }
  } else {
    // Loop version for longer strings.
    // Precompute key schedule (stored in a second encrypted global).
    // For simplicity we regenerate key bytes inline using the same rotation.
    // (A real implementation would store the key schedule in a .key section.)
    BasicBlock *LoopHdr = BasicBlock::Create(Ctx, "loop.hdr", Fn);
    BasicBlock *LoopBody = BasicBlock::Create(Ctx, "loop.body", Fn);
    BasicBlock *LoopExit = BasicBlock::Create(Ctx, "loop.exit", Fn);

    B.CreateBr(LoopHdr);

    IRBuilder<> BHdr(LoopHdr);
    PHINode *Idx = BHdr.CreatePHI(I64Ty, 2, "idx");
    Idx->addIncoming(ConstantInt::get(I64Ty, 0), Entry);
    Value *Cond = BHdr.CreateICmpULT(Idx,
                      ConstantInt::get(I64Ty, ByteLen), "cond");
    BHdr.CreateCondBr(Cond, LoopBody, LoopExit);

    IRBuilder<> BBody(LoopBody);
    Value *BufI8 = BBody.CreateBitCast(Buf, PointerType::get(I8Ty, 0));
    Value *SrcPtr = BBody.CreateGEP(I8Ty, EncPtr, Idx);
    Value *Encrypted = BBody.CreateLoad(I8Ty, SrcPtr, "enc");
    // Approximate rolling key: use (Key >> (idx%8)*8) & 0xFF
    Value *Shift = BBody.CreateURem(Idx, ConstantInt::get(I64Ty, 8));
    Value *Shift3 = BBody.CreateMul(Shift, ConstantInt::get(I64Ty, 8));
    Value *KeyVal = ConstantInt::get(I64Ty, Key);
    Value *KeyShifted = BBody.CreateLShr(KeyVal, Shift3);
    Value *KeyByte = BBody.CreateTrunc(KeyShifted, I8Ty, "keybyte");
    Value *Decrypted = BBody.CreateXor(Encrypted, KeyByte, "dec");
    Value *DstPtr = BBody.CreateGEP(I8Ty, BufI8, Idx);
    BBody.CreateStore(Decrypted, DstPtr);
    Value *Next = BBody.CreateAdd(Idx, ConstantInt::get(I64Ty, 1));
    Idx->addIncoming(Next, LoopBody);
    BBody.CreateBr(LoopHdr);

    IRBuilder<> BExit(LoopExit);
    BExit.CreateRet(Buf);
    return Fn;
  }
  B.CreateRet(Buf);
  return Fn;
}

} // anonymous namespace

PreservedAnalyses StringEncryptionPass::run(Module &M,
                                             ModuleAnalysisManager &) {
  LLVMContext &Ctx = M.getContext();
  Type *I8Ty = Type::getInt8Ty(Ctx);

  Xorshift64 Rng(getModuleSeed(M) ^ 0x5741AF0000000000ULL);

  // Collect all string-literal globals first so we can safely modify M.
  std::vector<GlobalVariable *> StringGlobals;
  for (auto &GV : M.globals())
    if (isStringLiteral(&GV))
      StringGlobals.push_back(&GV);

  bool Changed = false;

  for (auto *GV : StringGlobals) {
    // Skip if already processed or no users.
    if (GV->use_empty() || GV->getName().startswith(".ollvm.enc"))
      continue;

    auto *CDA = cast<ConstantDataArray>(GV->getInitializer());
    Type *ElemTy = CDA->getType()->getElementType();
    size_t ElemSize = ElemTy->isIntegerTy(16) ? 2 : 1;

    // Extract raw bytes from the constant.
    std::vector<uint8_t> PlainBytes;
    for (unsigned i = 0; i < CDA->getNumElements(); ++i) {
      uint64_t v = CDA->getElementAsInteger(i);
      for (size_t b = 0; b < ElemSize; ++b)
        PlainBytes.push_back(static_cast<uint8_t>((v >> (b * 8)) & 0xFF));
    }

    size_t ByteLen = PlainBytes.size();
    uint64_t Key = Rng.next();

    // Encrypt.
    auto EncryptedBytes = encryptBytes(PlainBytes, Key);

    // Build the encrypted global.
    ArrayType *ArrTy = ArrayType::get(I8Ty, ByteLen);
    SmallVector<Constant *, 256> Elems;
    for (uint8_t b : EncryptedBytes)
      Elems.push_back(ConstantInt::get(I8Ty, b));
    Constant *EncData = ConstantArray::get(ArrTy, Elems);

    GlobalVariable *EncGV = new GlobalVariable(
        M, ArrTy, /*isConstant=*/true, GlobalValue::InternalLinkage, EncData,
        ".ollvm.enc." + GV->getName().str());
    EncGV->setSection(".ollvm$str");
    EncGV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);

    // Build per-string decryptor function.
    Function *Decryptor =
        buildDecryptorFunction(M, EncGV, Key, ByteLen, ElemTy);

    // Replace uses of the original global with calls to the decryptor.
    SmallVector<User *, 16> Users(GV->users());
    for (User *U : Users) {
      // We only handle GetElementPtr and bitcast constant expressions /
      // instructions that appear as operands to instructions.
      if (auto *CE = dyn_cast<ConstantExpr>(U)) {
        // Expand the constant expression into instructions at each use.
        SmallVector<User *, 8> CEUsers(CE->users());
        for (User *CEU : CEUsers) {
          if (auto *Inst = dyn_cast<Instruction>(CEU)) {
            // Insert call before the instruction.
            IRBuilder<> B(Inst);
            CallInst *Call = B.CreateCall(Decryptor);
            // Rebuild the GEP / bitcast on the returned pointer.
            if (CE->getOpcode() == Instruction::GetElementPtr) {
              SmallVector<Value *, 4> Indices(CE->op_begin() + 1,
                                              CE->op_end());
              Value *NewGEP = B.CreateGEP(
                  ElemTy, Call, Indices, CE->getName());
              CE->replaceAllUsesWith(NewGEP);
            } else {
              // BitCast or other — just use the decrypted pointer directly.
              Value *Cast = B.CreateBitCast(Call, CE->getType());
              CE->replaceAllUsesWith(Cast);
            }
          }
        }
      } else if (auto *Inst = dyn_cast<Instruction>(U)) {
        IRBuilder<> B(Inst);
        CallInst *Call = B.CreateCall(Decryptor);
        Inst->replaceUsesOfWith(GV, Call);
      }
    }

    // If original global is now unused, remove it.
    if (GV->use_empty())
      GV->eraseFromParent();

    Changed = true;
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
