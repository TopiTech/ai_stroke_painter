"""代表プロンプトの決定性・視覚品質・生成時間を検証するCLI。"""

from __future__ import annotations

import json
import time

from .brushes import infer_brush_profile
from .docker import AIStrokePainterDocker
from .procedural import generate_procedural_plan
from .procedural.base import color_palette
from .quality import evaluate_plan_quality

EXPECTED_CATEGORIES = (
    "character",
    "character",
    "landscape",
    "landscape",
    "landscape",
    "fx",
    "fx",
    "creature",
    "geometry",
    "geometry",
    "landscape",
    "landscape",
)


def main() -> int:
    failed = False
    rows: list[dict[str, object]] = []
    for scenario_index, preset in enumerate(AIStrokePainterDocker.PRESETS):
        _title, prompt, palette_name, manual_count, brush_profile, _size, _opacity = preset
        expected_category = EXPECTED_CATEGORIES[scenario_index]
        palette_colors = {color.lower() for color in color_palette(palette_name).values()}
        for count_mode, requested_count in (("manual", manual_count), ("auto", None)):
            started = time.perf_counter()
            plan = generate_procedural_plan(
                prompt,
                42,
                requested_count,
                800,
                600,
                palette_name=palette_name,
                brush_profile=brush_profile,
            )
            elapsed = time.perf_counter() - started
            repeated = generate_procedural_plan(
                prompt,
                42,
                requested_count,
                800,
                600,
                palette_name=palette_name,
                brush_profile=brush_profile,
            )
            report = evaluate_plan_quality(plan)
            deterministic = plan.as_dict() == repeated.as_dict()
            overlay = plan.metadata.get("overlay", False) is True
            minimum_coverage = 0.05 if overlay else 0.35
            minimum_layers = 1 if overlay else 2
            brush_contract = all(
                infer_brush_profile(stroke.brush_preset, is_eraser=stroke.is_eraser) == brush_profile
                for stroke in plan.strokes
            )
            color_plan = plan.metadata.get("color_plan", {})
            planned_colors = (
                {value.lower() for value in color_plan.values() if isinstance(value, str) and value.startswith("#")}
                if isinstance(color_plan, dict)
                else set()
            )
            compiled_gradient_colors = {
                value.lower()
                for value in plan.metadata.get("compiled_gradient_colors", ())
                if isinstance(value, str) and value.startswith("#")
            }
            palette_contract = all(
                stroke.color.lower() in palette_colors | planned_colors | compiled_gradient_colors
                for stroke in plan.strokes
            )
            semantic_contract = report.semantic_fidelity_score >= 1.0 and not report.missing_required_elements
            budget = manual_count if requested_count is not None else 500
            passed = (
                deterministic
                and elapsed < 2.0
                and report.coverage >= minimum_coverage
                and report.layer_count >= minimum_layers
                and report.out_of_bounds_points == 0
                and report.score >= 0.70
                and not report.issues
                and report.estimated_paint_calls <= 15_000
                and 0 < len(plan.strokes) <= budget
                and plan.metadata.get("prompt_category") == expected_category
                and brush_contract
                and palette_contract
                and semantic_contract
            )
            failed = failed or not passed
            rows.append(
                {
                    "scenario": scenario_index,
                    "prompt": prompt,
                    "count_mode": count_mode,
                    "passed": passed,
                    "seconds": round(elapsed, 4),
                    "strokes": len(plan.strokes),
                    "coverage": round(report.coverage, 3),
                    "quality_score": round(report.score, 3),
                    "visual_score": report.visual_score,
                    "value_range": report.value_range,
                    "dominant_color_ratio": report.dominant_color_ratio,
                    "banding": max(report.horizontal_banding_score, report.vertical_banding_score),
                    "edge_density": report.edge_density,
                    "subject_background_contrast": report.subject_background_contrast,
                    "effect_subject_intrusion": report.effect_subject_intrusion_ratio,
                    "semantic_fidelity": report.semantic_fidelity_score,
                    "missing_elements": list(report.missing_required_elements),
                    "layers": report.layer_count,
                    "category": plan.metadata.get("prompt_category"),
                    "brush_contract": brush_contract,
                    "palette_contract": palette_contract,
                    "semantic_contract": semantic_contract,
                    "fallback_paint_calls": report.estimated_paint_calls,
                    "issues": report.issues,
                }
            )
    print(json.dumps(rows, ensure_ascii=True, indent=2))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
