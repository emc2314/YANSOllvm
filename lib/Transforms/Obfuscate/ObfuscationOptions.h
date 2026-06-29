#pragma once
#include <llvm/Support/YAMLParser.h>
#include <set>
namespace llvm {
struct ObfuscationOptions {
  explicit ObfuscationOptions(const Twine &FileName);
  explicit ObfuscationOptions();
  bool skipFunction(const Twine &FName);
  void dump();

  bool EnableIndirectBr;
  bool EnableIndirectCall;
  bool EnableIndirectGV;
  bool EnableCFF;
  bool EnableCSE;
  bool hasFilter;

private:
  void init();
  void handleRoot(yaml::Node *n);
  bool parseOptions(const Twine &FileName);
  std::set<std::string> FunctionFilter;
};
} // namespace llvm