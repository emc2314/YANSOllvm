#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Pass.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <queue>
#include <random>
#include <vector>

using namespace llvm;

namespace {
struct Func2Mod : public ModulePass {
  static char ID;
  Func2Mod() : ModulePass(ID) {}

  bool runOnModule(Module &M) override;

  std::vector<Function *> extractList;
  int NumOutputs = 3;
};

using ClusterMapType = DenseMap<const GlobalValue *, bool>;
using ComdatMembersType = DenseMap<const Comdat *, const GlobalValue *>;
using ClusterIDMapType = DenseMap<const GlobalValue *, unsigned>;
} // end anonymous namespace

static void addNonConstUser(ClusterMapType &GVtoClusterMap,
                            const GlobalValue *GV, const User *U) {
  assert((!isa<Constant>(U) || isa<GlobalValue>(U)) && "Bad user");

  if (const Instruction *I = dyn_cast<Instruction>(U)) {
    const GlobalValue *F = I->getParent()->getParent();
    GVtoClusterMap[F] = true;
  } else if (isa<GlobalIndirectSymbol>(U) || isa<Function>(U) ||
             isa<GlobalVariable>(U)) {
    GVtoClusterMap[cast<GlobalValue>(U)] = true;
  } else {
    llvm_unreachable("Underimplemented use case");
  }
}

static void addAllGlobalValueUsers(ClusterMapType &GVtoClusterMap,
                                   const GlobalValue *GV, const Value *V) {
  for (auto *U : V->users()) {
    SmallVector<const User *, 4> Worklist;
    Worklist.push_back(U);
    while (!Worklist.empty()) {
      const User *UU = Worklist.pop_back_val();
      // For each constant that is not a GV (a pure const) recurse.
      if (isa<Constant>(UU) && !isa<GlobalValue>(UU)) {
        Worklist.append(UU->user_begin(), UU->user_end());
        continue;
      }
      addNonConstUser(GVtoClusterMap, GV, UU);
    }
  }
}

static void findPartitions(Module *M, ClusterIDMapType &ClusterIDMap,
                           unsigned N) {
  ClusterMapType GVtoClusterMap;
  ComdatMembersType ComdatMembers;

  auto recordGVSet = [&GVtoClusterMap, &ComdatMembers](GlobalValue &GV) {
    if (GV.isDeclaration())
      return;

    if (!GV.hasName())
      GV.setName("__llvmsplit_unnamed");

    if (!GVtoClusterMap.count(&GV))
      GVtoClusterMap[&GV] = false;

    // Comdat groups must not be partitioned. For comdat groups that contain
    // locals, record all their members here so we can keep them together.
    // Comdat groups that only contain external globals are already handled by
    // the MD5-based partitioning.
    if (const Comdat *C = GV.getComdat()) {
      auto &Member = ComdatMembers[C];
      if (Member)
        GVtoClusterMap[&GV] = true;
      else
        Member = &GV;
    }

    // For aliases we should not separate them from their aliasees regardless
    // of linkage.
    if (auto *GIS = dyn_cast<GlobalIndirectSymbol>(&GV)) {
      if (const GlobalObject *Base = GIS->getBaseObject())
        GVtoClusterMap[Base] = true;
    }

    if (const Function *F = dyn_cast<Function>(&GV)) {
      for (const BasicBlock &BB : *F) {
        BlockAddress *BA = BlockAddress::lookup(&BB);
        if (!BA || !BA->isConstantUsed())
          continue;
        addAllGlobalValueUsers(GVtoClusterMap, F, BA);
      }
    }

    addAllGlobalValueUsers(GVtoClusterMap, &GV, &GV);
  };

  llvm::for_each(M->functions(), recordGVSet);
  llvm::for_each(M->globals(), recordGVSet);
  llvm::for_each(M->aliases(), recordGVSet);

  auto CompareClusters = [](const std::pair<unsigned, unsigned> &a,
                            const std::pair<unsigned, unsigned> &b) {
    if (a.second || b.second)
      return a.second > b.second;
    else
      return a.first > b.first;
  };

  std::priority_queue<std::pair<unsigned, unsigned>,
                      std::vector<std::pair<unsigned, unsigned>>,
                      decltype(CompareClusters)>
      BalancinQueue(CompareClusters);
  // Pre-populate priority queue with N slot blanks.
  for (unsigned i = 0; i < N; ++i)
    BalancinQueue.push(std::make_pair(i, 0));

  using SortType = std::pair<unsigned, const GlobalValue *>;

  SmallVector<SortType, 64> Sets;
  SmallPtrSet<const GlobalValue *, 32> Visited;

  for (auto &I : GVtoClusterMap)
    if (I.second == false)
      Sets.push_back(std::make_pair(
          isa<Function>(I.first)
              ? static_cast<const Function *>(I.first)->getInstructionCount()
              : 1,
          I.first));

  llvm::sort(Sets, [](const SortType &a, const SortType &b) {
    return a.first > b.first;
  });

  for (auto &I : Sets) {
    unsigned CurrentClusterID = BalancinQueue.top().first;
    unsigned CurrentClusterSize = BalancinQueue.top().second;
    BalancinQueue.pop();
    ClusterIDMap[I.second] = CurrentClusterID;
    CurrentClusterSize += I.first;
    BalancinQueue.push(std::make_pair(CurrentClusterID, CurrentClusterSize));
  }
}

static void externalize(GlobalValue *GV) {
  if (GV->hasLocalLinkage()) {
    GV->setLinkage(GlobalValue::ExternalLinkage);
    GV->setVisibility(GlobalValue::DefaultVisibility);
    MD5 Hasher;
    MD5::MD5Result Hash;
    Hasher.update(GV->getName());
    Hasher.final(Hash);
    SmallString<32> HexString;
    MD5::stringifyResult(Hash, HexString);
    GV->setName("?YANSOLLVM@@YAHP6AHH@ZH0@Z." + HexString);
  }

  // Unnamed entities must be named consistently between modules. setName will
  // give a distinct name to each such entity.
  if (!GV->hasName())
    GV->setName("__llvmsplit_unnamed");
  if (GV->getName() != "main")
    GV->setDSOLocal(false);
}

static void
SplitModule(Module &M, unsigned N,
            function_ref<void(std::unique_ptr<Module> MPart)> ModuleCallback) {
  for (Function &F : M)
    externalize(&F);
  for (GlobalVariable &GV : M.globals())
    externalize(&GV);
  for (GlobalAlias &GA : M.aliases())
    externalize(&GA);
  for (GlobalIFunc &GIF : M.ifuncs())
    externalize(&GIF);

  ClusterIDMapType ClusterIDMap;
  findPartitions(&M, ClusterIDMap, N);

  for (unsigned I = 0; I < N + 1; ++I) {
    ValueToValueMapTy VMap;
    std::unique_ptr<Module> MPart(
        CloneModule(M, VMap, [&](const GlobalValue *GV) {
          if (ClusterIDMap.count(GV))
            return (ClusterIDMap[GV] == I);
          else
            return I == N;
        }));
    if (I != 0)
      MPart->setModuleInlineAsm("");
    for (GlobalVariable &GV : M.globals()) {
      if (ClusterIDMap.count(&GV)) {
        static_cast<GlobalValue *>(&(*VMap[&GV]))
            ->setDLLStorageClass(I == N ? GlobalValue::DLLImportStorageClass
                                        : GlobalValue::DLLExportStorageClass);
      }
    }
    for (Function &F : M) {
      if (ClusterIDMap.count(&F)) {
        static_cast<GlobalValue *>(&(*VMap[&F]))
            ->setDLLStorageClass(I == N ? GlobalValue::DLLImportStorageClass
                                        : GlobalValue::DLLExportStorageClass);
      }
    }
    ModuleCallback(std::move(MPart));
  }
}

char Func2Mod::ID = 0;
static RegisterPass<Func2Mod> X("func2mod",
                                "Extract functions to independent modules");

bool Func2Mod::runOnModule(Module &M) {
  for (Function &F : M) {
    if (F.getLinkage() == GlobalValue::InternalLinkage) {
      extractList.push_back(&F);
    }
  }
  unsigned I = 0;
  SplitModule(M, NumOutputs, [&](std::unique_ptr<Module> MPart) {
    std::error_code EC;
    std::string fname = "_split_";
    for (Function &F : *MPart) {
      if (F.isDeclaration())
        continue;
      if (F.getName() == "main")
        fname = "_main_";
    }
    std::unique_ptr<ToolOutputFile> Out(new ToolOutputFile(
        M.getModuleIdentifier() + fname + utostr(I++), EC, sys::fs::F_None));
    if (EC) {
      errs() << EC.message() << '\n';
      exit(1);
    }

    verifyModule(*MPart);
    WriteBitcodeToFile(*MPart, Out->os());

    // Declare success.
    Out->keep();
  });
  return true;
}