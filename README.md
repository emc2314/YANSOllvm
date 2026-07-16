# YANSOllvm

Yet Another Not So Obfuscated LLVM.

YANSOllvm is an LLVM 21 obfuscation pass plugin. `ObfCall` is not currently
available because it requires LLVM core and X86 backend changes beyond the
normal plugin interface.

## Build

Requirements:

- LLVM 21 build compatible with 21.1.8 and configured with
  `-DLLVM_ENABLE_PROJECTS=clang`
- CMake 3.20+, Ninja, and Python 3
- Valgrind for the MFLA cleanup test

```bash
export LLVM_BUILD=/path/to/llvm-project/build
cmake -S . -B build -G Ninja -DLLVM_DIR="$LLVM_BUILD/lib/cmake/llvm"
ninja -C build check-yansollvm
```

`check-yansollvm` builds `build/yansollvm.so`, runs the YMBA catalog self-test,
and then runs the lit suite. The `func2mod` test remains unsupported.

## Usage

```bash
"$LLVM_BUILD/bin/clang" -O0 -Xclang -disable-O0-optnone \
  -emit-llvm -S input.c -o input.ll

"$LLVM_BUILD/bin/opt" \
  -load-pass-plugin build/yansollvm.so \
  -passes='yanso,verify' \
  -S input.ll -o input.obf.ll

"$LLVM_BUILD/bin/clang" input.obf.ll -o input.obf
```

`yanso` appends this fixed sequence at its position in the pipeline:

```text
obfcon -> vm -> bb2func -> merge -> mfla -> bb2func
```

Passes can be placed before or after the alias and may be repeated:

```bash
-passes='sobf,yanso,ibr,verify'
-passes='obfcon,yanso,bb2func,verify'
```

Use `-yanso-seed=<string>` to change the deterministic project seed.

### Protection pipeline

The default sequence builds protection in layers:

1. `vm` selects bounded local operation DAGs and turns them into super-op
   handlers. Each operation or DAG can choose among seeded expression, YMBA
   relation, data-mux, and control-flow variants.
2. `merge` groups those handlers together with eligible program functions
   behind keyed shared dispatchers. In the VM pipeline this acts as an op-fusion
   layer: individual helper identities and ABIs are absorbed into larger fused
   entries.
3. `mfla` converts eligible calls and returns across the merged set into a
   threaded continuation machine. At this point handlers, fused operations,
   business CFG, virtual frames, and dispatch state form one VM-like protection
   domain rather than separate transformations.

## Passes

| Pass | Provenance | Maturity | Key technique |
| --- | --- | --- | --- |
| `vm` | Original | Stable | Fuses local scalar/GEP/load DAGs into super-op handlers. YMBA relations, mutation wrappers, and bounded per-operation variants diversify both expression and CFG shape; stores remain conservative single operations. |
| `merge` | Original | Stable | Forms cost-guided dispatcher groups using semantic similarity, call weight, ABI shape, and slot pressure. Keyed XOR selectors and shared typed storage hide distinct functions behind ABI- and EH-aware wrappers. |
| `mfla` | Original | Experimental | Module-wide flattening through CPS: internal calls and returns become continuation records plus frame/state jumps inside a threaded mega function. Recursive SCCs, tail-frame reuse, paged logical frames, encoded targets, and `indirectbr` remove ordinary call/return structure. |
| `bb2func` | Original | Stable | Reshapes linear and dense CFG regions, scores branch arms, switch subsets, subchains, and single blocks, then extracts legal candidates with `CodeExtractor`. |
| `obfcon` | Original | Stable | Rebuilds constants with modular inverses, XOR, and additive identities; zero values can be derived from live integer data through seeded opaque expressions. |
| `connect` | Original | Stable | Splits and shuffles blocks, then replaces direct edges with opaque switch connectors containing decoy destinations and trap-backed defaults. |
| `func2mod` | Original | Experimental | Externalizes definitions and emits configurable verified bitcode shards through LLVM `SplitModule`; it is a partitioning tool, not a normal protection transform. |
| `fla` | Enhanced | Stable | Region-aware flattening preserves atomic, EH, native, and address-taken boundaries. Dual state/hash-state transitions feed a rolling `mix64` dispatcher, with encoded transitions for cross-region edges. |
| `split` | Ported | Unmaintained | Basic-block splitting. |
| `sub` | Ported | Unmaintained | Instruction substitution. |
| `bcf` | Ported | Unmaintained | Bogus control flow. |
| `icall` | Ported | Unmaintained | Indirect calls. |
| `sobf` | Ported | Unmaintained | String encryption. |
| `ibr` | Ported | Unmaintained | Indirect branches. |
| `igv` | Ported | Unmaintained | Indirect global-variable access. |

### MFLA design

MFLA performs module-level flattening through continuation-passing style (CPS).
Eligible functions become ABI-preserving wrappers around one threaded mega
function. An internal call no longer executes as a native call:

```text
store arguments -> push/reuse logical frame -> record continuation
-> update encoded state -> jump to callee entry
```

A return applies the saved continuation, copies the result to its caller slot,
restores or releases the frame, and jumps to the resume state. Recursive SCCs
use child frame tokens; eligible self-tail calls reuse the current frame.

Branch, switch, call, and return transfers share state-keyed encoded targets
and eventually reach `indirectbr`. Arguments, PHIs, spills, fixed allocas,
results, and continuation records live in logical frame storage. Frames are
allocated from linked pages on demand and recycled through a free list, while
each external wrapper invocation owns an independent context.

The result is not only a larger FLA: ordinary function boundaries, native
internal call/return edges, and a directly readable call graph are replaced by
one continuation and state machine. Applied after `vm` and `merge`, this is the
stage that turns super-op handlers and op fusion into a VM-style protected
program.

### Tuning options

- `-yanso-seed=<string>`: root seed for deterministic pass choices; default
  `YANSOllvm`.
- `-vm-op-max-len=N`: maximum instructions fused into one VM DAG/super-op;
  default 4. Set 1 for single-operation handlers.
- `-vm-max-variants-per-op=N`: maximum helper bodies retained per
  operation/type bucket; default 8. Set 0 for no cap.
- `-vm-mutation-variant-permille=N`: probability per thousand of selecting VM
  structural mutation wrappers; default 500.
- `-vm-relation-app-variant-permille=N`: probability per thousand of embedding
  a generated YMBA relation application; default 500.
- `-vm-ymba-temperature=T`: cost temperature for choosing catalog YMBA versus
  built-in expressions and relations; default 28. Positive values favor lower
  cost, zero selects the minimum cost, and negative values favor higher-cost
  MBA. Larger absolute values flatten the distribution toward uniform.
- `-merge-max-group-size=N`: maximum functions or handlers fused into one
  merge dispatcher; default 8. Larger groups increase ambiguity but also ABI
  slot pressure and code size.
- `-mfla-frames-per-page=N`: logical activation records per MFLA frame page;
  default 16. Smaller values allocate and resolve more pages.
- `-mfla-page-table-block-entries=N`: page pointers stored in each linked MFLA
  page-table block; default 16.
- `-func2mod-outputs=N`: number of emitted bitcode shards; default 3.

The VM, Merge, and MFLA controls are hidden implementation-tuning options and
may change.

## Correctness and limitations

### MFLA concurrency and reentry

MFLA no longer stores activation state in process-global frame or continuation
arrays. Each wrapper invocation creates an independent context, so ordinary
concurrent calls from different threads do not share MFLA frame state.

Internal CPS calls are state jumps within the current mega-function invocation,
not native reentry. Direct and mutual recursion use logical child frames and
are covered by runtime tests. A native boundary callback into a transformed
wrapper does create a nested mega-function invocation; the separate context
design should isolate it, but callback reentry and multi-thread stress are not
currently tested and should still be treated as experimental.

MFLA also changes concurrency-relevant runtime behavior:

- frame pages and page-table blocks use `malloc`/`free`, which may lock,
  synchronize, invoke allocator hooks, fail, or trap on OOM even when the
  original function performed no allocation;
- allocator-backed execution is not async-signal-safe, so signal-handler
  reentry is unsupported;
- wrapper attribute sanitization removes memory-effect and `nofree`
  attributes, but concurrency/progress/call-graph attributes such as `nosync`,
  `willreturn`, and `norecurse` have not been fully audited;
- atomic and thread-heavy programs have no dedicated MFLA correctness tests.

### MFLA exceptions and Windows EH

MFLA does not currently lower EH regions. Functions containing landing pads,
Windows `catchpad`/`cleanuppad` funclets, `invoke`-style terminators, or other
unsupported EH control are left outside the MFLA domain. Potentially throwing
internal CPS calls are also rejected.

If a native boundary call unwinds through a transformed wrapper, normal
`malloc` page cleanup is bypassed because MFLA does not yet emit EH cleanup
regions; this can leak frame storage. POSIX thread cancellation can have the
same issue. Windows funclet EH is regression-tested for `fla`, and `merge`
rewrites supported `invoke` callsites, but neither implies Windows EH support
inside MFLA.

### MFLA eligibility

MFLA needs at least two eligible functions. It supports integer, pointer,
floating-point, and fixed-vector frame values. It skips dynamic stack state,
scalable allocas, varargs, selected ABI attributes, `musttail`, `callbr`,
unsupported terminators, and unsafe `blockaddress` domains.

### Other passes

VM, Merge, BB2Func, ObfCon, Connect, and FLA do not add shared mutable runtime
state. VM explicitly leaves volatile and atomic memory operations out of its
memory handlers. No inherent cross-thread activation-state issue is currently
known for these passes, although broad multi-thread testing is still missing.

`func2mod` is disabled in the default test set. Its use of `llvm::SplitModule`
requires a host configuration that exports the symbol; component-style static
`opt` builds may fail to resolve it.

Dedicated test coverage is still missing for `icall`, `ibr`, `igv`, `split`,
`sub`, `bcf`, `connect`, and `obfcon`.

## Additional validation

```bash
LLVM_BUILD=/path/to/llvm-project/build \
  python3 scripts/check_determinism.py

LLVM_BUILD=/path/to/llvm-project/build \
LLVM_TEST_SUITE=/path/to/llvm-test-suite \
  python3 scripts/run_llvm_test_suite_pass_matrix.py
```

Outputs are written under `build/determinism/` and `build/test-suite-runs/`.

## License

GPLv3. See `LICENSE`.
