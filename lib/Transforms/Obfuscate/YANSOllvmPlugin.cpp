#include "BB2FuncPass.h"
#include "BogusControlFlow.h"
#include "ConnectPass.h"
#include "Flattening.h"
#include "Func2ModPass.h"
#include "IndirectBranch.h"
#include "IndirectCall.h"
#include "IndirectGlobalVariable.h"
#include "MFLAPass.h"
#include "MergePass.h"
#include "ObfConPass.h"
#include "SplitBasicBlock.h"
#include "StringEncryption.h"
#include "Substitution.h"
#include "VMPass.h"

#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

using namespace llvm;

namespace {
using PipelineElement = PassBuilder::PipelineElement;

template <typename PassT>
void addFunctionPass(ModulePassManager &MPM, PassT Pass) {
  FunctionPassManager FPM;
  FPM.addPass(std::move(Pass));
  MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
}

void addDefaultPipeline(ModulePassManager &MPM) {
  addFunctionPass(MPM, ObfConPass());
  MPM.addPass(VMPass());
  MPM.addPass(BB2FuncPass());
  MPM.addPass(MergePass());
  MPM.addPass(MFLAPass());
  MPM.addPass(BB2FuncPass());
}

bool addNamedPass(StringRef Name, ModulePassManager &MPM) {
  // Single-pass spelling: opt -passes=fla, sub, split, ...

  if (Name == "vm")
    MPM.addPass(VMPass());
  else if (Name == "merge")
    MPM.addPass(MergePass());
  else if (Name == "mfla")
    MPM.addPass(MFLAPass());
  else if (Name == "func2mod")
    MPM.addPass(Func2ModPass());
  else if (Name == "sobf")
    MPM.addPass(StringEncryptionPass(true));
  else if (Name == "icall")
    addFunctionPass(MPM, IndirectCallPass(true));
  else if (Name == "bb2func")
    MPM.addPass(BB2FuncPass());
  else if (Name == "split")
    addFunctionPass(MPM, SplitBasicBlockPass(true));
  else if (Name == "fla")
    addFunctionPass(MPM, FlatteningPass());
  else if (Name == "connect")
    addFunctionPass(MPM, ConnectPass());
  else if (Name == "sub")
    addFunctionPass(MPM, SubstitutionPass(true));
  else if (Name == "obfcon")
    addFunctionPass(MPM, ObfConPass());
  else if (Name == "bcf")
    addFunctionPass(MPM, BogusControlFlowPass(true));
  else if (Name == "ibr")
    MPM.addPass(IndirectBranchPass(true));
  else if (Name == "igv")
    MPM.addPass(IndirectGlobalVariablePass(true));
  else
    return false;

  return true;
}
} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "yansollvm", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback([](StringRef Name,
                                                  ModulePassManager &MPM,
                                                  ArrayRef<PipelineElement>) {
              if (Name == "yanso") {
                addDefaultPipeline(MPM);
                return true;
              }
              return addNamedPass(Name, MPM);
            });
          }};
}
