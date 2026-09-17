#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
AI Stroke Painter — V8 Quality Benchmark Runner
SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
SPDX-License-Identifier: GPL-2.0-or-later

Usage:
    python run_bench.py [--gate-only] [--report-out benchmark_report.json]
"""

import os
import sys
import json
import time
import argparse
import subprocess
from pathlib import Path

# ANSI color escape codes
GREEN = "\033[92m"
RED = "\033[91m"
YELLOW = "\033[93m"
CYAN = "\033[96m"
BOLD = "\033[1m"
RESET = "\033[0m"


def load_golden_set(golden_path: Path) -> dict:
    with open(golden_path, "r", encoding="utf-8") as f:
        return json.load(f)


def run_cpp_bench_test(build_dir: Path) -> tuple[int, str]:
    """Runs the compiled KisAiQualityBenchGateTest binary."""
    test_exe = build_dir / "KisAiQualityBenchGateTest.exe"
    if not test_exe.exists():
        test_exe = build_dir / "bin" / "KisAiQualityBenchGateTest.exe"
    if not test_exe.exists():
        test_exe = build_dir / "KisAiQualityBenchGateTest"

    if not test_exe.exists():
        return -1, f"Executable not found in {build_dir}"

    env = os.environ.copy()
    craft_root = os.environ.get("CRAFT_ROOT", r"C:\CraftRoot")
    extra_paths = [
        os.path.join(craft_root, "bin"),
        os.path.join(craft_root, "mingw64", "bin"),
        os.path.join(craft_root, "dev-utils", "bin"),
    ]
    env["PATH"] = ";".join(extra_paths) + ";" + env.get("PATH", "")

    proc = subprocess.run(
        [str(test_exe)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
        timeout=180,
    )
    return proc.returncode, proc.stdout


def main():
    parser = argparse.ArgumentParser(description="AI Stroke Painter V8 Quality Benchmark")
    parser.add_argument("--golden", default="golden_set.json", help="Path to golden set JSON")
    parser.add_argument("--build-dir", default="../../build-test", help="Path to test build directory")
    parser.add_argument("--report-out", default="benchmark_report.json", help="Path to output report")
    parser.add_argument("--gate-only", action="store_true", help="Fail fast on gate failure")
    args = parser.parse_args()

    script_dir = Path(__file__).parent.resolve()
    golden_file = script_dir / args.golden
    if not golden_file.exists():
        golden_file = Path(args.golden).resolve()

    if not golden_file.exists():
        print(f"{RED}Error: Golden set not found: {golden_file}{RESET}")
        sys.exit(1)

    golden_data = load_golden_set(golden_file)
    prompts = golden_data.get("prompts", [])
    print(f"{BOLD}{CYAN}=== AI Stroke Painter V8 Quality Benchmark ==={RESET}")
    print(f"Golden Set: {golden_data.get('name', 'V8')} ({len(prompts)} prompts)")

    build_dir = Path(args.build_dir).resolve()
    if not build_dir.exists():
        # Try alternate path
        build_dir = (script_dir.parent.parent / "build-test").resolve()

    print(f"Build Dir: {build_dir}")

    start_time = time.time()
    code, output = run_cpp_bench_test(build_dir)
    elapsed = time.time() - start_time

    report = {
        "benchmark": golden_data.get("name"),
        "version": golden_data.get("version"),
        "total_prompts": len(prompts),
        "execution_time_sec": round(elapsed, 3),
        "return_code": code,
        "passed": code == 0,
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }

    report_path = script_dir / args.report_out
    with open(report_path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)

    print(f"\nExecution finished in {elapsed:.2f}s (Exit code: {code})")
    if code == 0:
        print(f"{GREEN}{BOLD}[PASS] GATE PASSED: quality gate binary exited 0 (see C++ test log for per-prompt detail).{RESET}")
    else:
        print(f"{RED}{BOLD}[FAIL] GATE FAILED: Output details:{RESET}")
        print(output)
        sys.exit(1)


if __name__ == "__main__":
    main()
