#!/usr/bin/env python3
"""clang/clang++ wrapper for running yansollvm passes inside llvm-test-suite.

Intercepts ordinary single-source compile-to-object invocations:
  clang ... -c source.[c|cc|cpp|cxx] -o file.o
and rewrites them as:
  real clang ... -S -emit-llvm source -> input.ll
  opt -load-pass-plugin yansollvm.so -passes=<pass>,verify input.ll -> obf.ll
  real clang ... -c obf.ll -> file.o

Everything else is passed through to the real compiler so CMake/lit/test-suite
keep their normal include dirs, per-test flags, link lines, run args, and fpcmp.
"""
from __future__ import annotations

import json
import os
import shlex
import subprocess
import sys
import time
from pathlib import Path

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".C"}
PASSTHROUGH_FLAGS = {
    "-E",
    "-M",
    "-MM",
    "--version",
    "-v",
    "-dumpmachine",
    "-print-resource-dir",
    "-print-search-dirs",
    "-print-libgcc-file-name",
    "-print-prog-name=ld",
}
VALUE_FLAGS = {
    "-o",
    "-MF",
    "-MT",
    "-MQ",
    "-include",
    "-imacros",
    "-isysroot",
    "-target",
    "--target",
    "-x",
    "-Xclang",
    "-Xassembler",
    "-Xpreprocessor",
    "-Xlinker",
    "-mllvm",
    "-isystem",
    "-iquote",
    "-idirafter",
    "-I",
    "-D",
    "-U",
}
IR_DROP_VALUE_FLAGS = {"-o", "-MF", "-MT", "-MQ"}
IR_DROP_FLAGS = {"-c"}
BYPASS_SOURCE_PARTS = (
    "/CMakeFiles/CMakeScratch/",
    "/CMakeFiles/CompilerIdC/",
    "/CMakeFiles/CompilerIdCXX/",
    "/llvm-test-suite/tools/",
    "/llvm-test-suite/cmake/",
)


def env_path(name: str, default: str | None = None) -> str:
    value = os.environ.get(name, default)
    if not value:
        raise SystemExit(f"missing required env {name}")
    return value


def shell(cmd: list[str]) -> str:
    return " ".join(shlex.quote(x) for x in cmd)


def run(cmd: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(cmd, cwd=str(cwd) if cwd else None, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def is_source_arg(arg: str) -> bool:
    if arg.startswith("-"):
        return False
    return Path(arg).suffix in SOURCE_SUFFIXES


def should_bypass_source(source: str) -> bool:
    try:
        norm = Path(source).resolve().as_posix()
    except OSError:
        norm = Path(source).absolute().as_posix()
    return any(part in norm for part in BYPASS_SOURCE_PARTS)


def parse(argv: list[str]) -> tuple[bool, list[str], str | None, str | None, list[str]]:
    """Return (intercept, passthrough_reason, source, output, source_indices)."""
    if any(a in PASSTHROUGH_FLAGS for a in argv):
        return False, argv, None, None, []
    if "-c" not in argv:
        return False, argv, None, None, []

    source_indices: list[int] = []
    output: str | None = None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "-o" and i + 1 < len(argv):
            output = argv[i + 1]
            i += 2
            continue
        if a in VALUE_FLAGS and i + 1 < len(argv):
            i += 2
            continue
        if a.startswith("-o") and len(a) > 2:
            output = a[2:]
            i += 1
            continue
        if is_source_arg(a):
            source_indices.append(i)
        i += 1

    if len(source_indices) != 1 or output is None:
        return False, argv, None, None, []
    return True, argv, argv[source_indices[0]], output, [str(i) for i in source_indices]


def make_ir_args(argv: list[str], source: str, out_ll: Path) -> list[str]:
    res: list[str] = []
    i = 0
    saw_compile = False
    while i < len(argv):
        a = argv[i]
        if a in IR_DROP_FLAGS:
            saw_compile = True
            i += 1
            continue
        if a in IR_DROP_VALUE_FLAGS and i + 1 < len(argv):
            i += 2
            continue
        if a.startswith("-o") and len(a) > 2:
            i += 1
            continue
        if a == source:
            res.append(a)
            i += 1
            continue
        res.append(a)
        i += 1
    if not saw_compile:
        res.append("-c")
    res += ["-S", "-emit-llvm", "-o", str(out_ll)]
    return res


def make_final_args(argv: list[str], source: str, obf_ll: Path) -> list[str]:
    return [str(obf_ll) if a == source else a for a in argv]


def infer_cxx_mode(invoked: str, argv: list[str]) -> bool:
    if "++" in invoked or invoked.endswith("cxx") or invoked.endswith("cpp"):
        return True
    for arg in argv:
        p = Path(arg)
        if p.suffix in {".cc", ".cpp", ".cxx", ".C"}:
            return True
        if arg.endswith((".cc.o", ".cpp.o", ".cxx.o", ".C.o")):
            return True
    return False


def main() -> int:
    argv = sys.argv[1:]
    invoked = Path(sys.argv[0]).name
    is_cxx = infer_cxx_mode(invoked, argv)
    real = env_path("YANSOLLVM_REAL_CLANGXX" if is_cxx else "YANSOLLVM_REAL_CLANG")
    opt = env_path("YANSOLLVM_OPT")
    plugin = env_path("YANSOLLVM_PLUGIN")
    passes = env_path("YANSOLLVM_PASSES", "sobf,verify")
    log_path = os.environ.get("YANSOLLVM_WRAPPER_LOG")
    fail_dir = Path(os.environ.get("YANSOLLVM_FAIL_DIR", "/tmp/yansollvm-wrapper-fail-ir"))

    intercept, _, source, output, _ = parse(argv)
    if not intercept or source is None or output is None or should_bypass_source(source):
        cp = subprocess.run([real, *argv])
        return cp.returncode

    out = Path(output)
    tmp_root = Path(os.environ.get("YANSOLLVM_TMPDIR", str(out.parent / ".yansollvm-wrapper")))
    tmp_root.mkdir(parents=True, exist_ok=True)
    stem = out.name.replace(os.sep, "_")
    raw_ll = tmp_root / f"{stem}.input.ll"
    obf_ll = tmp_root / f"{stem}.obf.ll"

    t0 = time.time()
    emit_cmd = [real, *make_ir_args(argv, source, raw_ll)]
    opt_cmd = [opt, "-load-pass-plugin", plugin, "-passes=" + passes, "-S", str(raw_ll), "-o", str(obf_ll)]
    final_cmd = [real, *make_final_args(argv, source, obf_ll)]
    status = "ok"
    phase = "emit_ir"
    detail = ""

    cp = run(emit_cmd)
    if cp.returncode != 0:
        status = "fail"
        detail = cp.stderr.decode("utf-8", "replace")[-4000:]
        sys.stderr.buffer.write(cp.stderr)
        rc = cp.returncode
    else:
        phase = "opt"
        cp = run(opt_cmd)
        if cp.returncode != 0:
            status = "fail"
            detail = cp.stderr.decode("utf-8", "replace")[-4000:]
            fail_dir.mkdir(parents=True, exist_ok=True)
            saved = fail_dir / f"{stem}.{int(t0)}.ll"
            try:
                saved.write_bytes(raw_ll.read_bytes())
                detail += f"\nsaved_ir={saved}\n"
            except OSError:
                pass
            sys.stderr.buffer.write(cp.stderr)
            rc = cp.returncode
        else:
            phase = "codegen"
            cp = run(final_cmd)
            if cp.returncode != 0:
                status = "fail"
                detail = cp.stderr.decode("utf-8", "replace")[-4000:]
                sys.stderr.buffer.write(cp.stderr)
                rc = cp.returncode
            else:
                rc = 0

    if log_path:
        rec = {
            "status": status,
            "phase": phase,
            "source": source,
            "output": output,
            "passes": passes,
            "seconds": time.time() - t0,
            "emit_cmd": shell(emit_cmd),
            "opt_cmd": shell(opt_cmd),
            "final_cmd": shell(final_cmd),
            "detail": detail,
        }
        try:
            with open(log_path, "a", encoding="utf-8") as f:
                f.write(json.dumps(rec, ensure_ascii=False) + "\n")
        except OSError:
            pass
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
