"""DrawingPlan の視覚的な健全性を依存ライブラリなしで定量評価する。"""

from __future__ import annotations

from dataclasses import dataclass
import math

from .domain import DrawingPlan, Stroke


@dataclass(frozen=True)
class PlanQualityReport:
    coverage: float
    layer_count: int
    dynamic_pressure_ratio: float
    fragment_ratio: float
    out_of_bounds_points: int
    estimated_paint_calls: int
    score: float
    issues: tuple[str, ...]
    color_harmony_score: float = 1.0
    line_cleanliness_score: float = 1.0
    layer_balance_score: float = 1.0


def _stroke_length(stroke: Stroke) -> float:
    return sum(
        math.hypot(second.x - first.x, second.y - first.y)
        for first, second in zip(stroke.points, stroke.points[1:], strict=False)
    )


def _canvas_dimensions(plan: DrawingPlan) -> tuple[float, float]:
    max_x = max((point.x for stroke in plan.strokes for point in stroke.points), default=1.5)
    max_y = max((point.y for stroke in plan.strokes for point in stroke.points), default=1.5)
    return (
        max(2.0, plan.canvas_width if plan.canvas_width is not None else max_x + 0.5),
        max(2.0, plan.canvas_height if plan.canvas_height is not None else max_y + 0.5),
    )


MAX_COVERAGE_RADIUS_CELLS = 4


def _estimated_coverage(plan: DrawingPlan, width: float, height: float, grid_size: int) -> float:
    occupied: set[tuple[int, int]] = set()
    cell_width = width / grid_size
    cell_height = height / grid_size
    diagonal = math.hypot(cell_width, cell_height)
    for stroke in plan.strokes:
        if stroke.is_eraser or stroke.opacity <= 0.02:
            continue
        raw_radius = math.ceil((stroke.size_px * 0.5) / min(cell_width, cell_height))
        radius_cells = max(0, min(MAX_COVERAGE_RADIUS_CELLS, min(grid_size, raw_radius)))
        for first, second in zip(stroke.points, stroke.points[1:], strict=False):
            length = math.hypot(second.x - first.x, second.y - first.y)
            sample_count = max(1, min(64, math.ceil(length / max(diagonal * 0.45, 1.0))))
            for sample in range(sample_count + 1):
                phase = sample / sample_count
                x = first.x + (second.x - first.x) * phase
                y = first.y + (second.y - first.y) * phase
                center_x = max(0, min(grid_size - 1, int(x / cell_width)))
                center_y = max(0, min(grid_size - 1, int(y / cell_height)))
                for offset_y in range(-radius_cells, radius_cells + 1):
                    grid_y = center_y + offset_y
                    if not 0 <= grid_y < grid_size:
                        continue
                    for offset_x in range(-radius_cells, radius_cells + 1):
                        grid_x = center_x + offset_x
                        if (
                            0 <= grid_x < grid_size
                            and offset_x * offset_x + offset_y * offset_y <= (radius_cells + 0.5) ** 2
                        ):
                            occupied.add((grid_x, grid_y))
    return len(occupied) / float(grid_size * grid_size)


def evaluate_plan_quality(plan: DrawingPlan, *, grid_size: int = 40) -> PlanQualityReport:
    """構図被覆、レイヤー、筆圧、断片化、境界、描画コストを同じ尺度で評価する。"""
    if not isinstance(plan, DrawingPlan):
        raise TypeError("plan は DrawingPlan である必要があります")
    if isinstance(grid_size, bool) or not isinstance(grid_size, int) or not 16 <= grid_size <= 128:
        raise ValueError("grid_size は16から128の整数である必要があります")
    width, height = _canvas_dimensions(plan)
    coverage = _estimated_coverage(plan, width, height, grid_size) if plan.strokes else 0.0
    # FX/Highlights の短い発光ダッシュや粒子は均一圧が意図的なので、主線と陰影だけを評価する。
    expressive = [stroke for stroke in plan.strokes if stroke.layer_name.lower() in {"lineart", "shading"}]
    dynamic_count = sum(
        1
        for stroke in expressive
        if len(stroke.points) >= 3
        and max(point.pressure for point in stroke.points) - min(point.pressure for point in stroke.points) >= 0.15
    )
    dynamic_ratio = dynamic_count / len(expressive) if expressive else 0.0
    fragment_count = sum(1 for stroke in plan.strokes if _stroke_length(stroke) < max(1.0, stroke.size_px * 0.35))
    fragment_ratio = fragment_count / len(plan.strokes) if plan.strokes else 1.0
    out_of_bounds = sum(
        1
        for stroke in plan.strokes
        for point in stroke.points
        if point.x < 0 or point.y < 0 or point.x >= width or point.y >= height
    )
    estimated_calls = sum(max(0, len(stroke.points) - 1) for stroke in plan.strokes)
    # 宣言だけの空レイヤーでは品質を水増しできないよう、実ストロークから数える。
    layer_count = len({stroke.layer_name for stroke in plan.strokes})
    overlay = plan.metadata.get("overlay", False) is True
    coverage_target = 0.12 if overlay else 0.80
    layer_target = 1.0 if overlay else 4.0
    dynamic_component = 1.0 if overlay or not expressive else min(1.0, dynamic_ratio / 0.25)
    score = (
        min(1.0, coverage / coverage_target) * 0.40
        + min(1.0, layer_count / layer_target) * 0.20
        + dynamic_component * 0.20
        + (1.0 - fragment_ratio) * 0.10
        + (0.10 if out_of_bounds == 0 else 0.0)
    )
    issues: list[str] = []
    if coverage < (0.05 if overlay else 0.35):
        issues.append("キャンバス被覆率が低く、白抜けの可能性があります")
    if not overlay and layer_count < 2:
        issues.append("単一レイヤーだけで奥行きの分離が不足しています")
    if not overlay and expressive and dynamic_ratio < 0.10:
        issues.append("線画・陰影の筆圧変化が不足しています")
    if fragment_ratio > 0.20:
        issues.append("短い断片ストロークが多すぎます")
    if out_of_bounds:
        issues.append("キャンバス外の点が含まれています")
    # 色彩調和度・線画クリーン度・レイヤーバランスの算出
    unique_colors = len({stroke.color.lower() for stroke in plan.strokes if not stroke.is_eraser})
    color_harmony = min(1.0, 0.5 + 0.5 * (unique_colors / 4.0)) if unique_colors > 0 else 0.5

    lineart_strokes = [s for s in plan.strokes if s.layer_name.lower() == "lineart"]
    if lineart_strokes:
        smooth_line_count = sum(1 for s in lineart_strokes if len(s.points) >= 3)
        line_cleanliness = min(1.0, 0.4 + 0.6 * (smooth_line_count / len(lineart_strokes)))
    else:
        line_cleanliness = 1.0

    layer_balance = min(1.0, layer_count / 3.0) if layer_count > 0 else 0.5

    return PlanQualityReport(
        coverage=coverage,
        layer_count=layer_count,
        dynamic_pressure_ratio=dynamic_ratio,
        fragment_ratio=fragment_ratio,
        out_of_bounds_points=out_of_bounds,
        estimated_paint_calls=estimated_calls,
        score=max(0.0, min(1.0, score)),
        issues=tuple(issues),
        color_harmony_score=round(color_harmony, 3),
        line_cleanliness_score=round(line_cleanliness, 3),
        layer_balance_score=round(layer_balance, 3),
    )
