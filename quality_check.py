"""代表プロンプトの決定性・視覚品質・生成時間を検証するCLI。"""

from __future__ import annotations

import json
import time

from .procedural import generate_procedural_plan
from .quality import evaluate_plan_quality

SCENARIOS = (
    "anime girl with blue hair and green eyes",
    "mountain landscape with sakura",
    "blooming rose flower",
    "cute cat",
    "magic circle",
    "cyberpunk city skyline",
)


def main() -> int:
    failed = False
    rows: list[dict[str, object]] = []
    for prompt in SCENARIOS:
        started = time.perf_counter()
        plan = generate_procedural_plan(prompt, 42, None, 800, 600)
        elapsed = time.perf_counter() - started
        repeated = generate_procedural_plan(prompt, 42, None, 800, 600)
        report = evaluate_plan_quality(plan)
        deterministic = plan.as_dict() == repeated.as_dict()
        passed = (
            deterministic
            and elapsed < 2.0
            and report.coverage >= 0.35
            and report.layer_count >= 2
            and report.out_of_bounds_points == 0
            and report.score >= 0.70
            and not report.issues
            and report.estimated_paint_calls <= 15_000
            and len(plan.strokes) <= 500
        )
        failed = failed or not passed
        rows.append(
            {
                "prompt": prompt,
                "passed": passed,
                "seconds": round(elapsed, 4),
                "strokes": len(plan.strokes),
                "coverage": round(report.coverage, 3),
                "quality_score": round(report.score, 3),
                "layers": report.layer_count,
                "fallback_paint_calls": report.estimated_paint_calls,
                "issues": report.issues,
            }
        )
    print(json.dumps(rows, ensure_ascii=False, indent=2))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
