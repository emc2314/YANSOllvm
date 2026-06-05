#!/usr/bin/env python3
"""Run llvm-test-suite single-file C programs through each yansollvm pass.

This is a pragmatic correctness harness for out-of-tree IR passes:
  source.c -> baseline executable -> capture exit/stdout/stderr
  source.c -> LLVM IR -> yansollvm opt pass -> verify -> executable -> compare

It intentionally excludes func2mod because that pass emits side-effect bitcode shards
rather than an in-place executable-preserving transform.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import json
import os
import shlex
import subprocess
import time
from pathlib import Path
from typing import Iterable


DEFAULT_PASSES = [
    "sobf",
    "icall",
    "split",
    "fla",
    "sub",
    "bcf",
    "ibr",
    "igv",
    "vm",
    "merge",
    "bb2func",
    "connect",
    "obfcon",
]

# Keep this first wave deterministic and single-file: C tests with checked runtime
# behavior and no external input files. The list spans arrays, pointers, globals,
# switch/indirect-goto, varargs, structs, long double, and small benchmark kernels.
DEFAULT_SOURCE_DIRS = [
    "SingleSource/Regression/C",
    "SingleSource/Benchmarks/Stanford",
]

# These are valid test-suite programs but poor generic pass-smoke candidates for
# this harness: they depend on target-specific details, expected compile-time
# diagnostics, or frontend extensions that are not useful for pass semantics.
SKIP_BASENAMES = {
    "2003-06-16-InvalidInitializer.c",
    "float16-smoke.c",
}


@dataclasses.dataclass(frozen=True)
class Toolchain:
    clang: Path
    clangxx: Path
    opt: Path
    plugin: Path


@dataclasses.dataclass(frozen=True)
class TestCase:
    source: Path
    rel: str
    language: str


@dataclasses.dataclass
class Result:
    pass_name: str
    test: str
    status: str
    phase: str
    seconds: float
    detail: str = ""


def run_cmd(cmd: list[str], cwd: Path | None = None, timeout: int = 30) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd,
        cwd=str(cwd) if cwd else None,
        text=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )


def timeout_result(pass_name: str, tc: TestCase, phase: str, start: float, cmd: list[str]) -> Result:
    return Result(
        pass_name,
        tc.rel,
        "TIMEOUT",
        phase,
        time.time() - start,
        f"timeout after command: {quote_cmd(cmd)}",
    )


def decode(data: bytes, limit: int = 4000) -> str:
    text = data.decode("utf-8", errors="replace")
    return text if len(text) <= limit else text[:limit] + "\n...<truncated>"


def quote_cmd(cmd: list[str]) -> str:
    return " ".join(shlex.quote(x) for x in cmd)


SOURCE_SUFFIXES = {".c": "c", ".cc": "cxx", ".cpp": "cxx", ".cxx": "cxx", ".C": "cxx"}


def compiler_for(tc: TestCase, tools: Toolchain) -> Path:
    return tools.clangxx if tc.language == "cxx" else tools.clang


def compile_flags(tc: TestCase, args: argparse.Namespace, emit_ir: bool) -> list[str]:
    flags = [args.cxx_standard if tc.language == "cxx" else args.c_standard, "-O0"]
    if emit_ir:
        flags += ["-Xclang", "-disable-O0-optnone", "-emit-llvm", "-S"]
    return flags


def discover_tests(test_suite: Path, source_dirs: Iterable[str]) -> list[TestCase]:
    tests: list[TestCase] = []
    seen: set[Path] = set()
    for rel_dir in source_dirs:
        root = test_suite / rel_dir
        for src in sorted(p for p in root.rglob("*") if p.suffix in SOURCE_SUFFIXES):
            if src in seen or src.name in SKIP_BASENAMES:
                continue
            seen.add(src)
            # Require a test-suite reference_output as a cheap filter for programs
            # intended to be run and checked. We still compare baseline-vs-pass to
            # avoid target-format drift in bundled references.
            if not src.with_suffix(".reference_output").exists():
                continue
            tests.append(TestCase(src, src.relative_to(test_suite).as_posix(), SOURCE_SUFFIXES[src.suffix]))
    return tests


def compile_baseline(
    tc: TestCase, out_dir: Path, tools: Toolchain, args: argparse.Namespace
) -> tuple[str, str | tuple[int, bytes, bytes]]:
    exe = out_dir / "baseline.exe"
    cmd = [str(compiler_for(tc, tools)), *compile_flags(tc, args, emit_ir=False), str(tc.source), "-lm", "-o", str(exe)]
    try:
        cp = run_cmd(cmd, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        return "baseline_timeout", quote_cmd(cmd)
    except OSError as e:
        return "baseline_compile_oserror", quote_cmd(cmd) + "\n" + repr(e)
    if cp.returncode != 0:
        return "baseline_compile", quote_cmd(cmd) + "\n" + decode(cp.stderr)
    if not exe.is_file() or not os.access(exe, os.X_OK):
        return "baseline_not_executable", str(exe)
    run_cmdline = [str(exe.resolve())]
    try:
        cp = run_cmd(run_cmdline, cwd=tc.source.parent, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        return "baseline_run_timeout", quote_cmd(run_cmdline)
    except OSError as e:
        return "baseline_run_oserror", quote_cmd(run_cmdline) + "\n" + repr(e)
    return "ok", (cp.returncode, cp.stdout, cp.stderr)


def run_one(pass_name: str, tc: TestCase, args: argparse.Namespace, tools: Toolchain) -> Result:
    start = time.time()
    safe_rel = Path(tc.rel).with_suffix("").as_posix().replace("/", "__")
    out_dir = args.out / pass_name / safe_rel
    out_dir.mkdir(parents=True, exist_ok=True)

    phase, baseline_result = compile_baseline(tc, out_dir, tools, args)
    if phase != "ok":
        return Result(
            pass_name, tc.rel, "SKIP", phase, time.time() - start, str(baseline_result)
        )
    assert isinstance(baseline_result, tuple)
    baseline_exit, baseline_stdout, baseline_stderr = baseline_result

    raw_ll = out_dir / "input.ll"
    obf_ll = out_dir / f"{pass_name}.ll"
    exe = out_dir / f"{pass_name}.exe"

    compile_cmd = [
        str(compiler_for(tc, tools)),
        *compile_flags(tc, args, emit_ir=True),
        str(tc.source),
        "-o",
        str(raw_ll),
    ]
    try:
        cp = run_cmd(compile_cmd, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        return timeout_result(pass_name, tc, "emit_ir_timeout", start, compile_cmd)
    if cp.returncode != 0:
        return Result(
            pass_name,
            tc.rel,
            "FAIL",
            "emit_ir",
            time.time() - start,
            quote_cmd(compile_cmd) + "\n" + decode(cp.stderr),
        )

    opt_cmd = [
        str(tools.opt),
        "-load-pass-plugin",
        str(tools.plugin),
        f"-{pass_name}",
        "-passes=yanso,verify",
        "-S",
        str(raw_ll),
        "-o",
        str(obf_ll),
    ]
    try:
        cp = run_cmd(opt_cmd, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        return timeout_result(pass_name, tc, "opt_timeout", start, opt_cmd)
    if cp.returncode != 0:
        return Result(
            pass_name,
            tc.rel,
            "FAIL",
            "opt",
            time.time() - start,
            quote_cmd(opt_cmd) + "\n" + decode(cp.stderr),
        )

    link_cmd = [str(compiler_for(tc, tools)), str(obf_ll), "-lm", "-o", str(exe)]
    try:
        cp = run_cmd(link_cmd, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        return timeout_result(pass_name, tc, "link_timeout", start, link_cmd)
    if cp.returncode != 0:
        return Result(
            pass_name,
            tc.rel,
            "FAIL",
            "link",
            time.time() - start,
            quote_cmd(link_cmd) + "\n" + decode(cp.stderr),
        )

    run_cmdline = [str(exe.resolve())]
    try:
        cp = run_cmd(run_cmdline, cwd=tc.source.parent, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        return timeout_result(pass_name, tc, "run_timeout", start, run_cmdline)
    except OSError as e:
        return Result(pass_name, tc.rel, "FAIL", "run_oserror", time.time() - start, quote_cmd(run_cmdline) + "\n" + repr(e))

    if (cp.returncode, cp.stdout, cp.stderr) != (
        baseline_exit,
        baseline_stdout,
        baseline_stderr,
    ):
        detail = {
            "baseline_exit": baseline_exit,
            "pass_exit": cp.returncode,
            "baseline_stdout": decode(baseline_stdout),
            "pass_stdout": decode(cp.stdout),
            "baseline_stderr": decode(baseline_stderr),
            "pass_stderr": decode(cp.stderr),
        }
        return Result(
            pass_name,
            tc.rel,
            "FAIL",
            "compare",
            time.time() - start,
            json.dumps(detail, ensure_ascii=False, indent=2),
        )

    return Result(pass_name, tc.rel, "PASS", "ok", time.time() - start)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument(
        "--llvm-build",
        type=Path,
        default=None,
    )
    parser.add_argument("--test-suite", type=Path, default=None)
    parser.add_argument("--out", type=Path, default=None)
    parser.add_argument("--passes", default=",".join(DEFAULT_PASSES))
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 1) - 4))
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--c-standard", default="-std=gnu89")
    parser.add_argument("--cxx-standard", default="-std=gnu++14")
    parser.add_argument(
        "--limit", type=int, default=0, help="limit test count after discovery; 0 means all selected tests"
    )
    parser.add_argument(
        "--source-dir", action="append", default=[], help="relative llvm-test-suite source dir; may repeat"
    )
    args = parser.parse_args()

    if args.llvm_build is None:
        env_llvm_build = os.environ.get("LLVM_BUILD")
        if not env_llvm_build:
            parser.error("--llvm-build or LLVM_BUILD is required")
        args.llvm_build = Path(env_llvm_build)
    if args.test_suite is None:
        env_test_suite = os.environ.get("LLVM_TEST_SUITE")
        if not env_test_suite:
            parser.error("--test-suite or LLVM_TEST_SUITE is required")
        args.test_suite = Path(env_test_suite)
    if args.out is None:
        args.out = args.repo / "build" / "test-suite-runs"

    tools = Toolchain(
        clang=args.llvm_build / "bin" / "clang",
        clangxx=args.llvm_build / "bin" / "clang++",
        opt=args.llvm_build / "bin" / "opt",
        plugin=args.repo / "build" / "yansollvm.so",
    )
    for tool in [tools.clang, tools.clangxx, tools.opt, tools.plugin]:
        if not tool.exists():
            raise SystemExit(f"missing tool/artifact: {tool}")

    source_dirs = args.source_dir or DEFAULT_SOURCE_DIRS
    tests = discover_tests(args.test_suite, source_dirs)
    if args.limit:
        tests = tests[: args.limit]
    passes = [p for p in args.passes.split(",") if p]

    args.out.mkdir(parents=True, exist_ok=True)
    print(f"test_suite={args.test_suite}")
    print(f"out={args.out}")
    print(f"passes={','.join(passes)}")
    print(f"tests={len(tests)} jobs={args.jobs}")

    all_results: list[Result] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futs = [pool.submit(run_one, p, tc, args, tools) for p in passes for tc in tests]
        for i, fut in enumerate(concurrent.futures.as_completed(futs), 1):
            r = fut.result()
            all_results.append(r)
            if r.status not in {"PASS", "TIMEOUT"}:
                print(f"[{i}/{len(futs)}] {r.status} {r.pass_name} {r.test} phase={r.phase}")
            elif r.status == "TIMEOUT":
                print(f"[{i}/{len(futs)}] TIMEOUT {r.pass_name} {r.test} phase={r.phase}")
            elif i % 50 == 0 or i == len(futs):
                print(f"[{i}/{len(futs)}] progress")

    all_results.sort(key=lambda r: (r.pass_name, r.test))
    summary: dict[str, dict[str, int]] = {}
    for r in all_results:
        summary.setdefault(r.pass_name, {}).setdefault(r.status, 0)
        summary[r.pass_name][r.status] += 1

    report = {
        "passes": passes,
        "tests": [tc.rel for tc in tests],
        "summary": summary,
        "failures": [dataclasses.asdict(r) for r in all_results if r.status != "PASS"],
    }
    report_path = args.out / "summary.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2))

    print("summary:")
    for p in passes:
        s = summary.get(p, {})
        print(
            f"  {p:8s} PASS={s.get('PASS', 0)} FAIL={s.get('FAIL', 0)} "
            f"TIMEOUT={s.get('TIMEOUT', 0)} SKIP={s.get('SKIP', 0)}"
        )
    print(f"report={report_path}")
    return 1 if any(r.status in {"FAIL", "TIMEOUT"} for r in all_results) else 0


if __name__ == "__main__":
    raise SystemExit(main())
