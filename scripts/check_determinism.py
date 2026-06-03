#!/usr/bin/env python3
"""Basic yansollvm determinism smoke test.

Runs selected pass flag sets twice on the same input and compares output IR bytes.
This intentionally stays simple: no normalization, no broad matrix.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
from pathlib import Path

CASES = [
    ("tests/shape.ll", "-fla -sub -split"),
    ("tests/shape.ll", "-vm"),
    ("tests/shape.ll", "-connect"),
    ("tests/basic-pipeline.c", "-sobf -icall -ibr -igv"),
    ("tests/complex-pipelines.c", "-split -fla -sub -bcf -ibr -icall -igv -vm -merge -bb2func -connect -obfcon"),
]


def run(cmd: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument("--llvm-build", type=Path, default=None)
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument("--seed", default="YANSOllvm")
    args = ap.parse_args()

    if args.llvm_build is None:
        env_llvm_build = os.environ.get("LLVM_BUILD")
        if not env_llvm_build:
            ap.error("--llvm-build or LLVM_BUILD is required")
        args.llvm_build = Path(env_llvm_build)
    if args.out is None:
        args.out = args.repo / "build" / "determinism"

    clang = args.llvm_build / "bin" / "clang"
    opt = args.llvm_build / "bin" / "opt"
    plugin = args.repo / "build" / "yansollvm.so"
    args.out.mkdir(parents=True, exist_ok=True)

    failures = []
    for idx, (rel_input, flags) in enumerate(CASES):
        src = args.repo / rel_input
        flag_list = flags.split()
        ll_input = src
        temp_ll = None
        if src.suffix == ".c":
            temp_ll = args.out / f"case{idx}.input.ll"
            cp = run([str(clang), "-std=gnu89", "-O0", "-Xclang", "-disable-O0-optnone", "-emit-llvm", "-S", str(src), "-o", str(temp_ll)])
            if cp.returncode != 0:
                failures.append((rel_input, flags, "emit_ir", cp.stderr))
                continue
            ll_input = temp_ll

        hashes = []
        for run_idx in range(2):
            out = args.out / f"case{idx}.run{run_idx}.ll"
            cmd = [str(opt), "-load-pass-plugin", str(plugin), "-passes=yanso,verify", f"-yanso-seed={args.seed}", *flag_list, "-S", str(ll_input), "-o", str(out)]
            cp = run(cmd)
            if cp.returncode != 0:
                failures.append((rel_input, flags, "opt", cp.stderr))
                break
            hashes.append(sha256(out))
        if len(hashes) == 2 and hashes[0] != hashes[1]:
            failures.append((rel_input, flags, "mismatch", f"{hashes[0]} != {hashes[1]}"))
        elif len(hashes) == 2:
            print(f"PASS {rel_input} {flags} {hashes[0]}")

    if failures:
        for f in failures:
            print("FAIL", f[0], f[1], f[2], f[3][:1000])
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
