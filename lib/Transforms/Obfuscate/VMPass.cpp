#include "VMPass.h"

#include "CryptoUtils.h"
#include "YANSOllvmSeed.h"
#include "VMVariant.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

using namespace llvm;

namespace {
static cl::opt<unsigned> VMControlFlowVariantPermille(
    "vm-cf-variant-permille", cl::init(80), cl::Hidden,
    cl::desc("Permille probability for control-flow VM binary-op variants"));
static cl::opt<unsigned> VMForkVariantPermille(
    "vm-fork-variant-permille", cl::init(40), cl::Hidden,
    cl::desc("Permille probability for opaque-fork VM binary-op variants"));
static cl::opt<unsigned> VMDataMuxVariantPermille(
    "vm-datamux-variant-permille", cl::init(40), cl::Hidden,
    cl::desc("Permille probability for data-mux VM binary-op variants"));
static cl::opt<unsigned> VMRelationVariantPermille(
    "vm-relation-variant-permille", cl::init(80), cl::Hidden,
    cl::desc("Permille probability for relation-decorated VM binary-op variants"));

class VirtualizeImpl {
  StringMap<Function *> Cache;
  uint64_t ModuleSeed = 0;

  static constexpr StringRef Prefix = "__yansollvm_vm_";

  static void attrs(Function *F) {
    F->addFnAttr(Attribute::NoInline);
    F->addFnAttr(Attribute::OptimizeNone);
  }

  static bool isSupportedInt(Type *Ty) { return isa<IntegerType>(Ty); }

  static bool isSupportedPointer(Type *Ty) { return isa<PointerType>(Ty); }

  static bool isSupportedScalar(Type *Ty) {
    return isSupportedInt(Ty) || isSupportedPointer(Ty);
  }

  static std::string sanitizeName(StringRef Name) {
    std::string Sanitized;
    Sanitized.reserve(Name.size());
    bool LastWasUnderscore = false;

    for (char C : Name) {
      if ((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
          (C >= '0' && C <= '9')) {
        Sanitized.push_back(C);
        LastWasUnderscore = false;
      } else if (!LastWasUnderscore) {
        Sanitized.push_back('_');
        LastWasUnderscore = true;
      }
    }

    while (!Sanitized.empty() && Sanitized.front() == '_')
      Sanitized.erase(Sanitized.begin());
    while (!Sanitized.empty() && Sanitized.back() == '_')
      Sanitized.pop_back();
    return Sanitized;
  }

  static std::string sanitizedTypeName(Type *Ty) {
    std::string Name;
    raw_string_ostream OS(Name);
    Ty->print(OS);
    OS.flush();
    return sanitizeName(Name);
  }

  static std::string instructionName(unsigned Opcode) {
    return sanitizeName(Instruction::getOpcodeName(Opcode));
  }

  static std::string predicateName(CmpInst::Predicate Pred) {
    return sanitizeName(CmpInst::getPredicateName(Pred));
  }

  static std::string intrinsicName(Intrinsic::ID ID) {
    StringRef Name = Intrinsic::getBaseName(ID);
    if (Name.consume_front("llvm."))
      return sanitizeName(Name);
    return sanitizeName(Name);
  }

  static std::string typedName(StringRef Base, Type *Ty,
                               StringRef Suffix = "") {
    return (Twine(Prefix) + Base + "_" + sanitizedTypeName(Ty) + Suffix).str();
  }

  static uint64_t instructionSeed(Instruction *I, uint64_t Seed) {
    if (!I)
      return Seed;
    Function *F = I->getFunction();
    if (F)
      Seed = yanso_hash_string(F->getName(), Seed);
    unsigned BlockIndex = 0;
    if (F) {
      for (BasicBlock &BB : *F) {
        if (&BB == I->getParent())
          break;
        ++BlockIndex;
      }
    }
    unsigned InstIndex = 0;
    if (BasicBlock *BB = I->getParent()) {
      for (Instruction &Cur : *BB) {
        if (&Cur == I)
          break;
        ++InstIndex;
      }
    }
    Seed = yanso_mix64(BlockIndex + 1, Seed);
    Seed = yanso_mix64(InstIndex + 1, Seed);
    Seed = yanso_mix64(I->getOpcode() + 1, Seed);
    return Seed;
  }


  static std::string castHandlerName(StringRef Base, Type *SrcTy, Type *DstTy) {
    return (Twine(Prefix) + Base + "_" + sanitizedTypeName(SrcTy) + "_" +
            sanitizedTypeName(DstTy))
        .str();
  }

  Function *createBinaryHandler(Module &M, unsigned Opcode, IntegerType *Ty,
                                uint64_t VariantSeed) {
    std::string Name = instructionName(Opcode);
    if (Name.empty())
      return nullptr;

    VMVariantEmitter::BinaryVariant Variant =
        VMVariantEmitter::selectBinaryVariant(
            Opcode, Ty, VariantSeed, VMControlFlowVariantPermille,
            VMForkVariantPermille, VMDataMuxVariantPermille,
            VMRelationVariantPermille);
    std::string FullName = typedName(Name, Ty, VMVariantEmitter::suffix(Variant));
    Function *&F = Cache[FullName];
    if (F)
      return F;

    FunctionType *FuncTy = FunctionType::get(Ty, {Ty, Ty}, false);
    F = Function::Create(FuncTy, GlobalValue::InternalLinkage, FullName, M);
    auto It = F->arg_begin();
    Value *X = &*It++;
    Value *Y = &*It;
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);

    VMVariantEmitter::emitBinary(B, Opcode, Ty, X, Y, Variant, VariantSeed);
    attrs(F);
    return F;
  }

  Function *createICmpHandler(Module &M, CmpInst::Predicate Pred, Type *Ty,
                              uint64_t VariantSeed) {
    std::string Name = (Twine("icmp_") + predicateName(Pred)).str();
    if (Name.empty())
      return nullptr;

    VMVariantEmitter::PredicateVariant Variant =
        VMVariantEmitter::selectPredicateVariant(
            Ty, VariantSeed, VMForkVariantPermille, VMDataMuxVariantPermille);
    std::string FullName = typedName(Name, Ty, VMVariantEmitter::suffix(Variant));
    Function *&F = Cache[FullName];
    if (F)
      return F;

    IntegerType *I1 = Type::getInt1Ty(M.getContext());
    FunctionType *FuncTy = FunctionType::get(I1, {Ty, Ty}, false);
    F = Function::Create(FuncTy, GlobalValue::InternalLinkage, FullName, M);
    auto It = F->arg_begin();
    Value *X = &*It++;
    Value *Y = &*It;
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);
    VMVariantEmitter::emitICmp(B, Pred, Ty, X, Y, Variant, VariantSeed);
    attrs(F);
    return F;
  }

  Function *createIntrinsicHandler(Module &M, Intrinsic::ID ID, IntegerType *Ty,
                                   ConstantInt *ImmArg = nullptr) {
    std::string Name = intrinsicName(ID);
    if (Name.empty())
      return nullptr;

    std::string FullName = typedName(
        Name, Ty, ImmArg ? (ImmArg->isZero() ? "_0" : "_1") : "");
    Function *&F = Cache[FullName];
    if (F)
      return F;

    unsigned Arity = intrinsicDataArgCount(ID);
    if (!Arity)
      return nullptr;

    SmallVector<Type *, 4> Params(Arity, Ty);
    FunctionType *FuncTy = FunctionType::get(Ty, Params, false);
    F = Function::Create(FuncTy, GlobalValue::InternalLinkage, FullName, M);

    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);
    SmallVector<Value *, 4> Args;
    for (Argument &Arg : F->args())
      Args.push_back(&Arg);
    if (ImmArg)
      Args.push_back(ConstantInt::get(Type::getInt1Ty(M.getContext()),
                                      !ImmArg->isZero()));

    FunctionCallee Intr = Intrinsic::getOrInsertDeclaration(&M, ID, {Ty});
    B.CreateRet(B.CreateCall(Intr, Args));
    attrs(F);
    return F;
  }

  static unsigned intrinsicDataArgCount(Intrinsic::ID ID) {
    switch (ID) {
    case Intrinsic::fshl:
    case Intrinsic::fshr:
      return 3;
    case Intrinsic::bswap:
    case Intrinsic::bitreverse:
    case Intrinsic::ctpop:
    case Intrinsic::ctlz:
    case Intrinsic::cttz:
    case Intrinsic::abs:
      return 1;
    case Intrinsic::smin:
    case Intrinsic::smax:
    case Intrinsic::umin:
    case Intrinsic::umax:
      return 2;
    default:
      return 0;
    }
  }

  Function *createCastHandler(Module &M, unsigned Opcode, Type *SrcTy,
                              Type *DstTy) {
    std::string Name = instructionName(Opcode);
    if (Name.empty())
      return nullptr;

    std::string FullName = castHandlerName(Name, SrcTy, DstTy);
    Function *&F = Cache[FullName];
    if (F)
      return F;

    FunctionType *FuncTy = FunctionType::get(DstTy, {SrcTy}, false);
    F = Function::Create(FuncTy, GlobalValue::InternalLinkage, FullName, M);
    Value *X = &*F->arg_begin();
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);
    B.CreateRet(B.CreateCast(static_cast<Instruction::CastOps>(Opcode), X,
                             DstTy));
    attrs(F);
    return F;
  }

  Function *createSelectHandler(Module &M, Type *Ty, uint64_t VariantSeed) {
    VMVariantEmitter::SelectVariant Variant =
        VMVariantEmitter::selectSelectVariant(
            Ty, VariantSeed, VMForkVariantPermille, VMDataMuxVariantPermille);
    std::string FullName = typedName(instructionName(Instruction::Select), Ty,
                                     VMVariantEmitter::suffix(Variant));
    Function *&F = Cache[FullName];
    if (F)
      return F;

    Type *I1 = Type::getInt1Ty(M.getContext());
    FunctionType *FuncTy = FunctionType::get(Ty, {I1, Ty, Ty}, false);
    F = Function::Create(FuncTy, GlobalValue::InternalLinkage, FullName, M);
    auto It = F->arg_begin();
    Value *Cond = &*It++;
    Value *TrueV = &*It++;
    Value *FalseV = &*It;
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);

    VMVariantEmitter::emitSelect(B, Ty, Cond, TrueV, FalseV, Variant,
                                 VariantSeed);
    attrs(F);
    return F;
  }

  static bool isSupportedIntrinsicCall(CallInst *CI) {
    auto *RetTy = dyn_cast<IntegerType>(CI->getType());
    if (!RetTy || intrinsicName(CI->getIntrinsicID()).empty())
      return false;

    switch (CI->getIntrinsicID()) {
    case Intrinsic::fshl:
    case Intrinsic::fshr:
      return CI->arg_size() == 3 && CI->getArgOperand(0)->getType() == RetTy &&
             CI->getArgOperand(1)->getType() == RetTy &&
             CI->getArgOperand(2)->getType() == RetTy;
    case Intrinsic::bswap:
    case Intrinsic::bitreverse:
    case Intrinsic::ctpop:
      return CI->arg_size() == 1 && CI->getArgOperand(0)->getType() == RetTy;
    case Intrinsic::ctlz:
    case Intrinsic::cttz:
    case Intrinsic::abs:
      return CI->arg_size() == 2 && CI->getArgOperand(0)->getType() == RetTy &&
             isa<ConstantInt>(CI->getArgOperand(1));
    case Intrinsic::smin:
    case Intrinsic::smax:
    case Intrinsic::umin:
    case Intrinsic::umax:
      return CI->arg_size() == 2 && CI->getArgOperand(0)->getType() == RetTy &&
             CI->getArgOperand(1)->getType() == RetTy;
    default:
      return false;
    }
  }

  static bool isSupportedCast(CastInst *CI) {
    switch (CI->getOpcode()) {
    case Instruction::Trunc:
    case Instruction::ZExt:
    case Instruction::SExt:
      return isSupportedInt(CI->getSrcTy()) && isSupportedInt(CI->getDestTy());
    case Instruction::PtrToInt:
      return isSupportedPointer(CI->getSrcTy()) && isSupportedInt(CI->getDestTy());
    case Instruction::IntToPtr:
      return isSupportedInt(CI->getSrcTy()) && isSupportedPointer(CI->getDestTy());
    default:
      return false;
    }
  }

  static bool isSupportedSelect(SelectInst *SI) {
    return SI->getCondition()->getType()->isIntegerTy(1) &&
           isSupportedScalar(SI->getType()) &&
           SI->getTrueValue()->getType() == SI->getType() &&
           SI->getFalseValue()->getType() == SI->getType();
  }

  enum class HandlerKind { Binary, ICmp, Intrinsic, Cast, Select };

  struct HandlerKey {
    HandlerKind Kind;
    unsigned Opcode = 0;
    CmpInst::Predicate Predicate = CmpInst::ICMP_EQ;
    Intrinsic::ID IntrinsicID = Intrinsic::not_intrinsic;
    Type *Ty = nullptr;
    Type *SrcTy = nullptr;
    Type *DstTy = nullptr;
    ConstantInt *ImmArg = nullptr;
  };

  struct VMVariant {
    HandlerKey Key;
    unsigned Weight = 1;
  };

  struct VMMatch {
    SmallVector<Instruction *, 4> Insts;
    SmallVector<TrackingVH<Value>, 8> Args;
    Instruction *ResultInst = nullptr;
  };

  struct VMRewritePlan {
    VMMatch Match;
    SmallVector<VMVariant, 4> Variants;
  };

  static HandlerKey binaryKey(BinaryOperator *BO) {
    HandlerKey Key{HandlerKind::Binary};
    Key.Opcode = BO->getOpcode();
    Key.Ty = BO->getType();
    return Key;
  }

  static HandlerKey icmpKey(ICmpInst *ICI) {
    HandlerKey Key{HandlerKind::ICmp};
    Key.Predicate = ICI->getPredicate();
    Key.Ty = ICI->getOperand(0)->getType();
    return Key;
  }

  static HandlerKey intrinsicKey(CallInst *CI, ConstantInt *ImmArg) {
    HandlerKey Key{HandlerKind::Intrinsic};
    Key.IntrinsicID = CI->getIntrinsicID();
    Key.Ty = CI->getType();
    Key.ImmArg = ImmArg;
    return Key;
  }

  static HandlerKey castKey(CastInst *CI) {
    HandlerKey Key{HandlerKind::Cast};
    Key.Opcode = CI->getOpcode();
    Key.SrcTy = CI->getSrcTy();
    Key.DstTy = CI->getDestTy();
    return Key;
  }

  static HandlerKey selectKey(SelectInst *SI) {
    HandlerKey Key{HandlerKind::Select};
    Key.Ty = SI->getType();
    return Key;
  }

  static void addSingleInstPlan(SmallVectorImpl<VMRewritePlan> &Plans,
                                Instruction *I, HandlerKey Key,
                                ArrayRef<Value *> Args) {
    VMRewritePlan Plan;
    Plan.Match.Insts.push_back(I);
    Plan.Match.ResultInst = I;
    for (Value *Arg : Args)
      Plan.Match.Args.push_back(Arg);
    Plan.Variants.push_back({Key, 1});
    Plans.push_back(std::move(Plan));
  }

  void addBinaryPlan(SmallVectorImpl<VMRewritePlan> &Plans, BinaryOperator *BO) {
    if (!isSupportedInt(BO->getType()) ||
        !Instruction::isBinaryOp(BO->getOpcode()))
      return;
    Value *Args[] = {BO->getOperand(0), BO->getOperand(1)};
    addSingleInstPlan(Plans, BO, binaryKey(BO), Args);
  }

  void addICmpPlan(SmallVectorImpl<VMRewritePlan> &Plans, ICmpInst *ICI) {
    if (!isSupportedScalar(ICI->getOperand(0)->getType()) ||
        ICI->getOperand(0)->getType() != ICI->getOperand(1)->getType() ||
        !CmpInst::isIntPredicate(ICI->getPredicate()))
      return;
    Value *Args[] = {ICI->getOperand(0), ICI->getOperand(1)};
    addSingleInstPlan(Plans, ICI, icmpKey(ICI), Args);
  }

  void addIntrinsicPlan(SmallVectorImpl<VMRewritePlan> &Plans, CallInst *CI) {
    if (!CI->getCalledFunction() ||
        CI->getIntrinsicID() == Intrinsic::not_intrinsic ||
        !isSupportedIntrinsicCall(CI))
      return;

    ConstantInt *ImmArg = nullptr;
    if (CI->arg_size() == 2 && CI->getArgOperand(1)->getType()->isIntegerTy(1))
      ImmArg = dyn_cast<ConstantInt>(CI->getArgOperand(1));

    SmallVector<Value *, 4> Args;
    for (unsigned I = 0, E = CI->arg_size(); I != E; ++I) {
      if (ImmArg && I == E - 1)
        continue;
      Args.push_back(CI->getArgOperand(I));
    }
    addSingleInstPlan(Plans, CI, intrinsicKey(CI, ImmArg), Args);
  }

  void addCastPlan(SmallVectorImpl<VMRewritePlan> &Plans, CastInst *CI) {
    if (!isSupportedCast(CI))
      return;
    Value *Args[] = {CI->getOperand(0)};
    addSingleInstPlan(Plans, CI, castKey(CI), Args);
  }

  void addSelectPlan(SmallVectorImpl<VMRewritePlan> &Plans, SelectInst *SI) {
    if (!isSupportedSelect(SI))
      return;
    Value *Args[] = {SI->getCondition(), SI->getTrueValue(), SI->getFalseValue()};
    addSingleInstPlan(Plans, SI, selectKey(SI), Args);
  }

  class PlanCollector : public InstVisitor<PlanCollector> {
    VirtualizeImpl &Impl;
    SmallVectorImpl<VMRewritePlan> &Plans;

  public:
    PlanCollector(VirtualizeImpl &Impl, SmallVectorImpl<VMRewritePlan> &Plans)
        : Impl(Impl), Plans(Plans) {}

    void visitBinaryOperator(BinaryOperator &BO) { Impl.addBinaryPlan(Plans, &BO); }
    void visitICmpInst(ICmpInst &ICI) { Impl.addICmpPlan(Plans, &ICI); }
    void visitCallInst(CallInst &CI) { Impl.addIntrinsicPlan(Plans, &CI); }
    void visitCastInst(CastInst &CI) { Impl.addCastPlan(Plans, &CI); }
    void visitSelectInst(SelectInst &SI) { Impl.addSelectPlan(Plans, &SI); }
  };

  Function *createHandler(Module &M, const HandlerKey &Key, uint64_t VariantSeed) {
    switch (Key.Kind) {
    case HandlerKind::Binary:
      return createBinaryHandler(M, Key.Opcode, cast<IntegerType>(Key.Ty),
                                 VariantSeed);
    case HandlerKind::ICmp:
      return createICmpHandler(M, Key.Predicate, Key.Ty, VariantSeed);
    case HandlerKind::Intrinsic:
      return createIntrinsicHandler(M, Key.IntrinsicID,
                                    cast<IntegerType>(Key.Ty), Key.ImmArg);
    case HandlerKind::Cast:
      return createCastHandler(M, Key.Opcode, Key.SrcTy, Key.DstTy);
    case HandlerKind::Select:
      return createSelectHandler(M, Key.Ty, VariantSeed);
    }
    llvm_unreachable("unknown VM handler kind");
  }

  static VMVariant *selectVariant(VMRewritePlan &Plan) {
    if (Plan.Variants.empty())
      return nullptr;
    return &Plan.Variants.front();
  }

  bool rewritePlan(Module &M, VMRewritePlan &Plan) {
    if (!Plan.Match.ResultInst || Plan.Match.Insts.empty())
      return false;

    VMVariant *Variant = selectVariant(Plan);
    if (!Variant)
      return false;

    uint64_t VariantSeed = ModuleSeed;
    VariantSeed = yanso_mix64(static_cast<uint64_t>(Variant->Key.Kind) + 1,
                              VariantSeed);
    VariantSeed = yanso_mix64(Variant->Key.Opcode + 1, VariantSeed);
    VariantSeed = yanso_mix64(Variant->Key.Predicate + 1, VariantSeed);
    VariantSeed = yanso_mix64(Variant->Key.IntrinsicID + 1, VariantSeed);
    VariantSeed = instructionSeed(Plan.Match.ResultInst, VariantSeed);

    Function *Func = createHandler(M, Variant->Key, VariantSeed);
    if (!Func)
      return false;

    SmallVector<Value *, 8> Args;
    for (TrackingVH<Value> &Arg : Plan.Match.Args) {
      if (!Arg)
        return false;
      Args.push_back(Arg);
    }

    IRBuilder<> B(Plan.Match.ResultInst);
    Value *R = B.CreateCall(Func, Args);
    Plan.Match.ResultInst->replaceAllUsesWith(R);

    for (Instruction *I : reverse(Plan.Match.Insts))
      if (I->getParent())
        I->eraseFromParent();
    return true;
  }

public:
  bool run(Module &M) {
    ModuleSeed = yanso_module_seed(M, "vm");
    SmallVector<VMRewritePlan, 64> Plans;
    PlanCollector Collector(*this, Plans);
    for (Function &F : M)
      Collector.visit(F);

    bool Modified = false;
    for (VMRewritePlan &Plan : Plans)
      Modified |= rewritePlan(M, Plan);

    return Modified;
  }
};
} // namespace

PreservedAnalyses VMPass::run(Module &M, ModuleAnalysisManager &) {
  if (!Enabled)
    return PreservedAnalyses::all();
  VirtualizeImpl Impl;
  return Impl.run(M) ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
