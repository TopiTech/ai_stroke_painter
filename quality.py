"""DrawingPlan の視覚的な健全性を依存ライブラリなしで定量評価する。"""

from __future__ import annotations

from dataclasses import dataclass
import math
from statistics import fmean, pstdev

from .domain import LAYER_RENDER_ORDER, DrawingPlan, Stroke, split_color_alpha


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
    visual_score: float = 0.0
    value_range: float = 0.0
    dominant_color_ratio: float = 1.0
    horizontal_banding_score: float = 0.0
    vertical_banding_score: float = 0.0
    edge_density: float = 0.0
    significant_color_count: int = 0
    subject_background_contrast: float = 1.0
    effect_subject_intrusion_ratio: float = 0.0
    semantic_fidelity_score: float = 1.0
    missing_required_elements: tuple[str, ...] = ()
    incomplete_semantic_groups: tuple[str, ...] = ()
    quality_version: int = 2


@dataclass(frozen=True)
class _RasterMetrics:
    coverage: float
    value_range: float
    dominant_color_ratio: float
    horizontal_banding_score: float
    vertical_banding_score: float
    edge_density: float
    significant_color_count: int
    subject_background_contrast: float
    effect_subject_intrusion_ratio: float
    has_subject_region: bool
    has_subject_effects: bool


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


def _parse_rgb(color: str) -> tuple[float, float, float]:
    rgb, _alpha = split_color_alpha(color)
    raw = rgb.lstrip("#")
    if len(raw) == 3:
        raw = "".join(character * 2 for character in raw)
    try:
        return tuple(int(raw[index : index + 2], 16) / 255.0 for index in (0, 2, 4))  # type: ignore[return-value]
    except (TypeError, ValueError):
        return 0.137, 0.137, 0.137


def _paint_disc(
    red: list[float],
    green: list[float],
    blue: list[float],
    alpha: list[float],
    *,
    grid_size: int,
    center_x: float,
    center_y: float,
    radius: float,
    color: tuple[float, float, float],
    opacity: float,
    is_eraser: bool,
) -> None:
    x0 = max(0, math.floor(center_x - radius - 0.5))
    x1 = min(grid_size - 1, math.ceil(center_x + radius + 0.5))
    y0 = max(0, math.floor(center_y - radius - 0.5))
    y1 = min(grid_size - 1, math.ceil(center_y + radius + 0.5))
    safe_radius = max(0.35, radius)
    for grid_y in range(y0, y1 + 1):
        for grid_x in range(x0, x1 + 1):
            distance = math.hypot((grid_x + 0.5) - center_x, (grid_y + 0.5) - center_y)
            coverage = max(0.0, min(1.0, safe_radius + 0.65 - distance))
            source_alpha = opacity * coverage
            if source_alpha <= 0.0:
                continue
            pixel_index = grid_y * grid_size + grid_x
            if is_eraser:
                alpha[pixel_index] *= 1.0 - source_alpha
                continue
            destination_alpha = alpha[pixel_index]
            output_alpha = source_alpha + destination_alpha * (1.0 - source_alpha)
            if output_alpha <= 1e-9:
                continue
            red[pixel_index] = (
                color[0] * source_alpha + red[pixel_index] * destination_alpha * (1.0 - source_alpha)
            ) / output_alpha
            green[pixel_index] = (
                color[1] * source_alpha + green[pixel_index] * destination_alpha * (1.0 - source_alpha)
            ) / output_alpha
            blue[pixel_index] = (
                color[2] * source_alpha + blue[pixel_index] * destination_alpha * (1.0 - source_alpha)
            ) / output_alpha
            alpha[pixel_index] = output_alpha


def _rasterize_layer(
    strokes: list[Stroke], width: float, height: float, grid_size: int
) -> tuple[list[float], list[float], list[float], list[float]]:
    pixel_count = grid_size * grid_size
    red = [0.0] * pixel_count
    green = [0.0] * pixel_count
    blue = [0.0] * pixel_count
    alpha = [0.0] * pixel_count
    scale_x = grid_size / width
    scale_y = grid_size / height
    brush_scale = math.sqrt(scale_x * scale_y)
    for stroke in strokes:
        color = _parse_rgb(stroke.color)
        _rgb, color_alpha = split_color_alpha(stroke.color)
        opacity = max(0.0, min(1.0, stroke.opacity * color_alpha))
        radius = max(0.35, stroke.size_px * brush_scale * 0.5)
        for first, second in zip(stroke.points, stroke.points[1:], strict=False):
            first_x = first.x * scale_x
            first_y = first.y * scale_y
            second_x = second.x * scale_x
            second_y = second.y * scale_y
            distance = math.hypot(second_x - first_x, second_y - first_y)
            sample_count = max(1, min(256, math.ceil(distance / max(0.4, radius * 0.45))))
            for sample_index in range(sample_count + 1):
                phase = sample_index / sample_count
                pressure = first.pressure + (second.pressure - first.pressure) * phase
                _paint_disc(
                    red,
                    green,
                    blue,
                    alpha,
                    grid_size=grid_size,
                    center_x=first_x + (second_x - first_x) * phase,
                    center_y=first_y + (second_y - first_y) * phase,
                    radius=max(0.35, radius * pressure),
                    color=color,
                    opacity=opacity,
                    is_eraser=stroke.is_eraser,
                )
    return red, green, blue, alpha


def _composite_raster(
    plan: DrawingPlan, width: float, height: float, grid_size: int
) -> tuple[list[tuple[float, float, float]], list[float]]:
    by_layer: dict[str, list[Stroke]] = {}
    for stroke in plan.strokes:
        by_layer.setdefault(stroke.layer_name, []).append(stroke)
    original_order = {layer: index for index, layer in enumerate(by_layer)}
    layer_names = sorted(
        by_layer,
        key=lambda layer: (LAYER_RENDER_ORDER.get(layer, 35), original_order[layer]),
    )
    # オーバーレイは既存作品へ重ねる用途なので、白紙だけで評価すると加算色が
    # 全て白へ飽和する。中間灰のテストプレート上で可視性を測る。
    base_value = 0.5 if plan.metadata.get("overlay", False) is True else 1.0
    pixels = [(base_value, base_value, base_value) for _ in range(grid_size * grid_size)]
    painted_alpha = [0.0] * (grid_size * grid_size)
    for layer_name in layer_names:
        red, green, blue, alpha = _rasterize_layer(by_layer[layer_name], width, height, grid_size)
        lowered = layer_name.casefold()
        for index, destination in enumerate(pixels):
            source_alpha = alpha[index]
            if source_alpha <= 0.0:
                continue
            source = (red[index], green[index], blue[index])
            if "shading" in lowered or "shadow" in lowered:
                blended = tuple(destination[channel] * source[channel] for channel in range(3))
            elif any(token in lowered for token in ("highlight", "fx", "glow")):
                blended = tuple(1.0 - (1.0 - destination[channel]) * (1.0 - source[channel]) for channel in range(3))
            else:
                blended = source
            pixels[index] = (
                destination[0] * (1.0 - source_alpha) + blended[0] * source_alpha,
                destination[1] * (1.0 - source_alpha) + blended[1] * source_alpha,
                destination[2] * (1.0 - source_alpha) + blended[2] * source_alpha,
            )
            painted_alpha[index] = source_alpha + painted_alpha[index] * (1.0 - source_alpha)
    return pixels, painted_alpha


def _percentile(values: list[float], phase: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    position = max(0.0, min(1.0, phase)) * (len(ordered) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def _axis_banding(groups: list[list[float]]) -> float:
    if len(groups) < 3:
        return 0.0
    means = [fmean(group) for group in groups]
    deviations = [pstdev(group) if len(group) > 1 else 0.0 for group in groups]
    suspicious_transitions = sum(
        1
        for index in range(1, len(groups))
        if abs(means[index] - means[index - 1]) >= 0.035 and max(deviations[index], deviations[index - 1]) <= 0.10
    )
    return min(1.0, suspicious_transitions / max(2.0, len(groups) * 0.18))


def _normalized_box(value: object) -> tuple[float, float, float, float] | None:
    if not isinstance(value, (list, tuple)) or len(value) != 4:
        return None
    if any(isinstance(item, bool) or not isinstance(item, (int, float)) for item in value):
        return None
    x0, y0, x1, y1 = (float(item) for item in value)
    if not (0.0 <= x0 < x1 <= 1.0 and 0.0 <= y0 < y1 <= 1.0):
        return None
    return x0, y0, x1, y1


def _box_indices(box: tuple[float, float, float, float], grid_size: int) -> list[int]:
    x0, y0, x1, y1 = box
    return [
        row * grid_size + column
        for row in range(grid_size)
        for column in range(grid_size)
        if x0 <= (column + 0.5) / grid_size <= x1 and y0 <= (row + 0.5) / grid_size <= y1
    ]


def _raster_metrics(plan: DrawingPlan, width: float, height: float, grid_size: int) -> _RasterMetrics:
    pixels, painted_alpha = _composite_raster(plan, width, height, grid_size)
    luminances = [0.2126 * red + 0.7152 * green + 0.0722 * blue for red, green, blue in pixels]
    value_range = _percentile(luminances, 0.95) - _percentile(luminances, 0.05)
    histogram: dict[tuple[int, int, int], int] = {}
    for red, green, blue in pixels:
        quantized = (round(red * 7), round(green * 7), round(blue * 7))
        histogram[quantized] = histogram.get(quantized, 0) + 1
    dominant_color_ratio = max(histogram.values(), default=len(pixels)) / max(1, len(pixels))
    significant_color_count = sum(count >= len(pixels) * 0.01 for count in histogram.values())

    rows = [luminances[row * grid_size : (row + 1) * grid_size] for row in range(grid_size)]
    columns = [[luminances[row * grid_size + column] for row in range(grid_size)] for column in range(grid_size)]
    horizontal_banding = _axis_banding(rows)
    vertical_banding = _axis_banding(columns)

    edge_count = 0
    edge_total = 0
    for row in range(grid_size):
        for column in range(grid_size):
            index = row * grid_size + column
            if column + 1 < grid_size:
                edge_total += 1
                edge_count += abs(luminances[index] - luminances[index + 1]) >= 0.06
            if row + 1 < grid_size:
                edge_total += 1
                edge_count += abs(luminances[index] - luminances[index + grid_size]) >= 0.06

    composition = plan.metadata.get("composition_plan", {})
    scene_spec = plan.metadata.get("scene_spec", {})
    subjects = scene_spec.get("subjects", ()) if isinstance(scene_spec, dict) else ()
    has_subject_region = isinstance(subjects, (list, tuple)) and bool(subjects)
    primary_box = _normalized_box(composition.get("primary_box")) if isinstance(composition, dict) else None
    safe_box = _normalized_box(composition.get("subject_safe_box")) if isinstance(composition, dict) else None
    subject_background_contrast = 1.0
    if has_subject_region and primary_box is not None and safe_box is not None:
        subject_indices = _box_indices(safe_box, grid_size)
        primary_indices = set(_box_indices(primary_box, grid_size))
        background_indices = [index for index in range(len(luminances)) if index not in primary_indices]
        if not background_indices:
            safe_indices = set(subject_indices)
            background_indices = [index for index in range(len(luminances)) if index not in safe_indices]
        if subject_indices and background_indices:
            subject_values = [luminances[index] for index in subject_indices]
            background_values = [luminances[index] for index in background_indices]
            mean_separation = abs(fmean(subject_values) - fmean(background_values))
            # 明部と暗部を併せ持つ主役は平均値だけだと相殺される。階調分布の距離も
            # 測ることで、背景の中間調に対する輪郭・ハイライトの分離を拾う。
            quantile_separation = fmean(
                abs(_percentile(subject_values, phase) - _percentile(background_values, phase))
                for phase in (0.10, 0.25, 0.50, 0.75, 0.90)
            )
            subject_background_contrast = max(mean_separation, quantile_separation)

    stroke_node_map = plan.metadata.get("stroke_node_map", {})
    effect_ids = (
        {
            stroke_id
            for stroke_id, node_id in stroke_node_map.items()
            if isinstance(stroke_id, str) and isinstance(node_id, str) and node_id.startswith("effect-")
        }
        if isinstance(stroke_node_map, dict)
        else set()
    )
    effect_strokes = [stroke for stroke in plan.strokes if stroke.id in effect_ids]
    has_subject_effects = has_subject_region and bool(effect_strokes) and safe_box is not None
    effect_subject_intrusion = 0.0
    if has_subject_effects and safe_box is not None:
        _red, _green, _blue, effect_alpha = _rasterize_layer(effect_strokes, width, height, grid_size)
        effect_safe_indices = _box_indices(safe_box, grid_size)
        effect_subject_intrusion = (
            sum(effect_alpha[index] >= 0.08 for index in effect_safe_indices) / len(effect_safe_indices)
            if effect_safe_indices
            else 0.0
        )
    return _RasterMetrics(
        coverage=sum(alpha >= 0.08 for alpha in painted_alpha) / max(1, len(painted_alpha)),
        value_range=max(0.0, min(1.0, value_range)),
        dominant_color_ratio=max(0.0, min(1.0, dominant_color_ratio)),
        horizontal_banding_score=horizontal_banding,
        vertical_banding_score=vertical_banding,
        edge_density=edge_count / max(1, edge_total),
        significant_color_count=significant_color_count,
        subject_background_contrast=max(0.0, min(1.0, subject_background_contrast)),
        effect_subject_intrusion_ratio=max(0.0, min(1.0, effect_subject_intrusion)),
        has_subject_region=has_subject_region,
        has_subject_effects=has_subject_effects,
    )


def _line_cleanliness(plan: DrawingPlan, fragment_ratio: float) -> float:
    lineart_strokes = [stroke for stroke in plan.strokes if stroke.layer_name.casefold() == "lineart"]
    if not lineart_strokes:
        return 1.0
    sharp_reversals = 0
    evaluated_corners = 0
    for stroke in lineart_strokes:
        for first, middle, last in zip(stroke.points, stroke.points[1:], stroke.points[2:], strict=False):
            incoming = (middle.x - first.x, middle.y - first.y)
            outgoing = (last.x - middle.x, last.y - middle.y)
            incoming_length = math.hypot(*incoming)
            outgoing_length = math.hypot(*outgoing)
            if incoming_length <= 1e-6 or outgoing_length <= 1e-6:
                sharp_reversals += 1
                evaluated_corners += 1
                continue
            cosine = (incoming[0] * outgoing[0] + incoming[1] * outgoing[1]) / (incoming_length * outgoing_length)
            sharp_reversals += cosine < -0.5
            evaluated_corners += 1
    reversal_ratio = sharp_reversals / max(1, evaluated_corners)
    return max(0.0, min(1.0, 1.0 - fragment_ratio * 0.6 - reversal_ratio * 0.4))


def evaluate_plan_quality(plan: DrawingPlan, *, grid_size: int = 40) -> PlanQualityReport:
    """構造契約と低解像度合成結果の双方から、計画の健全性を評価する。"""
    if not isinstance(plan, DrawingPlan):
        raise TypeError("plan は DrawingPlan である必要があります")
    if isinstance(grid_size, bool) or not isinstance(grid_size, int) or not 16 <= grid_size <= 128:
        raise ValueError("grid_size は16から128の整数である必要があります")
    width, height = _canvas_dimensions(plan)
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
    raster = _raster_metrics(plan, width, height, grid_size)
    coverage = raster.coverage if plan.strokes else 0.0
    coverage_target = 0.12 if overlay else 0.80
    layer_target = 1.0 if overlay else 4.0
    dynamic_component = 1.0 if overlay or not expressive else min(1.0, dynamic_ratio / 0.25)
    structural_score = (
        min(1.0, coverage / coverage_target) * 0.40
        + min(1.0, layer_count / layer_target) * 0.20
        + dynamic_component * 0.20
        + (1.0 - fragment_ratio) * 0.10
        + (0.10 if out_of_bounds == 0 else 0.0)
    )
    line_cleanliness = _line_cleanliness(plan, fragment_ratio)
    contrast_component = min(1.0, raster.value_range / (0.18 if overlay else 0.32))
    dominance_component = max(0.0, min(1.0, (0.98 - raster.dominant_color_ratio) / 0.28))
    banding_component = 1.0 - max(raster.horizontal_banding_score, raster.vertical_banding_score)
    complexity_component = min(1.0, raster.edge_density / (0.025 if overlay else 0.055))
    visual_score = (
        contrast_component * 0.30
        + dominance_component * 0.20
        + banding_component * 0.20
        + complexity_component * 0.20
        + line_cleanliness * 0.10
    )
    if raster.has_subject_region:
        local_contrast_component = min(1.0, raster.subject_background_contrast / 0.10)
        visual_score = visual_score * 0.90 + local_contrast_component * 0.10
    if raster.has_subject_effects:
        intrusion_component = max(0.0, 1.0 - raster.effect_subject_intrusion_ratio / 0.20)
        visual_score = visual_score * 0.92 + intrusion_component * 0.08
    # composer が予算配分後に記録した要素から、必須要素の充足率を再計算する。
    raw_required = plan.metadata.get("required_elements", ())
    raw_rendered = plan.metadata.get("rendered_elements", ())
    required_elements = (
        tuple(dict.fromkeys(element for element in raw_required if isinstance(element, str) and element))
        if isinstance(raw_required, (list, tuple))
        else ()
    )
    rendered_elements = (
        {element for element in raw_rendered if isinstance(element, str) and element}
        if isinstance(raw_rendered, (list, tuple))
        else set()
    )
    missing_required = tuple(element for element in required_elements if element not in rendered_elements)
    semantic_fidelity = (
        (len(required_elements) - len(missing_required)) / len(required_elements) if required_elements else 1.0
    )
    raw_manifest = plan.metadata.get("semantic_manifest", ())
    incomplete_groups = (
        tuple(
            str(entry.get("node_id"))
            for entry in raw_manifest
            if isinstance(entry, dict)
            and entry.get("required_for")
            and entry.get("complete") is not True
            and isinstance(entry.get("node_id"), str)
        )
        if isinstance(raw_manifest, (list, tuple))
        else ()
    )

    # 構造だけで全面単色が、見た目だけで別題材が満点にならないよう三者を合議する。
    appearance_score = structural_score * 0.65 + visual_score * 0.35
    score = appearance_score if not required_elements else appearance_score * 0.75 + semantic_fidelity * 0.25
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
    if not overlay and raster.value_range < 0.07:
        issues.append("明度差が小さく、主役と背景を識別しにくい可能性があります")
    if not overlay and raster.dominant_color_ratio > 0.97:
        issues.append("単一色が画面の大半を占め、形状情報が不足しています")
    scene_spec = plan.metadata.get("scene_spec", {})
    scene_domain = str(scene_spec.get("primary_domain", "")) if isinstance(scene_spec, dict) else ""
    allows_regular_grid = scene_domain == "geometry" or plan.metadata.get("prompt_category") == "geometry"
    if not allows_regular_grid and max(raster.horizontal_banding_score, raster.vertical_banding_score) > 0.90:
        issues.append("均一な帯状パターンが多く、塗りの縞が目立つ可能性があります")
    if raster.edge_density < (0.001 if overlay else 0.002):
        issues.append("視覚的な形状境界がほとんど検出できません")
    if missing_required:
        issues.append(f"必須要素が描画予算内で満たされていません: {', '.join(missing_required)}")
    if raster.has_subject_region and raster.subject_background_contrast < 0.035:
        issues.append("主役領域と背景の局所明度差が不足しています")
    if raster.has_subject_effects and raster.effect_subject_intrusion_ratio > 0.20:
        issues.append("効果線が主役の安全領域へ入りすぎています")

    # 色数だけで高得点にせず、支配色、明度幅、実際に面積を持つ色数を併用する。
    palette_economy = max(0.0, min(1.0, 1.0 - max(0, raster.significant_color_count - 24) / 24.0))
    color_harmony = max(
        0.0,
        min(1.0, palette_economy * 0.35 + dominance_component * 0.30 + contrast_component * 0.35),
    )

    layer_sizes: dict[str, int] = {}
    for stroke in plan.strokes:
        layer_sizes[stroke.layer_name] = layer_sizes.get(stroke.layer_name, 0) + 1
    if overlay or len(layer_sizes) <= 1:
        layer_balance = 1.0
    else:
        total_strokes = sum(layer_sizes.values())
        entropy = -sum(
            (layer_size / total_strokes) * math.log(layer_size / total_strokes)
            for layer_size in layer_sizes.values()
            if layer_size > 0
        )
        layer_balance = entropy / math.log(len(layer_sizes))

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
        visual_score=round(max(0.0, min(1.0, visual_score)), 3),
        value_range=round(raster.value_range, 3),
        dominant_color_ratio=round(raster.dominant_color_ratio, 3),
        horizontal_banding_score=round(raster.horizontal_banding_score, 3),
        vertical_banding_score=round(raster.vertical_banding_score, 3),
        edge_density=round(raster.edge_density, 4),
        significant_color_count=raster.significant_color_count,
        subject_background_contrast=round(raster.subject_background_contrast, 3),
        effect_subject_intrusion_ratio=round(raster.effect_subject_intrusion_ratio, 3),
        semantic_fidelity_score=round(semantic_fidelity, 3),
        missing_required_elements=missing_required,
        incomplete_semantic_groups=incomplete_groups,
    )
