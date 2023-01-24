#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/Triple.h"

#include <vector>
#include <random>

using namespace llvm;

namespace {
  struct ObfCall : public ModulePass {
    static char ID;
    ObfCall() : ModulePass(ID) {}

    bool runOnModule(Module &M) override;
  };
}

char ObfCall::ID = 0;
static RegisterPass<ObfCall> X("obfCall", "Obfuscate calling convention for static functions");

bool ObfCall::runOnModule(Module &M){
  bool modified = false;
  Triple::ArchType at = Triple(M.getTargetTriple()).getArch();
  if(at == Triple::x86_64 || at == Triple::x86){
    std::random_device rd;
    std::mt19937 g(rd());
    std::uniform_int_distribution<CallingConv::ID> rand(CallingConv::OBF_CALL_START, CallingConv::OBF_CALL_END);
    for(Function &F: M){
      CallingConv::ID obfCC = rand(g);
      if(F.getLinkage() == GlobalValue::InternalLinkage && !F.isVarArg()){
        F.setCallingConv(obfCC);
        for(Use &U: F.uses()){
          if(CallBase *C = dyn_cast<CallBase>(U.getUser())){
            C->setCallingConv(obfCC);
          }
        }
        modified = true;
      }
    }
  }
  return modified;
}