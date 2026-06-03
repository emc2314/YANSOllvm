#include "BB2FuncPass.h"
#include "BogusControlFlow.h"
#include "ConnectPass.h"
#include "Flattening.h"
#include "Func2ModPass.h"
#include "IndirectBranch.h"
#include "IndirectCall.h"
#include "IndirectGlobalVariable.h"
#include "MergePass.h"
#include "ObfConPass.h"
#include "SplitBasicBlock.h"
#include "StringEncryption.h"
#include "Substitution.h"
#include "Utils.h"
#include "VMPass.h"

#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

static cl::opt<bool> EnableSplit("split", cl::init(false),
                                 cl::desc("yansollvm basic-block splitting"));
static cl::opt<bool>
    EnableStringEncryption("sobf", cl::init(false),
                           cl::desc("yansollvm string encryption"));
static cl::opt<bool>
    EnableFlattening("fla", cl::init(false),
                     cl::desc("yansollvm control-flow flattening"));
static cl::opt<bool>
    EnableSubstitution("sub", cl::init(false),
                       cl::desc("yansollvm instruction substitution"));
static cl::opt<bool>
    EnableBogusControlFlow("bcf", cl::init(false),
                           cl::desc("yansollvm bogus control flow"));
static cl::opt<bool>
    EnableIndirectBranch("ibr", cl::init(false),
                         cl::desc("yansollvm indirect branch"));
static cl::opt<bool> EnableIndirectGlobalVariable(
    "igv", cl::init(false), cl::desc("yansollvm indirect global variable"));
static cl::opt<bool> EnableIndirectCall("icall", cl::init(false),
                                        cl::desc("yansollvm indirect call"));
static cl::opt<bool> EnableFunctionNameControl(
    "fncmd", cl::init(false),
    cl::desc("yansollvm function-name controlled obfuscation"));

static cl::opt<bool>
    EnableVM("vm", cl::init(false),
             cl::desc("yansollvm arithmetic virtualization helpers"));
static cl::opt<bool> EnableMerge("merge", cl::init(false),
                                 cl::desc("yansollvm merge static functions"));
static cl::opt<bool>
    EnableFunc2Mod("func2mod", cl::init(false),
                   cl::desc("yansollvm split module to bitcode files"));
static cl::opt<unsigned>
    Func2ModOutputs("func2mod-outputs", cl::init(3),
                    cl::desc("yansollvm func2mod output partition count"));
static cl::opt<bool>
    EnableBB2Func("bb2func", cl::init(false),
                  cl::desc("yansollvm extract basic blocks to functions"));
static cl::opt<bool>
    EnableConnect("connect", cl::init(false),
                  cl::desc("yansollvm split/connect basic blocks"));
static cl::opt<bool> EnableObfCon(
    "obfcon", cl::init(false), cl::desc("yansollvm split and obfuscate constants"));

namespace {
using PipelineElement = PassBuilder::PipelineElement;

bool anyPassEnabled() {
  return EnableStringEncryption || EnableIndirectCall || EnableSplit ||
         EnableFlattening || EnableSubstitution || EnableBogusControlFlow ||
         EnableIndirectBranch || EnableIndirectGlobalVariable || EnableVM ||
         EnableMerge || EnableFunc2Mod || EnableBB2Func || EnableConnect ||
         EnableObfCon;
}

template <typename PassT>
void addFunctionPass(ModulePassManager &MPM, PassT Pass) {
  FunctionPassManager FPM;
  FPM.addPass(std::move(Pass));
  MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
}

ModulePassManager buildModulePipeline() {
  ModulePassManager MPM;
  MPM.addPass(VMPass(EnableVM));
  MPM.addPass(MergePass(EnableMerge));
  MPM.addPass(Func2ModPass(EnableFunc2Mod, Func2ModOutputs));
  MPM.addPass(StringEncryptionPass(EnableStringEncryption));

  FunctionPassManager FPM;
  FPM.addPass(IndirectCallPass(EnableIndirectCall));
  FPM.addPass(BB2FuncPass(EnableBB2Func));
  FPM.addPass(SplitBasicBlockPass(EnableSplit));
  FPM.addPass(FlatteningPass(EnableFlattening));
  FPM.addPass(ConnectPass(EnableConnect));
  FPM.addPass(SubstitutionPass(EnableSubstitution));
  FPM.addPass(ObfConPass(EnableObfCon));
  FPM.addPass(BogusControlFlowPass(EnableBogusControlFlow));
  MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));

  MPM.addPass(IndirectBranchPass(EnableIndirectBranch));
  MPM.addPass(IndirectGlobalVariablePass(EnableIndirectGlobalVariable));
  return MPM;
}

bool addNamedPass(StringRef Name, ModulePassManager &MPM) {
  // Single-pass spelling: opt -passes=fla, sub, split, ...

  if (Name == "vm")
    MPM.addPass(VMPass(true));
  else if (Name == "merge")
    MPM.addPass(MergePass(true));
  else if (Name == "sobf")
    MPM.addPass(StringEncryptionPass(true));
  else if (Name == "icall")
    addFunctionPass(MPM, IndirectCallPass(true));
  else if (Name == "bb2func")
    addFunctionPass(MPM, BB2FuncPass(true));
  else if (Name == "split")
    addFunctionPass(MPM, SplitBasicBlockPass(true));
  else if (Name == "fla")
    addFunctionPass(MPM, FlatteningPass(true));
  else if (Name == "connect")
    addFunctionPass(MPM, ConnectPass(true));
  else if (Name == "sub")
    addFunctionPass(MPM, SubstitutionPass(true));
  else if (Name == "obfcon")
    addFunctionPass(MPM, ObfConPass(true));
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

struct YANSOllvmPass : PassInfoMixin<YANSOllvmPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {
    obf_function_name_cmd = EnableFunctionNameControl;
    if (!anyPassEnabled())
      return PreservedAnalyses::all();

    ModulePassManager MPM = buildModulePipeline();
    MPM.run(M, MAM);
    return PreservedAnalyses::none();
  }
};
} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "yansollvm", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PipelineElement>) {
                  if (Name == "yanso") {
                    MPM.addPass(YANSOllvmPass());
                    return true;
                  }
                  return addNamedPass(Name, MPM);
                });
          }};
}
