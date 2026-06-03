# YANSOllvm

Yet Another Not So Obfuscated LLVM.

This branch ports YANSOllvm to LLVM 21 as an out-of-tree pass plugin. The old
LLVM 9 branch was an in-tree LLVM source overlay; this branch keeps the pass
implementation in this repository and builds it against an external LLVM 21
installation.

## Layout

- Plugin target: `yansollvm`
- Plugin artifact: `build/yansollvm.so`
- Plugin entry: `lib/Transforms/Obfuscate/YANSOllvmPlugin.cpp`
- Pass sources: `lib/Transforms/Obfuscate/`
- Tests: `tests/`
- Helper scripts: `scripts/`

The `lib/Transforms/Obfuscate/` path is retained from the original repository so
the LLVM 21 port stays visibly related to the main branch. The build is still
out-of-tree: this repository no longer vendors or overwrites LLVM source files.

## Requirements

- LLVM 21.1.8 build with `clang`, `opt`, and `llvm-lit`
- CMake 3.20+
- Ninja

Set `LLVM_BUILD` to the LLVM build directory. It must contain
`lib/cmake/llvm` and `bin/clang`.

```bash
export LLVM_BUILD=/path/to/llvm-project/build
```

## Build and test

```bash
cmake -S . -B build -G Ninja -DLLVM_DIR="$LLVM_BUILD/lib/cmake/llvm"
ninja -C build check-yansollvm
```

Equivalent helper:

```bash
LLVM_BUILD=/path/to/llvm-project/build scripts/build_and_smoke.sh
```

The lit tests use the plugin built at `build/yansollvm.so`. Temporary lit output
is ignored under `tests/Output/`.

## Usage

Compile source to LLVM IR:

```bash
"$LLVM_BUILD/bin/clang" -O0 -Xclang -disable-O0-optnone \
  -emit-llvm -S input.c -o input.ll
```

Run the plugin through the LLVM new pass manager:

```bash
"$LLVM_BUILD/bin/opt" \
  -load-pass-plugin build/yansollvm.so \
  -passes=yanso \
  -fla -sub -split -bcf \
  -S input.ll -o input.obf.ll
```

Compile the transformed IR:

```bash
"$LLVM_BUILD/bin/clang" input.obf.ll -o input.obf
```

## Pass flags

The LLVM 21 branch exposes a single new-pass-manager plugin pipeline:
`-passes=yanso`. Individual obfuscation features are enabled by plugin-local
LLVM command-line options such as `-vm`, `-fla`, or `-sobf`.

This differs from the LLVM 9 main branch, where the original YANSOllvm passes
were legacy-PM passes registered directly by `RegisterPass` and invoked as
separate `opt -load LLVMObf.so -vm -merge ...` pass names.

### Current LLVM 21 pass list

| Flag | Current registration / invocation | Implementation origin | Status | Notes |
| --- | --- | --- | --- | --- |
| `-vm` | `cl::opt` flag consumed by `-passes=yanso`; runs as a module pass inside the plugin pipeline. | Original YANSOllvm pass. | updated | Replaces selected integer binary operators with helper calls. LLVM 9 used direct legacy-PM `RegisterPass("vm")`. |
| `-merge` | `cl::opt` flag consumed by `-passes=yanso`; runs as a module pass inside the plugin pipeline. | Original YANSOllvm pass. | updated | Merges eligible internal functions behind a dispatcher. LLVM 9 used direct legacy-PM `RegisterPass("merge")`. |
| `-func2mod` | `cl::opt` flag consumed by `-passes=yanso`; runs as a module pass and writes side-effect bitcode outputs. | Original YANSOllvm pass. | updated / experimental | Module partitioning tool, not a normal protection pass. LLVM 9 had a direct legacy-PM `RegisterPass("func2mod")`. |
| `-bb2func` | `cl::opt` flag consumed by `-passes=yanso`; runs through the plugin's function-pass adaptor. | Original YANSOllvm pass. | updated | Extracts eligible basic blocks into new functions. LLVM 9 used direct legacy-PM `RegisterPass("bb2func")`. |
| `-connect` | `cl::opt` flag consumed by `-passes=yanso`; runs through the plugin's function-pass adaptor. | Original YANSOllvm pass. | updated | Splits/connects basic blocks and adds trap-backed default paths. LLVM 9 used direct legacy-PM `RegisterPass("connect")`. |
| `-obfcon` | `cl::opt` flag consumed by `-passes=yanso`; runs through the plugin's function-pass adaptor. | Original YANSOllvm pass. | updated | Splits and obfuscates integer constants. |
| `-fla` | `cl::opt` flag consumed by `-passes=yanso`; runs through the plugin's function-pass adaptor. | Original YANSOllvm flattening pass, updated for the LLVM 21 pipeline. | updated | Replaces the old user spelling `-flattening`; now handles native `switch` terminators without requiring a `LowerSwitch` pre-pass. |
| `-split` | `cl::opt` flag consumed by `-passes=yanso`; runs through the plugin's function-pass adaptor. | Imported obfuscation pass family; comments/header lineage point to Naville/OLLVM-style code. | ported | Basic-block splitting. The LLVM 17 branch wired it by editing LLVM's `PassBuilder` start-extension callback. |
| `-sub` | `cl::opt` flag consumed by `-passes=yanso`; runs through the plugin's function-pass adaptor. | Imported OLLVM-style operator substitution pass. | ported | Instruction substitution. The LLVM 17 branch wired it by editing LLVM's `PassBuilder` start-extension callback. |
| `-bcf` | `cl::opt` flag consumed by `-passes=yanso`; runs through the plugin's function-pass adaptor. | Imported OLLVM bogus-control-flow lineage with Naville modifications. | ported | Bogus control flow. The LLVM 17 branch wired it by editing LLVM's `PassBuilder` start-extension callback. |
| `-icall` | `cl::opt` flag consumed by `-passes=yanso`; runs through the plugin's function-pass adaptor. | Imported Goron/Hikari-style indirect-call family. | ported | Indirect call. The LLVM 17 branch wired it by editing LLVM's `PassBuilder` start-extension callback. |
| `-sobf` | `cl::opt` flag consumed by `-passes=yanso`; runs as a module pass inside the plugin pipeline. | Imported Goron/Hikari-style string-encryption family. | ported | String encryption. The LLVM 17 branch wired it by editing LLVM's `PassBuilder` start-extension callback. |
| `-ibr` | `cl::opt` flag consumed by `-passes=yanso`; runs as a module pass after the function-pass adaptor. | Imported Goron/Hikari-style indirect-branch family; headers also carry Naville lineage. | ported | Indirect branch. The LLVM 17 branch wired it by editing LLVM's `PassBuilder` start-extension callback. |
| `-igv` | `cl::opt` flag consumed by `-passes=yanso`; runs as a module pass after the function-pass adaptor. | Imported Goron/Hikari-style indirect-global-variable family; headers also carry Naville lineage. | ported | Indirect global variable. The LLVM 17 branch wired it by editing LLVM's `PassBuilder` start-extension callback. |
| `-fncmd` | `cl::opt` flag consumed by `-passes=yanso`; controls `toObfuscate()` function-name marker selection. | Imported LLVM 17 branch control mechanism. | ported | Enables per-function markers such as `_fla_` and `_nofla_`; it is not an obfuscation transform by itself. |

`func2mod` is not a normal protection pass. Keep it out of routine obfuscation
pipelines unless the downstream multi-module link/package flow is explicit. Its
output partition count is controlled by the `-func2mod-outputs=N` option, which
is not a pass.

Current status: `func2mod` is disabled in the default lit test set with
`REQUIRES: func2mod`. The implementation calls LLVM's `SplitModule()` helper,
whose definition lives in the `TransformUtils` component. In this out-of-tree
pass-plugin build, `opt` does not export that symbol to the plugin, so running
`-func2mod` currently fails at load/run time with an undefined
`llvm::SplitModule(...)` symbol. Do not fix this by statically linking
`TransformUtils` into `yansollvm.so`: that pulls duplicate LLVM static globals
into the plugin and causes duplicate command-line option registration inside
`opt`. Treat `func2mod` as a separate follow-up: either implement splitting
without `SplitModule`, build it as a standalone tool, or use an LLVM shared
library setup where the symbol is exported by the host.

### Difference from the LLVM 9 main branch

The LLVM 9 main branch was an in-tree LLVM overlay. Its original YANSOllvm pass
set was registered directly into the legacy pass manager:

- `RegisterPass("vm")`
- `RegisterPass("merge")`
- `RegisterPass("bb2func")`
- `RegisterPass("flattening")`
- `RegisterPass("connect")`
- `RegisterPass("obfcon")`
- `RegisterPass("obfCall")`
- `RegisterPass("func2mod")`

This LLVM 21 branch changes the call surface:

- The only pipeline name registered with LLVM is `yanso`, via
  `PassBuilder::registerPipelineParsingCallback` in the out-of-tree plugin.
- The user now invokes the plugin as `-load-pass-plugin build/yansollvm.so
  -passes=yanso`, then enables features with `cl::opt` flags.
- Original YANSOllvm passes are treated as updated implementations, not merely
  external ports: `-vm`, `-merge`, `-func2mod`, `-bb2func`, `-connect`,
  `-obfcon`, and `-fla`.
- `-fla` is the updated LLVM 21 spelling/implementation for the old flattening
  pass. The old user-facing name was `-flattening`.
- `-split`, `-sub`, `-bcf`, `-icall`, `-sobf`, `-ibr`, `-igv`, and `-fncmd` are
  ported imported passes/control logic from the LLVM 17 branch's obfuscation
  family rather than original YANSOllvm passes.
- `-obfCall` is not implemented in this LLVM 21 plugin. The original version
  depends on LLVM core IR calling-convention IDs, X86 backend lowering, register
  masks, and TableGen-generated calling-convention code. Those changes are not
  compatible with a normal out-of-tree `opt` plugin and are intentionally not
  carried in this branch.

## Suggested pipelines

Conservative CFG/data obfuscation:

```bash
-passes=yanso -split -fla -sub -bcf
```

Call graph + CFG + constants:

```bash
-passes=yanso -vm -merge -bb2func -fla -connect -obfcon -sub -bcf
```

## Determinism smoke test

After building the plugin:

```bash
LLVM_BUILD=/path/to/llvm-project/build python3 scripts/check_determinism.py
```

By default this writes temporary files under `build/determinism/`.

## LLVM test-suite matrix helper

For broader local validation:

```bash
LLVM_BUILD=/path/to/llvm-project/build \
LLVM_TEST_SUITE=/path/to/llvm-test-suite \
python3 scripts/run_llvm_test_suite_pass_matrix.py
```

By default this writes results under `build/test-suite-runs/`.

## License

The project is released under GPLv3. See `LICENSE`.

Some pass code is derived from or inspired by the original YANSOllvm sources and
third-party LLVM obfuscation work noted in the main branch history. LLVM itself
is not vendored in this branch.
