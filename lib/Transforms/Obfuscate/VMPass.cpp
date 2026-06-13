#include "VMPass.h"

#include "CryptoUtils.h"
#include "YANSOllvmSeed.h"
#include "VMVariant.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallPtrSet.h"
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

#include <map>
#include <string>
#include <tuple>

using namespace llvm;

namespace {
static cl::opt<unsigned> VMMutationVariantPermille(
    "vm-mutation-variant-permille", cl::init(500), cl::Hidden,
    cl::desc("Permille probability for VM binary-op mutation wrappers"));
static cl::opt<unsigned> VMRelationAppVariantPermille(
    "vm-relation-app-variant-permille", cl::init(500), cl::Hidden,
    cl::desc("Permille probability for relation-applied VM binary-op variants"));
static cl::opt<unsigned> VMMaxVariantsPerOp(
    "vm-max-variants-per-op", cl::init(8), cl::Hidden,
    cl::desc("Maximum VM helper-body variants per operation/type bucket; 0 means unlimited"));
static cl::opt<unsigned> VMOpMaxLen(
    "vm-op-max-len", cl::init(4), cl::Hidden,
    cl::desc("Maximum IR instruction count per VM op DAG; 1 keeps single-instruction handlers"));

class VirtualizeImpl {
  enum class HandlerKind { Binary, ICmp, Intrinsic, Cast, Select, GEP, Load, Store };

  struct VMOpNode {
    unsigned Opcode = 0;
    CmpInst::Predicate Predicate = CmpInst::ICMP_EQ;
    Intrinsic::ID IntrinsicID = Intrinsic::not_intrinsic;
    Type *Ty = nullptr;
    Type *SrcTy = nullptr;
    Type *DstTy = nullptr;
    Type *SourceElementTy = nullptr;
    Align Alignment = Align(1);
    bool InBounds = false;
    SmallVector<int, 3> Inputs;
    SmallVector<Value *, 3> Constants;
  };

  StringMap<Function *> Cache;
  std::map<std::tuple<HandlerKind, unsigned, unsigned, Intrinsic::ID,
                      CmpInst::Predicate, Type *, Type *, Type *>,
           SmallVector<uint64_t, 8>>
      VariantBuckets;
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
            Opcode, Ty, VariantSeed, VMMutationVariantPermille,
            VMRelationAppVariantPermille);
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
            Ty, VariantSeed, VMMutationVariantPermille);
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
                                   ConstantInt *ImmArg = nullptr,
                                   uint64_t VariantSeed = 0) {
    std::string Name = intrinsicName(ID);
    if (Name.empty())
      return nullptr;

    VMVariantEmitter::ScalarVariant Variant =
        VMVariantEmitter::selectIntrinsicVariant(ID, Ty, VariantSeed);
    std::string FullName = typedName(
        Name, Ty,
        (ImmArg ? (ImmArg->isZero() ? "_0" : "_1") : "") +
            VMVariantEmitter::suffix(Variant));
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

    VMVariantEmitter::emitIntrinsic(B, ID, Ty, Args, Variant, VariantSeed);
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
                              Type *DstTy, uint64_t VariantSeed) {
    std::string Name = instructionName(Opcode);
    if (Name.empty())
      return nullptr;

    VMVariantEmitter::ScalarVariant Variant =
        VMVariantEmitter::selectCastVariant(Opcode, SrcTy, DstTy, VariantSeed);
    std::string FullName = castHandlerName(Name, SrcTy, DstTy) +
                           VMVariantEmitter::suffix(Variant);
    Function *&F = Cache[FullName];
    if (F)
      return F;

    FunctionType *FuncTy = FunctionType::get(DstTy, {SrcTy}, false);
    F = Function::Create(FuncTy, GlobalValue::InternalLinkage, FullName, M);
    Value *X = &*F->arg_begin();
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);
    VMVariantEmitter::emitCast(B, Opcode, SrcTy, DstTy, X, Variant, VariantSeed);
    attrs(F);
    return F;
  }

  Function *createSelectHandler(Module &M, Type *Ty, uint64_t VariantSeed) {
    VMVariantEmitter::SelectVariant Variant =
        VMVariantEmitter::selectSelectVariant(
            Ty, VariantSeed, VMMutationVariantPermille);
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

  Function *createGEPHandler(Module &M, Type *RetTy, Type *SourceElementTy,
                             ArrayRef<Value *> IndexOperands,
                             ArrayRef<bool> IndexIsConstant,
                             ArrayRef<Type *> ParamTys, bool InBounds) {
    std::string FullName =
        (Twine(Prefix) + "gep_" + sanitizedTypeName(RetTy) + "_" +
         sanitizedTypeName(SourceElementTy) + (InBounds ? "_inbounds" : ""))
            .str();
    std::string CacheKey = FullName;
    for (Type *ParamTy : ParamTys)
      CacheKey += "#" + sanitizedTypeName(ParamTy);
    for (unsigned I = 0, E = IndexOperands.size(); I != E; ++I) {
      CacheKey += IndexIsConstant[I] ? "#c" : "#v";
      if (IndexIsConstant[I]) {
        std::string S;
        raw_string_ostream OS(S);
        IndexOperands[I]->print(OS);
        OS.flush();
        CacheKey += S;
      }
    }
    Function *&F = Cache[CacheKey];
    if (F)
      return F;

    FunctionType *FuncTy = FunctionType::get(RetTy, ParamTys, false);
    F = Function::Create(FuncTy, GlobalValue::InternalLinkage, FullName, M);
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);

    auto It = F->arg_begin();
    Value *Base = &*It++;
    SmallVector<Value *, 4> Indices;
    for (unsigned I = 0, E = IndexOperands.size(); I != E; ++I) {
      if (IndexIsConstant[I]) {
        Indices.push_back(cast<Constant>(IndexOperands[I]));
      } else {
        Indices.push_back(&*It++);
      }
    }

    Value *P = InBounds ? B.CreateInBoundsGEP(SourceElementTy, Base, Indices)
                        : B.CreateGEP(SourceElementTy, Base, Indices);
    B.CreateRet(P);
    attrs(F);
    return F;
  }

  Function *createLoadHandler(Module &M, Type *LoadedTy, Type *PtrTy,
                              Align Alignment) {
    std::string FullName =
        (Twine(Prefix) + "load_" + sanitizedTypeName(LoadedTy) + "_" +
         sanitizedTypeName(PtrTy) + "_a" + Twine(Alignment.value()))
            .str();
    Function *&F = Cache[FullName];
    if (F)
      return F;

    FunctionType *FuncTy = FunctionType::get(LoadedTy, {PtrTy}, false);
    F = Function::Create(FuncTy, GlobalValue::InternalLinkage, FullName, M);
    Value *Ptr = &*F->arg_begin();
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);
    LoadInst *L = B.CreateLoad(LoadedTy, Ptr);
    L->setAlignment(Alignment);
    B.CreateRet(L);

    // Deliberately do not attach readnone/memory(none): this handler performs
    // a real memory read.  We also drop TBAA/alias metadata instead of copying
    // it across a function boundary; losing optimization precision is safe, but
    // stale alias metadata could make later single-threaded load/store ordering
    // transforms incorrect.
    attrs(F);
    return F;
  }

  Function *createStoreHandler(Module &M, Type *StoredTy, Type *PtrTy,
                               Align Alignment) {
    std::string FullName =
        (Twine(Prefix) + "store_" + sanitizedTypeName(StoredTy) + "_" +
         sanitizedTypeName(PtrTy) + "_a" + Twine(Alignment.value()))
            .str();
    Function *&F = Cache[FullName];
    if (F)
      return F;

    FunctionType *FuncTy = FunctionType::get(Type::getVoidTy(M.getContext()),
                                             {StoredTy, PtrTy}, false);
    F = Function::Create(FuncTy, GlobalValue::InternalLinkage, FullName, M);
    auto It = F->arg_begin();
    Value *Val = &*It++;
    Value *Ptr = &*It;
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);
    StoreInst *S = B.CreateStore(Val, Ptr);
    S->setAlignment(Alignment);
    B.CreateRetVoid();

    // Store handlers are intentionally single-instruction VM ops, not DAG
    // nodes.  A store is an ordered side effect even in single-threaded code:
    // fusing it with surrounding loads/stores without MemorySSA/AA could cross
    // an aliasing access and silently change semantics.  Keep the outer call
    // conservatively side-effecting by not adding memory attributes.
    attrs(F);
    return F;
  }

  static std::string opNodeName(const VMOpNode &N) {
    if (N.IntrinsicID != Intrinsic::not_intrinsic)
      return intrinsicName(N.IntrinsicID);
    if (N.Opcode == Instruction::ICmp)
      return (Twine("icmp_") + predicateName(N.Predicate)).str();
    return instructionName(N.Opcode);
  }

  static std::string opDagPatternName(ArrayRef<VMOpNode> Nodes) {
    std::string Name;
    raw_string_ostream OS(Name);
    bool First = true;
    for (const VMOpNode &N : Nodes) {
      if (!First)
        OS << "_";
      First = false;
      OS << opNodeName(N);
    }
    OS.flush();
    return sanitizeName(Name);
  }

  std::string opDagName(Type *RetTy, ArrayRef<VMOpNode> Nodes) {
    return (Twine(Prefix) + opDagPatternName(Nodes) + "_" +
            sanitizedTypeName(RetTy))
        .str();
  }

  static constexpr int ConstantRefBase = -1000000;

  Function *createOpDagHandler(Module &M, ArrayRef<VMOpNode> Nodes,
                                 ArrayRef<Type *> ParamTys, Type *RetTy,
                                 uint64_t PatternHash, uint64_t VariantSeed) {
    std::string CacheKey = (Twine(opDagName(RetTy, Nodes)) + "#" +
                            Twine::utohexstr(PatternHash) + "#" +
                            Twine::utohexstr(VariantSeed))
                               .str();
    std::string FullName = opDagName(RetTy, Nodes);
    Function *&F = Cache[CacheKey];
    if (F)
      return F;

    FunctionType *FuncTy = FunctionType::get(RetTy, ParamTys, false);
    F = Function::Create(FuncTy, GlobalValue::InternalLinkage, FullName, M);
    BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
    IRBuilder<> B(Entry);

    SmallVector<Value *, 8> Params;
    for (Argument &Arg : F->args())
      Params.push_back(&Arg);

    SmallVector<Value *, 8> Results;
    for (const VMOpNode &N : Nodes) {
      SmallVector<Value *, 4> Ops;
      for (int Ref : N.Inputs) {
        if (Ref >= 0)
          Ops.push_back(Params[Ref]);
        else if (Ref <= ConstantRefBase)
          Ops.push_back(N.Constants[ConstantRefBase - Ref]);
        else
          Ops.push_back(Results[-Ref - 1]);
      }

      Value *R = nullptr;
      uint64_t NodeSeed = yanso_mix64(N.Opcode + 1,
                                      yanso_mix64(Results.size() + 1,
                                                  VariantSeed));
      switch (N.Opcode) {
      case Instruction::Trunc:
      case Instruction::ZExt:
      case Instruction::SExt:
      case Instruction::PtrToInt:
      case Instruction::IntToPtr: {
        VMVariantEmitter::ScalarVariant V =
            VMVariantEmitter::selectCastVariant(N.Opcode, N.SrcTy, N.DstTy,
                                                NodeSeed);
        R = VMVariantEmitter::emitCastValue(B, N.Opcode, N.SrcTy, N.DstTy,
                                            Ops[0], V, NodeSeed);
        break;
      }
      case Instruction::ICmp:
        R = VMVariantEmitter::emitICmpValue(
            B, N.Predicate, Ops[0]->getType(), Ops[0], Ops[1],
            VMVariantEmitter::selectPredicateVariant(
                Ops[0]->getType(), NodeSeed, VMMutationVariantPermille),
            NodeSeed);
        break;
      case Instruction::Select:
        R = VMVariantEmitter::emitSelectValue(
            B, N.Ty, Ops[0], Ops[1], Ops[2],
            VMVariantEmitter::selectSelectVariant(N.Ty, NodeSeed,
                                                  VMMutationVariantPermille),
            NodeSeed);
        break;
      case Instruction::GetElementPtr: {
        SmallVector<Value *, 4> Indices(Ops.begin() + 1, Ops.end());
        R = N.InBounds ? B.CreateInBoundsGEP(N.SourceElementTy, Ops[0], Indices)
                       : B.CreateGEP(N.SourceElementTy, Ops[0], Indices);
        break;
      }
      case Instruction::Load: {
        auto *L = B.CreateLoad(N.Ty, Ops[0]);
        L->setAlignment(N.Alignment);
        R = L;
        break;
      }
      case Instruction::Call: {
        auto *ITy = cast<IntegerType>(N.Ty);
        R = VMVariantEmitter::emitIntrinsicValue(
            B, N.IntrinsicID, ITy, Ops,
            VMVariantEmitter::selectIntrinsicVariant(N.IntrinsicID, ITy,
                                                     NodeSeed),
            NodeSeed);
        break;
      }
      default: {
        auto *ITy = cast<IntegerType>(N.Ty);
        R = VMVariantEmitter::emitBinaryValue(
            B, N.Opcode, ITy, Ops[0], Ops[1],
            VMVariantEmitter::selectBinaryVariant(
                N.Opcode, ITy, NodeSeed, VMMutationVariantPermille,
                VMRelationAppVariantPermille),
            NodeSeed);
        break;
      }
      }
      Results.push_back(R);
    }

    B.CreateRet(Results.back());
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

  struct HandlerKey {
    HandlerKind Kind;
    unsigned Opcode = 0;
    CmpInst::Predicate Predicate = CmpInst::ICMP_EQ;
    Intrinsic::ID IntrinsicID = Intrinsic::not_intrinsic;
    Type *Ty = nullptr;
    Type *SrcTy = nullptr;
    Type *DstTy = nullptr;
    Type *SourceElementTy = nullptr;
    Align Alignment = Align(1);
    bool InBounds = false;
    ConstantInt *ImmArg = nullptr;
    SmallVector<Value *, 4> GEPIndexOperands;
    SmallVector<bool, 4> GEPIndexIsConstant;
    SmallVector<VMOpNode, 8> OpNodes;
    SmallVector<Type *, 8> OpParamTys;
    uint64_t OpHash = 0;
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

  static HandlerKey gepKey(GetElementPtrInst *GEP) {
    HandlerKey Key{HandlerKind::GEP};
    Key.Ty = GEP->getType();
    Key.SourceElementTy = GEP->getSourceElementType();
    Key.SrcTy = GEP->getPointerOperandType();
    Key.InBounds = GEP->isInBounds();
    return Key;
  }

  static HandlerKey loadKey(LoadInst *LI) {
    HandlerKey Key{HandlerKind::Load};
    Key.Ty = LI->getType();
    Key.SrcTy = LI->getPointerOperandType();
    Key.Alignment = LI->getAlign();
    return Key;
  }

  static HandlerKey storeKey(StoreInst *SI) {
    HandlerKey Key{HandlerKind::Store};
    Key.Ty = SI->getValueOperand()->getType();
    Key.SrcTy = SI->getPointerOperandType();
    Key.Alignment = SI->getAlign();
    return Key;
  }

  static bool isSupportedGEP(GetElementPtrInst *GEP) {
    if (!isSupportedPointer(GEP->getPointerOperandType()) ||
        !isSupportedPointer(GEP->getType()))
      return false;
    for (Value *Idx : GEP->indices())
      if (!Idx->getType()->isIntegerTy())
        return false;
    return true;
  }

  static bool isSupportedLoad(LoadInst *LI) {
    return !LI->isVolatile() && !LI->isAtomic() &&
           isSupportedScalar(LI->getType()) &&
           isSupportedPointer(LI->getPointerOperandType());
  }

  static bool isSupportedStore(StoreInst *SI) {
    return !SI->isVolatile() && !SI->isAtomic() &&
           isSupportedScalar(SI->getValueOperand()->getType()) &&
           isSupportedPointer(SI->getPointerOperandType());
  }

  static HandlerKey keyForRoot(Instruction *I) {
    if (auto *BO = dyn_cast<BinaryOperator>(I))
      return binaryKey(BO);
    if (auto *ICI = dyn_cast<ICmpInst>(I))
      return icmpKey(ICI);
    if (auto *CI = dyn_cast<CastInst>(I))
      return castKey(CI);
    if (auto *SI = dyn_cast<SelectInst>(I))
      return selectKey(SI);
    if (auto *GEP = dyn_cast<GetElementPtrInst>(I))
      return gepKey(GEP);
    if (auto *LI = dyn_cast<LoadInst>(I))
      return loadKey(LI);
    if (auto *SI = dyn_cast<StoreInst>(I))
      return storeKey(SI);
    if (auto *Call = dyn_cast<CallInst>(I)) {
      ConstantInt *ImmArg = nullptr;
      if (Call->arg_size() == 2 &&
          Call->getArgOperand(1)->getType()->isIntegerTy(1))
        ImmArg = dyn_cast<ConstantInt>(Call->getArgOperand(1));
      return intrinsicKey(Call, ImmArg);
    }
    return HandlerKey{HandlerKind::Binary};
  }

  static bool isSupportedVMOpInst(Instruction *I) {
    if (auto *BO = dyn_cast<BinaryOperator>(I))
      return isSupportedInt(BO->getType()) &&
             Instruction::isBinaryOp(BO->getOpcode());
    if (auto *ICI = dyn_cast<ICmpInst>(I))
      return isSupportedScalar(ICI->getOperand(0)->getType()) &&
             ICI->getOperand(0)->getType() == ICI->getOperand(1)->getType() &&
             CmpInst::isIntPredicate(ICI->getPredicate());
    if (auto *SI = dyn_cast<SelectInst>(I))
      return isSupportedSelect(SI);
    if (auto *GEP = dyn_cast<GetElementPtrInst>(I))
      return isSupportedGEP(GEP);
    if (auto *LI = dyn_cast<LoadInst>(I))
      return isSupportedLoad(LI);
    // Stores are intentionally not supported as DAG nodes.  They are emitted
    // only as single-op side-effect handlers below; otherwise a local DAG slice
    // could cross an aliasing load/store in single-threaded code and change the
    // observable value.
    if (isa<StoreInst>(I))
      return false;
    if (auto *CI = dyn_cast<CastInst>(I))
      return isSupportedCast(CI);
    if (auto *Call = dyn_cast<CallInst>(I))
      return Call->getCalledFunction() &&
             Call->getIntrinsicID() != Intrinsic::not_intrinsic &&
             isSupportedIntrinsicCall(Call);
    return false;
  }

  static bool hasMemoryOrderingBarrierBetweenLoadAndRoot(
      ArrayRef<Instruction *> Insts, Instruction *Root) {
    SmallPtrSet<Instruction *, 8> InSlice(Insts.begin(), Insts.end());
    for (Instruction *I : Insts) {
      if (!isa<LoadInst>(I))
        continue;

      // DAG handlers are inserted at the root instruction.  If a fused load is
      // earlier than the root, the transformation moves that memory read down to
      // the call site.  Even without considering multi-threading, moving a load
      // across an aliasing store/call/other memory op can change the value it
      // observes.  Until this pass uses MemorySSA/AA, reject any slice that would
      // move a load across an intervening memory operation or side effect.
      for (Instruction *Cur = I->getNextNode(); Cur && Cur != Root;
           Cur = Cur->getNextNode()) {
        if (InSlice.contains(Cur))
          continue;
        if (Cur->mayReadOrWriteMemory() || Cur->mayHaveSideEffects())
          return true;
      }
    }
    return false;
  }

  static void collectVMOpInsts(Instruction *Root, unsigned Budget,
                                  SmallVectorImpl<Instruction *> &Nodes) {
    if (!Root || !isSupportedVMOpInst(Root) || Budget == 0)
      return;
    SmallPtrSet<Instruction *, 8> Seen;
    SmallVector<Instruction *, 8> Worklist;
    auto Add = [&](Instruction *I) {
      if (!I || Seen.contains(I) || Nodes.size() >= Budget)
        return;
      Seen.insert(I);
      Nodes.push_back(I);
      Worklist.push_back(I);
    };
    Add(Root);
    for (unsigned WI = 0; WI != Worklist.size() && Nodes.size() < Budget; ++WI) {
      Instruction *Cur = Worklist[WI];
      SmallVector<Instruction *, 4> Deps;
      for (Value *Op : Cur->operands()) {
        auto *Dep = dyn_cast<Instruction>(Op);
        if (!Dep || Dep->getParent() != Root->getParent() ||
            !isSupportedVMOpInst(Dep) || !Dep->hasOneUse() || Seen.contains(Dep))
          continue;
        Deps.push_back(Dep);
      }
      llvm::sort(Deps, [](Instruction *A, Instruction *B) { return A->comesBefore(B); });
      for (Instruction *Dep : Deps) {
        if (Nodes.size() >= Budget)
          break;
        Add(Dep);
      }
    }
    llvm::sort(Nodes, [](Instruction *A, Instruction *B) { return A->comesBefore(B); });
  }

  using HandlerBucketKey =
      std::tuple<HandlerKind, unsigned, unsigned, Intrinsic::ID,
                 CmpInst::Predicate, Type *, Type *, Type *>;

  static HandlerBucketKey bucketKey(const HandlerKey &Key) {
    return {Key.Kind,
            Key.Opcode,
            Key.OpNodes.empty()
                ? (Key.ImmArg ? (Key.ImmArg->isZero() ? 0U : 1U) : 0U)
                : static_cast<unsigned>(Key.OpHash),
            Key.IntrinsicID,
            Key.Predicate,
            Key.Ty,
            Key.SrcTy,
            Key.DstTy};
  }

  static uint64_t hashValueShape(Value *V, uint64_t H) {
    if (auto *CI = dyn_cast<ConstantInt>(V)) {
      H = yanso_mix64(0xC0FFEEULL, H);
      std::string S;
      raw_string_ostream OS(S);
      CI->getValue().print(OS, /*isSigned=*/true);
      OS.flush();
      return yanso_hash_string(S, H);
    }
    std::string S;
    raw_string_ostream OS(S);
    V->print(OS);
    OS.flush();
    return yanso_hash_string(S, yanso_mix64(0xA970ULL, H));
  }

  static uint64_t hashOpDag(ArrayRef<VMOpNode> Nodes, ArrayRef<Type *> ParamTys,
                            Type *RetTy) {
    uint64_t H = yanso_hash_string("vmop");
    H = yanso_hash_string(sanitizedTypeName(RetTy), H);
    for (Type *ParamTy : ParamTys)
      H = yanso_hash_string(sanitizedTypeName(ParamTy), H);
    for (const VMOpNode &N : Nodes) {
      H = yanso_mix64(N.Opcode + 1, H);
      H = yanso_mix64(N.Predicate + 1, H);
      H = yanso_mix64(N.IntrinsicID + 1, H);
      if (N.Ty)
        H = yanso_hash_string(sanitizedTypeName(N.Ty), H);
      if (N.SrcTy)
        H = yanso_hash_string(sanitizedTypeName(N.SrcTy), H);
      if (N.DstTy)
        H = yanso_hash_string(sanitizedTypeName(N.DstTy), H);
      if (N.SourceElementTy)
        H = yanso_hash_string(sanitizedTypeName(N.SourceElementTy), H);
      H = yanso_mix64(N.Alignment.value(), H);
      H = yanso_mix64(N.InBounds ? 2 : 1, H);
      for (int Ref : N.Inputs)
        H = yanso_mix64(static_cast<uint64_t>(Ref + 4096), H);
      for (Value *C : N.Constants)
        H = hashValueShape(C, H);
    }
    return H;
  }

  uint64_t limitVariantSeed(const HandlerKey &Key, uint64_t Seed) {
    if (VMMaxVariantsPerOp == 0)
      return Seed;

    SmallVector<uint64_t, 8> &Bucket = VariantBuckets[bucketKey(Key)];
    if (Bucket.size() < VMMaxVariantsPerOp) {
      Bucket.push_back(Seed);
      return Seed;
    }

    uint64_t Pick = yanso_mix64(Seed, 0x94d049bb133111ebULL) % Bucket.size();
    return Bucket[Pick];
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

  bool addVMOpPlan(SmallVectorImpl<VMRewritePlan> &Plans, Instruction *Root,
                      SmallPtrSetImpl<Instruction *> &Consumed) {
    if (Consumed.contains(Root) || !isSupportedVMOpInst(Root))
      return false;

    SmallVector<Instruction *, 8> Insts;
    unsigned MaxLen = std::max(1U, static_cast<unsigned>(VMOpMaxLen));
    collectVMOpInsts(Root, MaxLen, Insts);
    if (Insts.empty())
      return false;

    for (Instruction *I : Insts)
      if (Consumed.contains(I))
        return false;

    SmallPtrSet<Instruction *, 8> InSlice(Insts.begin(), Insts.end());
    if (hasMemoryOrderingBarrierBetweenLoadAndRoot(Insts, Root))
      return false;

    for (Instruction *I : Insts) {
      if (I == Root)
        continue;
      for (User *U : I->users()) {
        auto *UI = dyn_cast<Instruction>(U);
        if (!UI || !InSlice.contains(UI))
          return false;
      }
    }

    DenseMap<Value *, int> ArgIndex;
    DenseMap<Instruction *, unsigned> NodeIndex;
    SmallVector<Value *, 8> Args;
    SmallVector<Type *, 8> ParamTys;
    SmallVector<VMOpNode, 8> Nodes;
    for (unsigned NI = 0; NI != Insts.size(); ++NI)
      NodeIndex[Insts[NI]] = NI;

    for (Instruction *I : Insts) {
      VMOpNode N;
      N.Opcode = isa<ICmpInst>(I) ? Instruction::ICmp : I->getOpcode();
      N.Ty = I->getType();
      if (auto *ICI = dyn_cast<ICmpInst>(I))
        N.Predicate = ICI->getPredicate();
      if (auto *Call = dyn_cast<CallInst>(I))
        N.IntrinsicID = Call->getIntrinsicID();
      if (auto *CI = dyn_cast<CastInst>(I)) {
        N.SrcTy = CI->getSrcTy();
        N.DstTy = CI->getDestTy();
      }
      if (auto *GEP = dyn_cast<GetElementPtrInst>(I)) {
        N.SourceElementTy = GEP->getSourceElementType();
        N.InBounds = GEP->isInBounds();
      }
      if (auto *LI = dyn_cast<LoadInst>(I))
        N.Alignment = LI->getAlign();
      for (Use &U : I->operands()) {
        Value *Op = U.get();
        if (auto *Call = dyn_cast<CallInst>(I)) {
          if (Op == Call->getCalledOperand())
            continue;
        }
        if (auto *Dep = dyn_cast<Instruction>(Op)) {
          auto It = NodeIndex.find(Dep);
          if (It != NodeIndex.end()) {
            N.Inputs.push_back(-static_cast<int>(It->second) - 1);
            continue;
          }
        }
        if (isa<Constant>(Op)) {
          N.Inputs.push_back(ConstantRefBase - static_cast<int>(N.Constants.size()));
          N.Constants.push_back(Op);
          continue;
        }
        auto It = ArgIndex.find(Op);
        if (It == ArgIndex.end()) {
          int Idx = Args.size();
          ArgIndex[Op] = Idx;
          Args.push_back(Op);
          ParamTys.push_back(Op->getType());
          N.Inputs.push_back(Idx);
        } else {
          N.Inputs.push_back(It->second);
        }
      }
      Nodes.push_back(std::move(N));
    }

    HandlerKey Key = keyForRoot(Root);
    if (!Key.Ty && !Key.DstTy)
      return false;
    Key.Ty = Root->getType();
    Key.OpNodes = Nodes;
    Key.OpParamTys = ParamTys;
    Key.OpHash = hashOpDag(Key.OpNodes, Key.OpParamTys, Key.Ty);

    VMRewritePlan Plan;
    for (Instruction *I : Insts)
      Plan.Match.Insts.push_back(I);
    Plan.Match.ResultInst = Root;
    for (Value *Arg : Args)
      Plan.Match.Args.push_back(Arg);
    Plan.Variants.push_back({Key, 1});
    Plans.push_back(std::move(Plan));
    for (Instruction *I : Insts)
      Consumed.insert(I);
    return true;
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

  void addGEPPlan(SmallVectorImpl<VMRewritePlan> &Plans, GetElementPtrInst *GEP) {
    if (!isSupportedGEP(GEP))
      return;

    HandlerKey Key = gepKey(GEP);
    SmallVector<Value *, 4> Args;
    SmallVector<Value *, 4> IndexOperands;
    SmallVector<bool, 4> IndexIsConstant;
    Args.push_back(GEP->getPointerOperand());
    for (Value *Idx : GEP->indices()) {
      IndexOperands.push_back(Idx);
      bool IsConstant = isa<Constant>(Idx);
      IndexIsConstant.push_back(IsConstant);
      if (!IsConstant)
        Args.push_back(Idx);
    }
    Key.GEPIndexOperands = IndexOperands;
    Key.GEPIndexIsConstant = IndexIsConstant;
    addSingleInstPlan(Plans, GEP, Key, Args);
  }

  void addLoadPlan(SmallVectorImpl<VMRewritePlan> &Plans, LoadInst *LI) {
    if (!isSupportedLoad(LI))
      return;
    Value *Args[] = {LI->getPointerOperand()};
    addSingleInstPlan(Plans, LI, loadKey(LI), Args);
  }

  void addStorePlan(SmallVectorImpl<VMRewritePlan> &Plans, StoreInst *SI) {
    if (!isSupportedStore(SI))
      return;
    Value *Args[] = {SI->getValueOperand(), SI->getPointerOperand()};
    addSingleInstPlan(Plans, SI, storeKey(SI), Args);
  }

  class PlanCollector : public InstVisitor<PlanCollector> {
    VirtualizeImpl &Impl;
    SmallVectorImpl<VMRewritePlan> &Plans;
    SmallPtrSetImpl<Instruction *> &Consumed;

  public:
    PlanCollector(VirtualizeImpl &Impl, SmallVectorImpl<VMRewritePlan> &Plans,
                  SmallPtrSetImpl<Instruction *> &Consumed)
        : Impl(Impl), Plans(Plans), Consumed(Consumed) {}

    void visitBinaryOperator(BinaryOperator &BO) {
      if (!Consumed.contains(&BO))
        Impl.addBinaryPlan(Plans, &BO);
    }
    void visitICmpInst(ICmpInst &ICI) {
      if (!Consumed.contains(&ICI))
        Impl.addICmpPlan(Plans, &ICI);
    }
    void visitCallInst(CallInst &CI) {
      if (!Consumed.contains(&CI))
        Impl.addIntrinsicPlan(Plans, &CI);
    }
    void visitCastInst(CastInst &CI) {
      if (!Consumed.contains(&CI))
        Impl.addCastPlan(Plans, &CI);
    }
    void visitSelectInst(SelectInst &SI) {
      if (!Consumed.contains(&SI))
        Impl.addSelectPlan(Plans, &SI);
    }
    void visitGetElementPtrInst(GetElementPtrInst &GEP) {
      if (!Consumed.contains(&GEP))
        Impl.addGEPPlan(Plans, &GEP);
    }
    void visitLoadInst(LoadInst &LI) {
      if (!Consumed.contains(&LI))
        Impl.addLoadPlan(Plans, &LI);
    }
    void visitStoreInst(StoreInst &SI) {
      // Store is always a single side-effecting VM op: no DAG fusion.
      if (!Consumed.contains(&SI))
        Impl.addStorePlan(Plans, &SI);
    }
  };

  Function *createHandler(Module &M, const HandlerKey &Key, uint64_t VariantSeed) {
    if (!Key.OpNodes.empty())
      return createOpDagHandler(M, Key.OpNodes, Key.OpParamTys, Key.Ty,
                                Key.OpHash, VariantSeed);

    switch (Key.Kind) {
    case HandlerKind::Binary:
      return createBinaryHandler(M, Key.Opcode, cast<IntegerType>(Key.Ty),
                                 VariantSeed);
    case HandlerKind::ICmp:
      return createICmpHandler(M, Key.Predicate, Key.Ty, VariantSeed);
    case HandlerKind::Intrinsic:
      return createIntrinsicHandler(M, Key.IntrinsicID,
                                    cast<IntegerType>(Key.Ty), Key.ImmArg,
                                    VariantSeed);
    case HandlerKind::Cast:
      return createCastHandler(M, Key.Opcode, Key.SrcTy, Key.DstTy,
                               VariantSeed);
    case HandlerKind::Select:
      return createSelectHandler(M, Key.Ty, VariantSeed);
    case HandlerKind::GEP: {
      SmallVector<Type *, 4> ParamTys;
      ParamTys.push_back(Key.SrcTy);
      for (unsigned I = 0, E = Key.GEPIndexOperands.size(); I != E; ++I)
        if (!Key.GEPIndexIsConstant[I])
          ParamTys.push_back(Key.GEPIndexOperands[I]->getType());
      return createGEPHandler(M, Key.Ty, Key.SourceElementTy,
                              Key.GEPIndexOperands, Key.GEPIndexIsConstant,
                              ParamTys, Key.InBounds);
    }
    case HandlerKind::Load:
      return createLoadHandler(M, Key.Ty, Key.SrcTy, Key.Alignment);
    case HandlerKind::Store:
      return createStoreHandler(M, Key.Ty, Key.SrcTy, Key.Alignment);
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
    VariantSeed = limitVariantSeed(Variant->Key, VariantSeed);

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
    CallInst *Call = B.CreateCall(Func, Args);
    if (!Plan.Match.ResultInst->getType()->isVoidTy())
      Plan.Match.ResultInst->replaceAllUsesWith(Call);

    for (Instruction *I : reverse(Plan.Match.Insts))
      if (I->getParent())
        I->eraseFromParent();
    return true;
  }

public:
  bool run(Module &M) {
    ModuleSeed = yanso_module_seed(M, "vm");
    SmallVector<VMRewritePlan, 64> Plans;
    SmallPtrSet<Instruction *, 32> Consumed;
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      for (BasicBlock &BB : F) {
        SmallVector<Instruction *, 32> Roots;
        for (Instruction &I : BB)
          Roots.push_back(&I);
        for (Instruction *I : reverse(Roots))
          addVMOpPlan(Plans, I, Consumed);
      }
    }

    PlanCollector Collector(*this, Plans, Consumed);
    for (Function &F : M) {
      Collector.visit(F);
    }

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
