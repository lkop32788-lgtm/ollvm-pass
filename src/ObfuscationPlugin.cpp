/// ObfuscationPlugin.cpp — LLVM New Pass Manager plugin entry point
///
/// Registers all six obfuscation passes with the LLVM pipeline so they can
/// be activated via -passes="..." on the clang/opt command line.
///
/// Pipeline example for Windows kernel drivers (clang-cl):
///
///   clang-cl -O2 -target x86_64-pc-windows-msvc                    \
///            -fpass-plugin=OllvmPass.dll                            \
///            -mllvm -passes="cff,bcf,isub,strenc,cenc,ibc"         \
///            /kernel /GS- /GL- driver.c -o driver.sys
///
/// Pass names:
///   cff    — ControlFlowFlattening  (control-flow flattening)
///   bcf    — BogusControlFlow       (bogus branches)
///   isub   — InstructionSubstitution(arithmetic substitution)
///   strenc — StringEncryption       (string encryption, module pass)
///   cenc   — ConstantEncryption     (constant obfuscation)
///   ibc    — IndirectBranching      (indirect call obfuscation, module pass)
///
/// Each pass can also be enabled individually.  A convenience "ollvm-all"
/// pipeline is provided that chains all six in the recommended order:
///
///   -passes="ollvm-all"

#include "Obfuscation/BogusControlFlow.h"
#include "Obfuscation/ConstantEncryption.h"
#include "Obfuscation/ControlFlowFlattening.h"
#include "Obfuscation/IndirectBranching.h"
#include "Obfuscation/InstructionSubstitution.h"
#include "Obfuscation/StringEncryption.h"

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

using namespace llvm;
using namespace ollvm;

/// Register function-level passes with the FunctionPassManager.
static void registerFunctionPasses(FunctionPassManager &FPM,
                                   OptimizationLevel /*Level*/) {
  FPM.addPass(ControlFlowFlatteningPass());
  FPM.addPass(BogusControlFlowPass());
  FPM.addPass(InstructionSubstitutionPass());
  FPM.addPass(ConstantEncryptionPass());
}

/// Register module-level passes with the ModulePassManager.
static void registerModulePasses(ModulePassManager &MPM,
                                 OptimizationLevel /*Level*/) {
  MPM.addPass(StringEncryptionPass());
  MPM.addPass(IndirectBranchingPass());
}

// ── Plugin entry point ────────────────────────────────────────────────────────

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {
      LLVM_PLUGIN_API_VERSION, "OllvmPass", LLVM_VERSION_STRING,
      [](PassBuilder &PB) {
        // ── Register individual pass names ──────────────────────────────────

        // Function passes
        PB.registerPipelineParsingCallback(
            [](StringRef Name, FunctionPassManager &FPM,
               ArrayRef<PassBuilder::PipelineElement>) {
              if (Name == "cff") {
                FPM.addPass(ControlFlowFlatteningPass());
                return true;
              }
              if (Name == "bcf") {
                FPM.addPass(BogusControlFlowPass());
                return true;
              }
              if (Name == "isub") {
                FPM.addPass(InstructionSubstitutionPass());
                return true;
              }
              if (Name == "cenc") {
                FPM.addPass(ConstantEncryptionPass());
                return true;
              }
              return false;
            });

        // Module passes
        PB.registerPipelineParsingCallback(
            [](StringRef Name, ModulePassManager &MPM,
               ArrayRef<PassBuilder::PipelineElement>) {
              if (Name == "strenc") {
                MPM.addPass(StringEncryptionPass());
                return true;
              }
              if (Name == "ibc") {
                MPM.addPass(IndirectBranchingPass());
                return true;
              }
              // Convenience "run everything" pipeline.
              if (Name == "ollvm-all") {
                MPM.addPass(StringEncryptionPass());
                MPM.addPass(IndirectBranchingPass());
                // Wrap function passes inside a module pass adapter.
                FunctionPassManager FPM;
                FPM.addPass(ControlFlowFlatteningPass());
                FPM.addPass(BogusControlFlowPass());
                FPM.addPass(InstructionSubstitutionPass());
                FPM.addPass(ConstantEncryptionPass());
                MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
                return true;
              }
              return false;
            });

        // ── Hook into the optimizer pipeline (O2/O3 builds) ─────────────────
        PB.registerOptimizerEarlyEPCallback(
            [](ModulePassManager &MPM, OptimizationLevel Level) {
              if (Level == OptimizationLevel::O0)
                return;
              // Run string/indirect obfuscation early so later passes
              // don't undo them.
              MPM.addPass(StringEncryptionPass());
              MPM.addPass(IndirectBranchingPass());
            });

        PB.registerOptimizerLastEPCallback(
            [](ModulePassManager &MPM, OptimizationLevel Level) {
              if (Level == OptimizationLevel::O0)
                return;
              // Run per-function obfuscation last so the optimiser has
              // already simplified the IR before we obfuscate it.
              FunctionPassManager FPM;
              FPM.addPass(ControlFlowFlatteningPass());
              FPM.addPass(BogusControlFlowPass());
              FPM.addPass(InstructionSubstitutionPass());
              FPM.addPass(ConstantEncryptionPass());
              MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
            });
      }};
}
