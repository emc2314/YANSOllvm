// Shared yansollvm IR utilities.
#include "Utils.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;
using std::vector;

LLVMContext *CONTEXT = nullptr;
static cl::opt<bool> EnableFunctionNameControl(
    "fncmd", cl::init(false), cl::Hidden,
    cl::desc("enable legacy function-name control for ported passes"));

std::string llvm::readAnnotate(Function *f) {
  std::string Annotation;
  GlobalVariable *Annotations =
      f->getParent()->getGlobalVariable("llvm.global.annotations");
  if (!Annotations)
    return Annotation;

  auto *Array = dyn_cast<ConstantArray>(Annotations->getInitializer());
  if (!Array)
    return Annotation;

  for (unsigned I = 0; I < Array->getNumOperands(); ++I) {
    auto *Entry = dyn_cast<ConstantStruct>(Array->getOperand(I));
    if (!Entry)
      continue;

    auto *AnnotatedValue = dyn_cast<ConstantExpr>(Entry->getOperand(0));
    if (!AnnotatedValue ||
        AnnotatedValue->getOpcode() != Instruction::BitCast ||
        AnnotatedValue->getOperand(0) != f)
      continue;

    auto *AnnotationExpr = dyn_cast<ConstantExpr>(Entry->getOperand(1));
    if (!AnnotationExpr ||
        AnnotationExpr->getOpcode() != Instruction::GetElementPtr)
      continue;

    auto *AnnotationGV =
        dyn_cast<GlobalVariable>(AnnotationExpr->getOperand(0));
    if (!AnnotationGV)
      continue;

    auto *Data =
        dyn_cast<ConstantDataSequential>(AnnotationGV->getInitializer());
    if (Data && Data->isString())
      Annotation += Data->getAsString().lower() + " ";
  }
  return Annotation;
}

bool llvm::toObfuscate(bool flag, Function *f, std::string const &attribute) {
  const std::string Attr = attribute;
  const std::string NoAttr = "no" + Attr;

  if (f->isDeclaration() || f->hasAvailableExternallyLinkage())
    return false;

  const std::string Annotation = readAnnotate(f);
  if (Annotation.find(NoAttr) != std::string::npos)
    return false;
  if (Annotation.find(Attr) != std::string::npos)
    return true;

  if (EnableFunctionNameControl) {
    if (f->getName().find("_" + NoAttr + "_") != StringRef::npos)
      return false;
    if (f->getName().find("_" + Attr + "_") != StringRef::npos)
      return true;
  }

  return flag;
}

void llvm::FixFunctionConstantExpr(Function *Func) {
  for (BasicBlock &BB : *Func)
    FixBasicBlockConstantExpr(&BB);
}

void llvm::FixBasicBlockConstantExpr(BasicBlock *BB) {
  assert(!BB->empty() && "BasicBlock is empty!");
  assert(BB->getParent() && "BasicBlock must be in a Function!");

  Instruction *FunctionInsertPt =
      &*BB->getParent()->getEntryBlock().getFirstInsertionPt();
  for (Instruction &I : *BB) {
    if (isa<LandingPadInst>(I) || isa<FuncletPadInst>(I))
      continue;

    for (unsigned Op = 0; Op < I.getNumOperands(); ++Op) {
      auto *C = dyn_cast<ConstantExpr>(I.getOperand(Op));
      if (!C)
        continue;

      IRBuilder<NoFolder> IRB(&I);
      if (isa<PHINode>(I))
        IRB.SetInsertPoint(FunctionInsertPt);
      Instruction *Inst = IRB.Insert(C->getAsInstruction());
      I.setOperand(Op, Inst);
    }
  }
}

string llvm::rand_str(int len) {
  string str;
  char c = 'O';
  int idx;
  for (idx = 0; idx < len; idx++) {

    switch ((idx + len) % 3) {
    case 1:
      c = 'O';
      break;
    case 2:
      c = '0';
      break;
    default:
      c = 'o';
      break;
    }
    str.push_back(c);
  }
  return str;
}

void llvm::LowerConstantExpr(Function &F) {
  SmallPtrSet<Instruction *, 8> WorkList;

  for (inst_iterator It = inst_begin(F), E = inst_end(F); It != E; ++It) {
    Instruction *I = &*It;

    if (isa<LandingPadInst>(I) || isa<CatchPadInst>(I) ||
        isa<CatchSwitchInst>(I) || isa<CatchReturnInst>(I))
      continue;
    if (auto *II = dyn_cast<IntrinsicInst>(I)) {
      if (II->getIntrinsicID() == Intrinsic::eh_typeid_for) {
        continue;
      }
    }

    for (unsigned int i = 0; i < I->getNumOperands(); ++i) {
      if (isa<ConstantExpr>(I->getOperand(i)))
        WorkList.insert(I);
    }
  }

  while (!WorkList.empty()) {
    auto It = WorkList.begin();
    Instruction *I = *It;
    WorkList.erase(*It);

    if (PHINode *PHI = dyn_cast<PHINode>(I)) {
      for (unsigned int i = 0; i < PHI->getNumIncomingValues(); ++i) {
        Instruction *TI = PHI->getIncomingBlock(i)->getTerminator();
        if (ConstantExpr *C =
                dyn_cast<ConstantExpr>(PHI->getIncomingValue(i))) {
          Instruction *NewInst = C->getAsInstruction();
          NewInst->insertBefore(it(TI));
          PHI->setIncomingValue(i, NewInst);
          WorkList.insert(NewInst);
        }
      }
    } else {
      for (unsigned int i = 0; i < I->getNumOperands(); ++i) {
        if (ConstantExpr *C = dyn_cast<ConstantExpr>(I->getOperand(i))) {
          Instruction *NewInst = C->getAsInstruction();
          NewInst->insertBefore(it(I));
          I->setOperand(i, NewInst);
          WorkList.insert(NewInst);
        }
      }
    }
  }
}
