#include "MergePass.h"
#include "CryptoUtils.h"
#include "Utils.h"

#include "YANSOllvmSeed.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/BlockFrequency.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <vector>

using namespace llvm;

namespace {

static cl::opt<unsigned> MergeMaxGroupSize(
    "merge-max-group-size", cl::init(8), cl::Hidden,
    cl::desc("Maximum number of functions per merge dispatcher"));

struct ParamShape {
  int NI32 = 0;
  int NI64 = 0;
  std::vector<Type *> OtherTypes;
};

struct FunctionInfo {
  Function *F = nullptr;
  ParamShape Shape;
  unsigned RetBits = 64;
  unsigned StaticCost = 0;
  unsigned CallCount = 0;
  uint64_t WeightedCallCount = 0;
  double CallWeight = 1.0;
  bool NeedsWrapper = false;
  bool InGroup = false;
  Function *Dispatcher = nullptr;
  uint32_t FuncID = 0;
  uint64_t Semantic[6] = {0, 0, 0, 0, 0, 0};
  int FirstCallOrder = std::numeric_limits<int>::max();
  int LastCallOrder = -1;
  std::vector<int> CallOrders;
};

struct MergeGroup {
  std::vector<FunctionInfo *> Members;
  ParamShape Shape;
  unsigned RetBits = 64;
};

static double finiteOr(double X, double Fallback = 0.0) {
  return std::isfinite(X) ? X : Fallback;
}

static double clampDouble(double X, double Lo, double Hi) {
  if (!std::isfinite(X))
    return Lo;
  return std::min(Hi, std::max(Lo, X));
}

static double safeLog2p1(uint64_t X) {
  return finiteOr(std::log2(static_cast<double>(X) + 1.0));
}

static double safeLog2p1(double X) {
  if (!std::isfinite(X) || X <= 0.0)
    return 0.0;
  return finiteOr(std::log2(X + 1.0));
}

static uint64_t saturatingAdd(uint64_t A, uint64_t B) {
  if (std::numeric_limits<uint64_t>::max() - A < B)
    return std::numeric_limits<uint64_t>::max();
  return A + B;
}

static bool hasSupportedLinkage(Function &F) {
  return F.hasLocalLinkage() || F.hasExternalLinkage();
}

static bool isReusableI32(Type *Ty) {
  if (Ty->isFloatTy())
    return true;
  auto *IT = dyn_cast<IntegerType>(Ty);
  return IT && IT->getBitWidth() <= 32;
}

static bool isReusableI64(Type *Ty) {
  if (isa<PointerType>(Ty) || Ty->isDoubleTy())
    return true;
  auto *IT = dyn_cast<IntegerType>(Ty);
  return IT && IT->getBitWidth() > 32 && IT->getBitWidth() <= 64;
}

static bool isReusableScalarOrPtr(Type *Ty) {
  return isReusableI32(Ty) || isReusableI64(Ty);
}

static void appendNonReusableParamTypes(Function *F,
                                        std::vector<Type *> &Types) {
  for (Type *Ty : F->getFunctionType()->params())
    if (!isReusableScalarOrPtr(Ty))
      Types.push_back(Ty);
}

static ParamShape computeParamShape(Function *F) {
  ParamShape S;
  for (Type *Ty : F->getFunctionType()->params()) {
    if (isReusableI32(Ty))
      ++S.NI32;
    else if (isReusableI64(Ty))
      ++S.NI64;
    else
      S.OtherTypes.push_back(Ty);
  }
  return S;
}

static unsigned returnBits(Type *RetTy) {
  if (auto *IT = dyn_cast<IntegerType>(RetTy))
    return std::max<unsigned>(64,
                              IT->getBitWidth() == 1 ? 8 : IT->getBitWidth());
  return 64;
}

static unsigned countInstructions(Function *F) {
  unsigned Cost = 0;
  for (BasicBlock &BB : *F)
    Cost += static_cast<unsigned>(std::distance(BB.begin(), BB.end()));
  return std::max(1u, Cost);
}

static void computeSemantic(FunctionInfo &Info) {
  for (BasicBlock &BB : *Info.F) {
    for (Instruction &I : BB) {
      if (I.isTerminator())
        ++Info.Semantic[0];
      else if (I.isBinaryOp())
        ++Info.Semantic[1];
      else if (isa<LoadInst>(I) || isa<StoreInst>(I) ||
               isa<GetElementPtrInst>(I))
        ++Info.Semantic[2];
      else if (isa<CallBase>(I))
        ++Info.Semantic[3];
      else if (isa<CastInst>(I) || isa<CmpInst>(I))
        ++Info.Semantic[4];
      else
        ++Info.Semantic[5];
    }
  }
}

static void collectDirectCalls(Function *F, std::vector<CallInst *> &Calls) {
  for (Use &U : F->uses()) {
    auto *Call = dyn_cast<CallInst>(U.getUser());
    if (Call && Call->getCalledFunction() == F)
      Calls.push_back(Call);
  }
}

static int callsiteDistance(const FunctionInfo &A, const FunctionInfo &B) {
  if (A.CallOrders.empty() || B.CallOrders.empty())
    return 0;

  int Best = std::numeric_limits<int>::max();
  for (int AO : A.CallOrders)
    for (int BO : B.CallOrders)
      Best = std::min(Best, std::abs(AO - BO));

  int Span = std::max({0, A.LastCallOrder, B.LastCallOrder}) -
             std::min(A.FirstCallOrder, B.FirstCallOrder);
  return std::min(10000, Best + Span / 4);
}

static int semanticDistance(const FunctionInfo &A, const FunctionInfo &B) {
  uint64_t Diff = 0;
  uint64_t Total = 0;
  for (unsigned I = 0; I < 6; ++I) {
    Diff += A.Semantic[I] > B.Semantic[I] ? A.Semantic[I] - B.Semantic[I]
                                          : B.Semantic[I] - A.Semantic[I];
    Total += A.Semantic[I] + B.Semantic[I];
  }
  if (Total == 0)
    return 0;
  return static_cast<int>((Diff * 100) / Total);
}

static ParamShape mergedShape(ArrayRef<FunctionInfo *> Members) {
  ParamShape S;
  int MaxI64 = 0;
  int MaxTotalReusable = 0;
  for (FunctionInfo *Info : Members) {
    MaxI64 = std::max(MaxI64, Info->Shape.NI64);
    MaxTotalReusable =
        std::max(MaxTotalReusable, Info->Shape.NI32 + Info->Shape.NI64);
    appendNonReusableParamTypes(Info->F, S.OtherTypes);
  }

  int BestI64 = MaxI64;
  int BestI32 = std::numeric_limits<int>::max();
  int BestTotal = std::numeric_limits<int>::max();
  for (int NI64 = MaxI64; NI64 <= MaxTotalReusable; ++NI64) {
    int NI32 = 0;
    for (FunctionInfo *Info : Members) {
      int SpareI64 = std::max(0, NI64 - Info->Shape.NI64);
      NI32 = std::max(NI32, Info->Shape.NI32 - SpareI64);
    }
    int Total = NI64 + NI32;
    if (Total < BestTotal || (Total == BestTotal && NI64 < BestI64)) {
      BestTotal = Total;
      BestI64 = NI64;
      BestI32 = NI32;
    }
  }

  S.NI32 = std::max(0, BestI32);
  S.NI64 = BestI64;
  return S;
}

static double dynamicSlotInflation(const ParamShape &Shape,
                                   ArrayRef<FunctionInfo *> Members) {
  double Inflation = 0.0;
  unsigned GroupSlots =
      Shape.NI32 + Shape.NI64 + static_cast<unsigned>(Shape.OtherTypes.size());
  for (FunctionInfo *Info : Members) {
    unsigned OwnSlots = Info->Shape.NI32 + Info->Shape.NI64 +
                        static_cast<unsigned>(Info->Shape.OtherTypes.size());
    unsigned ExtraSlots = GroupSlots > OwnSlots ? GroupSlots - OwnSlots : 0;
    Inflation +=
        std::max(1.0, Info->CallWeight) * static_cast<double>(ExtraSlots);
  }
  return Inflation;
}

static double shapeCost(const ParamShape &Shape) {
  return static_cast<double>(Shape.NI32 + Shape.NI64) +
         static_cast<double>(Shape.OtherTypes.size()) * 2.0;
}

static int promotedI32SlotCount(const FunctionInfo &Info,
                                const MergeGroup &Group) {
  return std::max(0, Info.Shape.NI32 - Group.Shape.NI32);
}

static double mergeScore(ArrayRef<FunctionInfo *> Group,
                         FunctionInfo *Candidate) {
  unsigned OldSize = static_cast<unsigned>(Group.size());
  unsigned NewSize = OldSize + 1;

  ParamShape BeforeShape = mergedShape(Group);
  SmallVector<FunctionInfo *, 8> After(Group.begin(), Group.end());
  After.push_back(Candidate);
  ParamShape AfterShape = mergedShape(After);

  double BeforeShapeCost = shapeCost(BeforeShape);
  double AfterShapeCost = shapeCost(AfterShape);
  double AddedShapeCost = std::max(0.0, AfterShapeCost - BeforeShapeCost);
  double AddedInflation =
      std::max(0.0, dynamicSlotInflation(AfterShape, After) -
                        dynamicSlotInflation(BeforeShape, Group));

  double GroupCallWeight = 0.0;
  for (FunctionInfo *Info : Group)
    GroupCallWeight += std::max(1.0, Info->CallWeight);
  double CandidateCallWeight = std::max(1.0, Candidate->CallWeight);
  double SwitchCost = safeLog2p1(static_cast<double>(NewSize)) *
                      (GroupCallWeight + CandidateCallWeight);
  double BodyCost = safeLog2p1(static_cast<uint64_t>(Candidate->StaticCost)) *
                    (1.0 + 0.15 * CandidateCallWeight);
  double ShapeCost = AddedShapeCost * 12.0 + safeLog2p1(AddedInflation) * 18.0;

  bool CandidateHasOther = !Candidate->Shape.OtherTypes.empty();
  bool GroupHasOther = false;
  for (FunctionInfo *Member : Group)
    GroupHasOther |= !Member->Shape.OtherTypes.empty();
  double OtherMixCost = CandidateHasOther != GroupHasOther ? 20.0 : 0.0;

  double MinDistance = std::numeric_limits<double>::infinity();
  double MinSemantic = std::numeric_limits<double>::infinity();
  for (FunctionInfo *Member : Group) {
    MinDistance =
        std::min(MinDistance,
                 static_cast<double>(callsiteDistance(*Member, *Candidate)));
    MinSemantic =
        std::min(MinSemantic,
                 static_cast<double>(semanticDistance(*Member, *Candidate)));
  }
  if (!std::isfinite(MinDistance))
    MinDistance = 0.0;
  if (!std::isfinite(MinSemantic))
    MinSemantic = 0.0;

  // Benefit grows super-linearly with group size: each added function creates
  // new pairwise ambiguity with the existing dispatcher cases. Keep the model
  // simple and bounded; max-group-size remains the hard cap.
  double AmbiguityBenefit =
      static_cast<double>(NewSize * NewSize - OldSize * OldSize) * 18.0;
  double DistanceBenefit =
      safeLog2p1(clampDouble(MinDistance, 0.0, 1000.0)) * 8.0;
  double SemanticBenefit = clampDouble(MinSemantic, 0.0, 100.0) * 1.5;
  double ReuseBenefit =
      std::max(0.0,
               BeforeShapeCost + shapeCost(Candidate->Shape) - AfterShapeCost) *
      10.0;

  double Score = AmbiguityBenefit + DistanceBenefit + SemanticBenefit +
                 ReuseBenefit - SwitchCost - BodyCost - ShapeCost -
                 OtherMixCost;
  return finiteOr(Score, -std::numeric_limits<double>::max());
}

static std::vector<MergeGroup>
planMergeGroups(std::vector<FunctionInfo> &Infos) {
  std::vector<size_t> Order(Infos.size());
  std::iota(Order.begin(), Order.end(), 0);
  std::stable_sort(Order.begin(), Order.end(), [&](size_t L, size_t R) {
    const FunctionInfo &A = Infos[L];
    const FunctionInfo &B = Infos[R];
    if (A.StaticCost != B.StaticCost)
      return A.StaticCost < B.StaticCost;
    if (A.CallWeight != B.CallWeight)
      return A.CallWeight < B.CallWeight;
    if (A.CallCount != B.CallCount)
      return A.CallCount < B.CallCount;
    return L < R;
  });

  std::vector<bool> Used(Infos.size(), false);
  std::vector<MergeGroup> Groups;
  unsigned MaxGroupSize = std::max(2u, MergeMaxGroupSize.getValue());

  for (size_t LeaderIdx : Order) {
    if (Used[LeaderIdx])
      continue;

    MergeGroup G;
    G.Members.push_back(&Infos[LeaderIdx]);
    Used[LeaderIdx] = true;

    while (G.Members.size() < MaxGroupSize) {
      double BestScore = -std::numeric_limits<double>::infinity();
      size_t BestIdx = Infos.size();
      for (size_t I : Order) {
        if (Used[I])
          continue;
        double Score = mergeScore(G.Members, &Infos[I]);
        if (Score > BestScore) {
          BestScore = Score;
          BestIdx = I;
        }
      }
      if (BestIdx == Infos.size())
        break;

      G.Members.push_back(&Infos[BestIdx]);
      Used[BestIdx] = true;
    }

    if (G.Members.size() >= 2) {
      G.Shape = mergedShape(G.Members);
      for (FunctionInfo *Info : G.Members) {
        Info->InGroup = true;
        G.RetBits = std::max(G.RetBits, Info->RetBits);
      }
      Groups.push_back(std::move(G));
      continue;
    }

    // A size-1 dispatcher is not a merge; leave the last unpaired function out.
  }

  return Groups;
}

static Value *packValueForSlot(IRBuilder<> &B, Value *V, IntegerType *SlotTy) {
  Type *Ty = V->getType();
  LLVMContext &Ctx = Ty->getContext();
  IntegerType *I32 = IntegerType::get(Ctx, 32);
  IntegerType *I64 = IntegerType::get(Ctx, 64);

  if (Ty->isFloatTy())
    V = B.CreateBitCast(V, I32);
  else if (Ty->isDoubleTy())
    V = B.CreateBitCast(V, I64);
  else if (isa<PointerType>(Ty))
    V = B.CreatePtrToInt(V, I64);

  Type *PackedTy = V->getType();
  if (PackedTy == SlotTy)
    return V;
  auto *IT = dyn_cast<IntegerType>(PackedTy);
  if (!IT)
    return V;
  if (IT->getBitWidth() < SlotTy->getBitWidth())
    return B.CreateZExt(V, SlotTy);
  if (IT->getBitWidth() > SlotTy->getBitWidth())
    return B.CreateTrunc(V, SlotTy);
  return V;
}

static Value *unpackValueFromSlot(IRBuilder<> &B, Value *V, Type *DstTy) {
  if (V->getType() == DstTy)
    return V;
  LLVMContext &Ctx = DstTy->getContext();
  IntegerType *I32 = IntegerType::get(Ctx, 32);
  IntegerType *I64 = IntegerType::get(Ctx, 64);

  if (DstTy->isFloatTy()) {
    if (V->getType() != I32)
      V = B.CreateTrunc(V, I32);
    return B.CreateBitCast(V, DstTy);
  }
  if (DstTy->isDoubleTy()) {
    if (V->getType() != I64)
      V = B.CreateZExt(V, I64);
    return B.CreateBitCast(V, DstTy);
  }
  if (DstTy->isPointerTy()) {
    if (V->getType() != I64)
      V = B.CreateZExt(V, I64);
    return B.CreateIntToPtr(V, DstTy);
  }

  auto *DstIT = dyn_cast<IntegerType>(DstTy);
  auto *SrcIT = dyn_cast<IntegerType>(V->getType());
  if (!DstIT || !SrcIT)
    return V;
  if (SrcIT->getBitWidth() < DstIT->getBitWidth())
    return B.CreateZExt(V, DstTy);
  if (SrcIT->getBitWidth() > DstIT->getBitWidth())
    return B.CreateTrunc(V, DstTy);
  return V;
}

static SmallVector<Value *, 8> buildMergedCallArgs(IRBuilder<> &B,
                                                   FunctionInfo &TargetInfo,
                                                   const MergeGroup &Group,
                                                   ArrayRef<Value *> ActualArgs,
                                                   uint64_t SelectorKey) {
  LLVMContext &Ctx = TargetInfo.F->getContext();
  IntegerType *I32 = IntegerType::get(Ctx, 32);
  IntegerType *I64 = IntegerType::get(Ctx, 64);

  SmallVector<Value *, 8> CallArgs;
  SmallVector<Value *, 4> I32Args, I64Args, OtherArgs;
  CallArgs.push_back(ConstantInt::get(I64, SelectorKey));

  int PromoteI32 = promotedI32SlotCount(TargetInfo, Group);
  int SeenI32 = 0;
  for (Value *Arg : ActualArgs) {
    Type *Ty = Arg->getType();
    if (isReusableI32(Ty)) {
      if (SeenI32++ < PromoteI32)
        I64Args.push_back(packValueForSlot(B, Arg, I64));
      else
        I32Args.push_back(packValueForSlot(B, Arg, I32));
    } else if (isReusableI64(Ty)) {
      I64Args.push_back(packValueForSlot(B, Arg, I64));
    } else {
      OtherArgs.push_back(Arg);
    }
  }

  for (int J = 0; J < Group.Shape.NI32; J++)
    CallArgs.push_back(J < (int)I32Args.size() ? I32Args[J]
                                               : Constant::getNullValue(I32));
  for (int J = 0; J < Group.Shape.NI64; J++)
    CallArgs.push_back(J < (int)I64Args.size() ? I64Args[J]
                                               : Constant::getNullValue(I64));
  for (FunctionInfo *Info : Group.Members) {
    if (Info->F != TargetInfo.F) {
      for (Type *Ty : Info->F->getFunctionType()->params())
        if (!isReusableScalarOrPtr(Ty))
          CallArgs.push_back(Constant::getNullValue(Ty));
    } else {
      CallArgs.append(OtherArgs.begin(), OtherArgs.end());
    }
  }
  return CallArgs;
}

static Value *convertMergedReturn(IRBuilder<> &B, Value *V, Type *RetTy,
                                  unsigned MergedRetBits, bool ToMergedRet) {
  if (RetTy->isVoidTy())
    return nullptr;
  if (RetTy->isPointerTy())
    return ToMergedRet ? B.CreatePtrToInt(V, IntegerType::get(RetTy->getContext(),
                                                             MergedRetBits))
                       : B.CreateIntToPtr(V, RetTy);
  auto *RetIntTy = dyn_cast<IntegerType>(RetTy);
  if (!RetIntTy)
    return V;
  if (RetIntTy->getBitWidth() < MergedRetBits) {
    Type *DstTy = ToMergedRet ? IntegerType::get(RetTy->getContext(), MergedRetBits)
                              : RetTy;
    return ToMergedRet ? B.CreateZExt(V, DstTy) : B.CreateTrunc(V, DstTy);
  }
  return V;
}

static void rewriteFunctionAsWrapper(FunctionInfo &TargetInfo,
                                     MergeGroup &Group, YansoRNG &RNG) {
  Function *F = TargetInfo.F;
  Function *Dispatcher = TargetInfo.Dispatcher;
  if (!F || !Dispatcher)
    return;

  LLVMContext &Ctx = F->getContext();

  F->deleteBody();
  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", F);
  IRBuilder<> B(Entry);

  uint32_t SelectorHi = RNG.next32();
  uint32_t SelectorLo = SelectorHi ^ TargetInfo.FuncID;
  uint64_t SelectorKey = (static_cast<uint64_t>(SelectorHi) << 32) |
                         static_cast<uint64_t>(SelectorLo);

  SmallVector<Value *, 8> ActualArgs;
  for (Argument &Arg : F->args())
    ActualArgs.push_back(&Arg);
  SmallVector<Value *, 8> CallArgs =
      buildMergedCallArgs(B, TargetInfo, Group, ActualArgs, SelectorKey);

  CallInst *Call = B.CreateCall(Dispatcher, CallArgs);
  Type *RetTy = F->getReturnType();
  if (RetTy->isVoidTy())
    B.CreateRetVoid();
  else
    B.CreateRet(convertMergedReturn(B, Call, RetTy, Group.RetBits,
                                    /*ToMergedRet=*/false));
}

static Function *emitMergeGroup(Module &M, MergeGroup &Group, YansoRNG &RNG) {
  LLVMContext &Ctx = M.getContext();
  IntegerType *I32 = IntegerType::get(Ctx, 32);
  IntegerType *I64 = IntegerType::get(Ctx, 64);

  std::string FuncName;
  std::vector<uint32_t> FuncID;
  std::vector<Type *> ParamTy;
  ParamTy.push_back(I64);
  for (int I = 0; I < Group.Shape.NI32; I++)
    ParamTy.push_back(I32);
  for (int I = 0; I < Group.Shape.NI64; I++)
    ParamTy.push_back(I64);
  for (Type *Ty : Group.Shape.OtherTypes)
    ParamTy.push_back(Ty);

  for (FunctionInfo *Info : Group.Members) {
    FuncName += std::string(Info->F->getName()) + ".";
    FuncID.push_back(RNG.next32());
  }

  IntegerType *RetTy = IntegerType::get(Ctx, Group.RetBits);
  FunctionType *FuncTy = FunctionType::get(RetTy, ParamTy, false);
  Function *NewFunction = Function::Create(FuncTy, GlobalValue::InternalLinkage,
                                           FuncName + "merge", M);
  NewFunction->addFnAttr(Attribute::NoInline);
  for (size_t I = 0; I < Group.Members.size(); ++I) {
    Group.Members[I]->Dispatcher = NewFunction;
    Group.Members[I]->FuncID = FuncID[I];
  }

  for (size_t I = 0; I < Group.Members.size(); I++) {
    FunctionInfo *TargetInfo = Group.Members[I];
    Function *Target = TargetInfo->F;
    if (TargetInfo->NeedsWrapper)
      continue;

    std::vector<CallInst *> VecCall;
    collectDirectCalls(Target, VecCall);

    for (CallInst *Call : VecCall) {
      IRBuilder<> B(Call);
      uint32_t SelectorHi = RNG.next32();
      uint32_t SelectorLo = SelectorHi ^ FuncID[I];
      uint64_t SelectorKey = (static_cast<uint64_t>(SelectorHi) << 32) |
                             static_cast<uint64_t>(SelectorLo);

      SmallVector<Value *, 8> ActualArgs(Call->args());
      SmallVector<Value *, 8> CallArgs =
          buildMergedCallArgs(B, *TargetInfo, Group, ActualArgs, SelectorKey);
      CallInst *NewCall = B.CreateCall(NewFunction, CallArgs);
      if (!Target->getReturnType()->isVoidTy())
        Call->replaceAllUsesWith(convertMergedReturn(
            B, NewCall, Target->getReturnType(), Group.RetBits,
            /*ToMergedRet=*/false));
      Call->eraseFromParent();
    }
  }

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", NewFunction);
  BasicBlock *SwitchB = BasicBlock::Create(Ctx, "switch", NewFunction);
  IRBuilder<> EntryB(Entry);
  Value *SelectorKey = &*NewFunction->arg_begin();
  Value *SelectorHi =
      EntryB.CreateLShr(SelectorKey, ConstantInt::get(I64, 32));
  Value *SelectorLo =
      EntryB.CreateAnd(SelectorKey, ConstantInt::get(I64, 0xffffffffULL));
  Value *SelectorXor = EntryB.CreateXor(SelectorHi, SelectorLo);
  Value *Selector = EntryB.CreateTrunc(SelectorXor, I32);
  EntryB.CreateBr(SwitchB);
  IRBuilder<> SwitchBld(SwitchB);
  SwitchInst *SwitchI = SwitchBld.CreateSwitch(Selector, SwitchB, 0);

  for (size_t I = 0; I < Group.Members.size(); I++) {
    Function *Target = Group.Members[I]->F;
    BasicBlock *CallFunc = BasicBlock::Create(Ctx, "", NewFunction, SwitchB);
    IRBuilder<> B(CallFunc);
    auto ItI32 = NewFunction->arg_begin();
    std::advance(ItI32, 1);
    auto ItI64 = NewFunction->arg_begin();
    std::advance(ItI64, 1 + Group.Shape.NI32);
    auto ItOther = NewFunction->arg_begin();
    std::advance(ItOther, 1 + Group.Shape.NI32 + Group.Shape.NI64);
    for (FunctionInfo *Info : Group.Members) {
      if (Info->F == Target)
        break;
      for (Type *Ty : Info->F->getFunctionType()->params())
        if (!isReusableScalarOrPtr(Ty))
          ++ItOther;
    }

    SmallVector<Value *, 8> CallArgs;
    int PromoteI32 = promotedI32SlotCount(*Group.Members[I], Group);
    int SeenI32 = 0;
    for (Argument &Arg : Target->args()) {
      Type *Ty = Arg.getType();
      if (isReusableI32(Ty)) {
        if (SeenI32++ < PromoteI32)
          CallArgs.push_back(unpackValueFromSlot(B, &*ItI64++, Ty));
        else
          CallArgs.push_back(unpackValueFromSlot(B, &*ItI32++, Ty));
      } else if (isReusableI64(Ty)) {
        CallArgs.push_back(unpackValueFromSlot(B, &*ItI64++, Ty));
      } else {
        CallArgs.push_back(&*ItOther++);
      }
    }

    CallInst *CallI = B.CreateCall(Target, CallArgs);
    if (Target->getReturnType()->isVoidTy())
      B.CreateRet(ConstantInt::get(RetTy, 0));
    else
      B.CreateRet(convertMergedReturn(B, CallI, Target->getReturnType(),
                                      Group.RetBits, /*ToMergedRet=*/true));

    SwitchI->addCase(ConstantInt::get(I32, FuncID[I]), CallFunc);
    InlineFunctionInfo IFI;
    InlineFunction(*CallI, IFI);
  }

  for (FunctionInfo *Info : Group.Members)
    if (Info->NeedsWrapper)
      rewriteFunctionAsWrapper(*Info, Group, RNG);

  return NewFunction;
}

static uint64_t blockFrequencyForCall(
    CallInst *Call, FunctionAnalysisManager &FAM,
    std::unordered_map<Function *, BlockFrequencyInfo *> &BFICache) {
  Function *Caller = Call->getFunction();
  if (!Caller)
    return 1;

  BlockFrequencyInfo *BFI = nullptr;
  auto It = BFICache.find(Caller);
  if (It != BFICache.end()) {
    BFI = It->second;
  } else {
    BFI = &FAM.getResult<BlockFrequencyAnalysis>(*Caller);
    BFICache[Caller] = BFI;
  }

  uint64_t Freq = BFI->getBlockFreq(Call->getParent()).getFrequency();
  return Freq == 0 ? 1 : Freq;
}

} // namespace

PreservedAnalyses MergePass::run(Module &M, ModuleAnalysisManager &MAM) {
  if (!Enabled)
    return PreservedAnalyses::all();

  FunctionAnalysisManager &FAM =
      MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  std::unordered_map<Function *, BlockFrequencyInfo *> BFICache;

  std::vector<FunctionInfo> Infos;
  for (Function &F : M) {
    if (F.isDeclaration()) {
      if (!F.use_empty() && !F.isIntrinsic())
        YANSO_WARN_SKIP_FUNCTION("merge", F, "declaration has no body");
      continue;
    }
    if (F.isVarArg()) {
      YANSO_WARN_SKIP_FUNCTION("merge", F,
                               "vararg functions are not supported");
      continue;
    }
    if (!(F.getReturnType()->isIntOrPtrTy() || F.getReturnType()->isVoidTy())) {
      YANSO_WARN_SKIP_FUNCTION("merge", F, "unsupported return type");
      continue;
    }
    if (F.hasAvailableExternallyLinkage()) {
      YANSO_WARN_SKIP_FUNCTION(
          "merge", F, "available_externally definitions are not emitted");
      continue;
    }
    if (F.hasComdat()) {
      YANSO_WARN_SKIP_FUNCTION("merge", F,
                               "COMDAT functions are not supported yet");
      continue;
    }
    if (!hasSupportedLinkage(F)) {
      YANSO_WARN_SKIP_FUNCTION("merge", F, "unsupported linkage");
      continue;
    }
    if (F.getName() == "main" || F.getName() == "wmain") {
      YANSO_WARN_SKIP_FUNCTION("merge", F,
                               "entry-point functions are not supported");
      continue;
    }

    std::vector<CallInst *> Calls;
    collectDirectCalls(&F, Calls);
    if (Calls.empty() && F.hasLocalLinkage()) {
      YANSO_WARN_SKIP_FUNCTION("merge", F,
                               "local function has no direct callsites");
      continue;
    }

    FunctionInfo Info;
    Info.F = &F;
    Info.NeedsWrapper = !F.hasLocalLinkage();
    Info.Shape = computeParamShape(&F);
    Info.RetBits = returnBits(F.getReturnType());
    Info.StaticCost = countInstructions(&F);
    Info.CallCount = static_cast<unsigned>(Calls.size());
    for (CallInst *Call : Calls)
      Info.WeightedCallCount = saturatingAdd(
          Info.WeightedCallCount, blockFrequencyForCall(Call, FAM, BFICache));
    Info.CallWeight = std::max(1.0, safeLog2p1(Info.WeightedCallCount));
    computeSemantic(Info);
    Infos.push_back(std::move(Info));
  }

  if (Infos.size() < 2) {
    YANSO_WARN_MODULE("merge", M,
                      "fewer than two eligible mergeable non-vararg "
                      "int/pointer/void-return function definitions");
    return PreservedAnalyses::all();
  }

  std::unordered_map<Function *, FunctionInfo *> InfoByFunction;
  for (FunctionInfo &Info : Infos)
    InfoByFunction[Info.F] = &Info;

  int Order = 0;
  for (Function &F : M) {
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (auto *Call = dyn_cast<CallInst>(&I)) {
          auto It = InfoByFunction.find(Call->getCalledFunction());
          if (It != InfoByFunction.end()) {
            FunctionInfo &Info = *It->second;
            Info.CallOrders.push_back(Order);
            Info.FirstCallOrder = std::min(Info.FirstCallOrder, Order);
            Info.LastCallOrder = std::max(Info.LastCallOrder, Order);
          }
        }
        ++Order;
      }
    }
  }

  YansoRNG RNG(yanso_module_seed(M, "merge"));

  std::vector<MergeGroup> Groups = planMergeGroups(Infos);
  for (FunctionInfo &Info : Infos)
    if (!Info.InGroup)
      YANSO_WARN_SKIP_FUNCTION(
          "merge", *Info.F,
          "not merged: left without an available merge partner");
  if (Groups.empty()) {
    YANSO_WARN_MODULE("merge", M, "no merge group with at least two functions");
    return PreservedAnalyses::all();
  }

  std::vector<Function *> OriginalFunctions;
  for (FunctionInfo &Info : Infos)
    OriginalFunctions.push_back(Info.F);

  for (MergeGroup &Group : Groups)
    emitMergeGroup(M, Group, RNG);

  for (Function *F : OriginalFunctions)
    if (F->isDefTriviallyDead())
      F->eraseFromParent();

  return PreservedAnalyses::none();
}
