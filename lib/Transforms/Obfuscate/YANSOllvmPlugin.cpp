#include "BogusControlFlow.h"
#include "Flattening.h"
#include "IndirectBranch.h"
#include "IndirectCall.h"
#include "IndirectGlobalVariable.h"
#include "SplitBasicBlock.h"
#include "StringEncryption.h"
#include "Substitution.h"
#include "Utils.h"
#include "YANSOllvmCommon.h"

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
static cl::opt<bool>
    EnableObfCon("obfCon", cl::init(false),
                 cl::desc("yansollvm split and obfuscate constants"));

namespace {
bool anyPassEnabled() {
  return EnableStringEncryption || EnableIndirectCall || EnableSplit ||
         EnableFlattening || EnableSubstitution || EnableBogusControlFlow ||
         EnableIndirectBranch || EnableIndirectGlobalVariable || EnableVM ||
         EnableMerge || EnableFunc2Mod || EnableBB2Func || EnableConnect ||
         EnableObfCon;
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
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "yanso") {
                    MPM.addPass(YANSOllvmPass());
                    return true;
                  }
                  return false;
                });
          }};
}
