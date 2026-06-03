#ifndef OBFUSCATION_IPOBFUSCATIONCONTEXT_H
#define OBFUSCATION_IPOBFUSCATIONCONTEXT_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include <map>
#include <set>
#include <vector>

namespace llvm {

struct IPObfuscationContext {
  bool flag;

  /* Inter-procedural obfuscation secret info of a function */
  struct IPOInfo {
    IPOInfo(AllocaInst *CallerAI, AllocaInst *CalleeAI, LoadInst *LI,
            ConstantInt *Value)
        : CallerSlot(CallerAI), CalleeSlot(CalleeAI), SecretLI(LI),
          SecretCI(Value) {}
    // Stack slot use to store caller's secret token
    AllocaInst *CallerSlot;
    // Stack slot use to store callee's secret argument
    AllocaInst *CalleeSlot;
    // Load caller secret from caller's slot or the secret argument passed by
    // caller
    LoadInst *SecretLI;
    // A random constant value
    ConstantInt *SecretCI;
  };

  std::set<Function *> LocalFunctions;
  SmallVector<IPOInfo *, 16> IPOInfoList;
  std::map<Function *, IPOInfo *> IPOInfoMap;
  std::vector<AllocaInst *> DeadSlots;

  IPObfuscationContext() : flag(false) {}
  explicit IPObfuscationContext(bool flag) : flag(flag) {}
  ~IPObfuscationContext();

  void SurveyFunction(Function &F);
  Function *InsertSecretArgument(Function *F);
  void computeCallSiteSecretArgument(Function *F);
  IPOInfo *AllocaSecretSlot(Function &F);
  const IPOInfo *getIPOInfo(Function *F);

  bool run(Module &M);
};

} // namespace llvm

#endif
