"""プロジェクト全体の Lint、Format、型チェック、セルフテストを一括実行するスクリプト。"""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys


def run_command(name: str, args: list[str], cwd: Path) -> bool:
    print(f"\n=== Running {name} ({' '.join(args)}) ===")
    result = subprocess.run(args, cwd=cwd)
    if result.returncode == 0:
        print(f"-> {name}: PASSED")
        return True
    print(f"-> {name}: FAILED (exit code: {result.returncode})")
    return False


def main() -> int:
    project_dir = Path(__file__).resolve().parent
    parent_dir = project_dir.parent

    steps: list[tuple[str, list[str], Path]] = [
        ("Ruff Check", [sys.executable, "-m", "ruff", "check", "."], project_dir),
        ("Ruff Format Check", [sys.executable, "-m", "ruff", "format", "--check", "."], project_dir),
        ("Mypy Type Check", [sys.executable, "-m", "mypy", "."], project_dir),
        ("Pyrefly Type Check", [sys.executable, "-m", "pyrefly", "check"], project_dir),
        (
            "Regression Self-Tests",
            [sys.executable, "-m", "unittest", "ai_stroke_painter.self_test", "-v"],
            parent_dir,
        ),
        (
            "Headless Compatibility Tests",
            [sys.executable, "-S", "-m", "unittest", "ai_stroke_painter.self_test"],
            parent_dir,
        ),
        (
            "Illustration Quality Gates",
            [sys.executable, "-m", "ai_stroke_painter.quality_check"],
            parent_dir,
        ),
    ]

    failed: list[str] = []
    for name, args, cwd in steps:
        if not run_command(name, args, cwd):
            failed.append(name)

    print("\n" + "=" * 40)
    if failed:
        print(f"FAILED steps: {', '.join(failed)}")
        return 1

    print("ALL CHECKS PASSED SUCCESSFULLY!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
