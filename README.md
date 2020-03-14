# YANSOllvm
Yet Another Not So Obfuscated LLVM

# LLVM Version
Based on the release version [9.0.1](https://github.com/llvm/llvm-project/releases/tag/llvmorg-9.0.1). Other version might work as well, but one has to merge/rebase the X86 related code.

# Build
```
wget https://github.com/llvm/llvm-project/releases/download/llvmorg-9.0.1/llvm-9.0.1.src.tar.xz
tar xf llvm-9.0.1.src.tar.xz && cd llvm-9.0.1.src
git init
git remote add origin https://github.com/emc2314/YANSOllvm.git
git fetch
git checkout -t origin/master -f
rm -rf .git
mkdir build && cd build
cmake -DLLVM_TARGETS_TO_BUILD="X86" ..
make
```

# Usage
YANSOllvm operates on the IR level (and also X86 backend for obfCall). So first convert your source code to LLVM bytecode, e.g. ```clang -c -emit-llvm -O0 main.c -o main.bc```.

Then you can apply passes to the bytecode:

```{PATH_TO_BUILD_DIR}/bin/opt -load {PATH_TO_BUILD_DIR}/lib/LLVMObf.so -vm -merge -flattening -connect -bb2func -obfZero -obfCall main.bc -o main.obf.bc```

Notice that **the order of passes matters**. You can add llvm's own passes or apply the same obfuscate pass twice, e.g. ```{PATH_TO_BUILD_DIR}/bin/opt -load {PATH_TO_BUILD_DIR}/lib/LLVMObf.so -vm -merge -O3 -flattening -obfZero -connect -bb2func -obfZero -obfCall --view-callgraph main.bc -o main.obf.bc```.

After that, compile the output bytecode to assembly using llc:

```{PATH_TO_BUILD_DIR}/bin/llc -O3 main.obf.bc```

Finally, assemble and link the output assembly:

```clang main.obf.s -o main```

# Passes
## VM
Substitute some basic binary operators (e.g. xor, add) with functions.
## Merge
This pass merges all internal linkage functions (e.g. static function) to a single function.
## Flattening
Based on OLLVM's CFG flattening, but it seperates the internal state transfer and the switch variable using a simple hash function.
## Connect
Similar to OLLVM's bogus control flow, but totally different. It splits basic blocks and use switch to add false branches among them.
## ObfZero
Obfuscate zero constants using opaque predicts. The Flattening and Connect passes need this otherwise the almighty compiler optimizer will optimize away all false branches.
## BB2func
Extract some basic blocks from functions and make them new functions.
## ObfCall
Obfuscate all internal linkage functions calls by using randomly generated calling conventions.

# Warrant
No warrant. Only bugs. Do not use.

# License
**Partial** code of ```Flattening.cpp``` comes from the original [OLLVM](https://github.com/obfuscator-llvm/obfuscator/tree/llvm-4.0) project, which is released under the University of Illinois/NCSA Open Source License.

**Partial** code of ```ObfuscateZero.cpp``` comes from the [Quarkslab/llvm-passes](https://github.com/quarkslab/llvm-passes), which is released under the MIT License.

Besides, the X86 related code is modified directly from the [LLVM](https://github.com/llvm/llvm-project/releases/tag/llvmorg-9.0.1), which is released under the Apache-2.0 WITH LLVM-exception License.

All other files are my own work.

The whole project is released under GPLv3 which is surely compatible with all above licenses.
