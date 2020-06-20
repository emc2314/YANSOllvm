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

```{PATH_TO_BUILD_DIR}/bin/opt -load {PATH_TO_BUILD_DIR}/lib/LLVMObf.so -vm -merge -bb2func -flattening -connect -obfCon -obfCall main.bc -o main.obf.bc```

Notice that **the order of passes matters**. You can use llvm's own passes or apply the same obfuscate pass twice, e.g. ```{PATH_TO_BUILD_DIR}/bin/opt -load {PATH_TO_BUILD_DIR}/lib/LLVMObf.so -vm -merge -O3 -bb2func -flattening -obfCon -connect -obfCon -obfCall main.bc -o main.obf.bc```.

After that, compile the output bytecode to assembly using llc:

```{PATH_TO_BUILD_DIR}/bin/llc -O3 --disable-block-placement main.obf.bc```

Finally, assemble and link the output assembly:

```clang main.obf.s -o main```

# Passes
Let's use the following source code as an example to obfuscate:
```c
#include <stdio.h>
 
short zero[2] = {0,1};
static short *d(void){
  return zero;
}
static short c(int x){
  if(x == 0)
    return (*(d()+1) ^ 12);
  return c(x-1)+1;
}
 
static int b(int x){
  int sum = 0;
  for(int i = 0; i < x; i++){
    sum += c(i);
  }
  return sum;
}
 
static void a(unsigned long long x){
  for(int i = 0; i < x; i++){
    int temp = b(i) + 1;
    printf("%d ", temp);
  }
}
 
int main(int argc, char *argv[]){
  int i;
  if(argc > 1){
    sscanf(argv[1], "%d", &i);
    a(i);
  }
  return 0;
}
```
## VM
Substitute some basic binary operators (e.g. xor, add) with functions.
![vm](https://user-images.githubusercontent.com/14357110/85194064-a7826780-b2fe-11ea-9430-6e0ccd5e584a.png)
## Merge
This pass merges all internal linkage functions (e.g. static function) to a single function.
![merge](https://user-images.githubusercontent.com/14357110/85194050-a3eee080-b2fe-11ea-94c4-fec41fbf01bf.png)
## Flattening
Based on OLLVM's CFG flattening, but it seperates the internal state transfer and the switch variable using a simple hash function.
![flattening](https://user-images.githubusercontent.com/14357110/85194036-9fc2c300-b2fe-11ea-9870-242f2d369d42.png)
## Connect
Similar to OLLVM's bogus control flow, but totally different. It splits basic blocks and uses switch to add false branches among them.
![connect](https://user-images.githubusercontent.com/14357110/85194034-9d606900-b2fe-11ea-99bb-a829531bd6d6.png)
IDA cannot show CFG due to some garbage code. After patching them:
![connect_patched](https://user-images.githubusercontent.com/14357110/85194035-9e919600-b2fe-11ea-8c8f-1095657c3bf7.png)
## ObfCon
Obfuscate constants using MBA. The Flattening and Connect passes will need this otherwise the almighty compiler optimizer will optimize away all false branches.
![obfCon](https://user-images.githubusercontent.com/14357110/85194058-a5b8a400-b2fe-11ea-8d8f-02ec65beeed9.png)
## BB2func
Split & extract some basic blocks and make them new functions.
![bb2func](https://user-images.githubusercontent.com/14357110/85194031-9b96a580-b2fe-11ea-942e-3dc65dc1c0b8.png)
## ObfCall
Obfuscate all internal linkage functions calls by using randomly generated calling conventions.
![obfCall](https://user-images.githubusercontent.com/14357110/85194054-a5200d80-b2fe-11ea-9ae0-634ea945ac42.png)
## Full protect
The CFG after enabling all above passes:
![full_protect](https://user-images.githubusercontent.com/14357110/85194043-a2bdb380-b2fe-11ea-9986-d8b0b6a3d363.png)

# Warrant
No warrant. Only bugs. Use at your own risk.

# License
**Partial** code of ```Flattening.cpp``` comes from the original [OLLVM](https://github.com/obfuscator-llvm/obfuscator/tree/llvm-4.0) project, which is released under the University of Illinois/NCSA Open Source License.

**Partial** code of ```ObfuscateConstant.cpp``` comes from the [Quarkslab/llvm-passes](https://github.com/quarkslab/llvm-passes), which is released under the MIT License.

Besides, the X86 related code is modified directly from the [LLVM](https://github.com/llvm/llvm-project/releases/tag/llvmorg-9.0.1), which is released under the Apache-2.0 WITH LLVM-exception License.

All other files are my own work.

The whole project is released under GPLv3 which is surely compatible with all above licenses.
