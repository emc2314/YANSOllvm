#ifndef LLVM_UTILS_H
#define LLVM_UTILS_H
// LLVM libs
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
// System libs
#include <map>
#include <set>
#include <sstream>
#include <stdio.h>
#include <vector>
// 常用宏定义
#define INIT_CONTEXT(F) CONTEXT = &F.getContext()
#define TYPE_I32 Type::getInt32Ty(*CONTEXT)
#define CONST_I32(V) ConstantInt::get(TYPE_I32, V, false)
#define TYPE_I64 Type::getInt64Ty(*CONTEXT)
#define CONST_I64(V) ConstantInt::get(TYPE_I64, V, false)
#define CONST(T, V) ConstantInt::get(T, V)
extern llvm::LLVMContext *CONTEXT;
// fla和bcf在混淆部分函数时会报错, 所以无法用命令行开启整体混淆
// 而且Visual Studio似乎没法把 annotate 传给LLVM, 只能用函数名控制
extern bool obf_function_name_cmd;
#define YANSO_WARN_STREAM llvm::errs()
#define YANSO_ERROR_STREAM llvm::errs()
#define YANSO_WARN_FUNCTION(PASS, F, REASON)                                   \
  do {                                                                         \
    YANSO_WARN_STREAM                                                          \
        << "yansollvm: warning: " << (PASS) << ": skip function '"           \
        << (F).getName() << "': " << (REASON) << "\n";                       \
  } while (false)
#define YANSO_ERROR_FUNCTION(PASS, F, REASON)                                  \
  do {                                                                         \
    YANSO_ERROR_STREAM                                                         \
        << "yansollvm: error: " << (PASS) << ": skip function '"             \
        << (F).getName() << "': " << (REASON) << "\n";                       \
  } while (false)
#define YANSO_WARN_MODULE(PASS, M, REASON)                                     \
  do {                                                                         \
    YANSO_WARN_STREAM                                                          \
        << "yansollvm: warning: " << (PASS) << ": skip module '"             \
        << (M).getModuleIdentifier() << "': " << (REASON) << "\n";           \
  } while (false)
#define YANSO_ERROR_MODULE(PASS, M, REASON)                                    \
  do {                                                                         \
    YANSO_ERROR_STREAM                                                         \
        << "yansollvm: error: " << (PASS) << ": skip module '"               \
        << (M).getModuleIdentifier() << "': " << (REASON) << "\n";           \
  } while (false)
#define YANSO_WARN_BLOCK(PASS, F, BB, REASON)                                  \
  do {                                                                         \
    YANSO_WARN_STREAM                                                          \
        << "yansollvm: warning: " << (PASS) << ": partial skip block '"      \
        << (BB).getName() << "' in function '" << (F).getName()               \
        << "': " << (REASON) << "\n";                                        \
  } while (false)
#define YANSO_ERROR_BLOCK(PASS, F, BB, REASON)                                 \
  do {                                                                         \
    YANSO_ERROR_STREAM                                                         \
        << "yansollvm: error: " << (PASS) << ": partial skip block '"        \
        << (BB).getName() << "' in function '" << (F).getName()               \
        << "': " << (REASON) << "\n";                                        \
  } while (false)
#define YANSO_WARN_EDGE(PASS, F, FROM, TO, REASON)                             \
  do {                                                                         \
    YANSO_WARN_STREAM                                                          \
        << "yansollvm: warning: " << (PASS) << ": degraded edge '"           \
        << (FROM).getName() << "' -> '" << (TO).getName()                    \
        << "' in function '" << (F).getName() << "': " << (REASON)           \
        << "\n";                                                             \
  } while (false)
#define YANSO_ERROR_EDGE(PASS, F, FROM, TO, REASON)                            \
  do {                                                                         \
    YANSO_ERROR_STREAM                                                         \
        << "yansollvm: error: " << (PASS) << ": degraded edge '"             \
        << (FROM).getName() << "' -> '" << (TO).getName()                    \
        << "' in function '" << (F).getName() << "': " << (REASON)           \
        << "\n";                                                             \
  } while (false)
using namespace std;
namespace llvm {

inline BasicBlock::iterator it(Instruction *I) { return I->getIterator(); }
inline BasicBlock::iterator it(Instruction &I) { return I.getIterator(); }
inline Function::iterator it(BasicBlock *BB) { return BB->getIterator(); }
inline Function::iterator it(BasicBlock &BB) { return BB.getIterator(); }

inline BasicBlock::iterator firstNonAlloca(BasicBlock &Entry) {
  BasicBlock::iterator It = Entry.begin();
  while (It != Entry.end() && isa<AllocaInst>(&*It))
    ++It;
  return It;
}

std::string
readAnnotate(Function *f); // 读取llvm.global.annotations中的annotation值
bool toObfuscate(bool flag, llvm::Function *f,
                 std::string const &attribute); // 判断是否开启混淆
void FixBasicBlockConstantExpr(BasicBlock *BB);
void FixFunctionConstantExpr(Function *Func);
string rand_str(int len);
// LLVM-MSVC有这个函数, 官方版LLVM没有 (LLVM:17.0.6 | LLVM-MSVC:3.2.6)
void LowerConstantExpr(Function &F);
} // namespace llvm
#endif // LLVM_UTILS_H