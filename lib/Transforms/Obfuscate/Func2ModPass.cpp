#include "YANSOllvmCommon.h"

#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/SplitModule.h"

#include <memory>
#include <string>

using namespace llvm;

static void externalizeForFunc2Mod(GlobalValue &GV) {
  if (GV.isDeclaration())
    return;
  if (!GV.hasName())
    GV.setName("__llvmsplit_unnamed");
  if (GV.hasLocalLinkage()) {
    GV.setLinkage(GlobalValue::ExternalLinkage);
    GV.setVisibility(GlobalValue::DefaultVisibility);
  }
  if (GV.getName() != "main")
    GV.setDSOLocal(false);
}

PreservedAnalyses Func2ModPass::run(Module &M, ModuleAnalysisManager &) {
  if (!Enabled)
    return PreservedAnalyses::all();

  // func2mod is a side-effecting pass: it emits split bitcode modules.
  // Keep that behavior, but use LLVM's maintained SplitModule utility.
  // than copying the old partitioning implementation.
  for (Function &F : M)
    externalizeForFunc2Mod(F);
  for (GlobalVariable &GV : M.globals())
    externalizeForFunc2Mod(GV);
  for (GlobalAlias &GA : M.aliases())
    externalizeForFunc2Mod(GA);
  for (GlobalIFunc &GIF : M.ifuncs())
    externalizeForFunc2Mod(GIF);

  std::string Stem = sys::path::stem(M.getModuleIdentifier()).str();
  if (Stem.empty())
    Stem = "module";
  SmallString<256> OutputDir(M.getModuleIdentifier());
  sys::path::remove_filename(OutputDir);

  unsigned I = 0;
  SplitModule(M, NumOutputs, [&](std::unique_ptr<Module> MPart) {
    std::error_code EC;
    bool HasMain = false;
    for (Function &F : *MPart) {
      if (!F.isDeclaration() && F.getName() == "main") {
        HasMain = true;
        break;
      }
    }
    std::string FileName =
        Stem + (HasMain ? "_main_" : "_split_") + utostr(I++) + ".bc";
    SmallString<256> OutputPath(OutputDir);
    sys::path::append(OutputPath, FileName);
    ToolOutputFile Out(OutputPath, EC, sys::fs::OF_None);
    if (EC) {
      errs() << "func2mod: " << EC.message() << '\n';
      report_fatal_error("func2mod failed to open output file");
    }
    if (verifyModule(*MPart, &errs()))
      report_fatal_error("func2mod produced invalid module");
    WriteBitcodeToFile(*MPart, Out.os());
    Out.keep();
  });

  return PreservedAnalyses::none();
}
