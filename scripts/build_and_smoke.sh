#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

if [[ -z "${LLVM_BUILD:-}" ]]; then
  cat >&2 <<'EOF'
error: LLVM_BUILD is not set

Set LLVM_BUILD to an LLVM 21 build directory that contains lib/cmake/llvm.
Example:
  LLVM_BUILD=/path/to/llvm-project/build scripts/build_and_smoke.sh
EOF
  exit 2
fi

cd "$ROOT"
cmake -S . -B build -G Ninja -DLLVM_DIR="$LLVM_BUILD/lib/cmake/llvm"
ninja -C build check-yansollvm
