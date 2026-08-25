"""本格プロシージャル・イラスト生成のための幾何計算、スプライン補間、筆圧プロファイル。"""

from __future__ import annotations

from dataclasses import replace
import math
import random
import uuid

from ..brushes import brush_preset_for_profile, canonical_brush_profile
from ..domain import Stroke, StrokePoint


def catmull_rom_spline(
    control_points: list[tuple[float, float]],
    samples_per_segment: int = 10,
) -> list[tuple[float, float]]:
    """Catmull-Rom スプライン補間により、制御点を通る滑らかな手描き風曲線を生成する。"""
    if len(control_points) < 2:
        return list(control_points)
    if len(control_points) == 2:
        p0, p1 = control_points
        return [
            (p0[0] + (p1[0] - p0[0]) * (i / samples_per_segment), p0[1] + (p1[1] - p0[1]) * (i / samples_per_segment))
            for i in range(samples_per_segment + 1)
        ]

    pts = [control_points[0]] + list(control_points) + [control_points[-1]]
    result: list[tuple[float, float]] = []

    for i in range(1, len(pts) - 2):
        p0, p1, p2, p3 = pts[i - 1], pts[i], pts[i + 1], pts[i + 2]
        for step in range(samples_per_segment if i < len(pts) - 3 else samples_per_segment + 1):
            t = step / samples_per_segment
            t2 = t * t
            t3 = t2 * t

            x = 0.5 * (
                (2 * p1[0])
                + (-p0[0] + p2[0]) * t
                + (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2
                + (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * t3
            )
            y = 0.5 * (
                (2 * p1[1])
                + (-p0[1] + p2[1]) * t
                + (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2
                + (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * t3
            )
            result.append((x, y))
    return result


def bezier_cubic(
    p0: tuple[float, float],
    p1: tuple[float, float],
    p2: tuple[float, float],
    p3: tuple[float, float],
    samples: int = 20,
) -> list[tuple[float, float]]:
    """3次ベジェ曲線から点列を生成。"""
    points: list[tuple[float, float]] = []
    for i in range(samples + 1):
        t = i / samples
        u = 1.0 - t
        x = u**3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t**3 * p3[0]
        y = u**3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t**3 * p3[1]
        points.append((x, y))
    return points


def _smoothstep(edge0: float, edge1: float, x: float) -> float:
    """エルミート補間によるなめらかな 0.0〜1.0 遷移関数。"""
    delta = edge1 - edge0
    if abs(delta) < 1e-9:
        return 0.0 if x < edge0 else 1.0
    # 抜き側では ``smoothstep(1.0, 0.84, t)`` のような降順区間を使う。
    # 分母を正数へ丸めると全区間が 0 になってしまうため、向きを保ったまま正規化する。
    t = max(0.0, min(1.0, (x - edge0) / delta))
    return t * t * (3.0 - 2.0 * t)


def pressure_profile(
    t: float,
    profile_type: str = "gpen",
    base: float = 0.8,
    rng: random.Random | None = None,
) -> float:
    """描画スタイルに応じた本格的な筆圧ダイナミクスを算出 (0.05〜1.0)。"""
    noise = rng.uniform(-0.02, 0.02) if rng is not None else 0.0
    ptype = profile_type.lower().strip()

    if ptype in {"gpen", "pen"}:
        taper_in = _smoothstep(0.0, 0.14, t)
        taper_out = _smoothstep(1.0, 0.84, t)
        curve = math.sin(t * math.pi) ** 0.75
        p = base * (0.15 + 0.85 * taper_in * taper_out * curve)
    elif ptype == "marupen":
        taper = min(_smoothstep(0.0, 0.09, t), _smoothstep(1.0, 0.91, t))
        p = base * (0.45 + 0.55 * taper)
    elif ptype in {"brush", "fude", "calligraphy"}:
        taper_in = _smoothstep(0.0, 0.18, t)
        taper_out = _smoothstep(1.0, 0.72, t)
        wave = 0.09 * math.sin(t * math.pi * 3.0)
        p = base * (0.25 + 0.75 * taper_in * taper_out) + wave
    elif ptype == "marker":
        taper = min(_smoothstep(0.0, 0.04, t), _smoothstep(1.0, 0.96, t))
        p = base * (0.85 + 0.15 * taper)
    elif ptype == "pencil":
        taper_in = _smoothstep(0.0, 0.08, t)
        taper_out = _smoothstep(1.0, 0.88, t)
        jitter = rng.uniform(-0.06, 0.06) if rng is not None else 0.0
        p = base * (0.4 + 0.6 * taper_in * taper_out) + jitter
    elif ptype == "watercolor":
        taper_in = _smoothstep(0.0, 0.25, t)
        taper_out = _smoothstep(1.0, 0.70, t)
        bleed = 0.05 * math.sin(t * math.pi * 5.0)
        p = base * (0.3 + 0.7 * taper_in * taper_out) + bleed
    elif ptype == "airbrush":
        p = base * math.sin(t * math.pi) ** 0.5
    else:  # soft / default
        p = base * math.sin(t * math.pi)

    return float(max(0.05, min(1.0, p + noise)))


def create_stroke(
    points_2d: list[tuple[float, float]],
    profile_type: str = "gpen",
    base_pressure: float = 0.8,
    color: str = "#232323",
    size_px: float = 6.0,
    layer_name: str = "Lineart",
    opacity: float = 1.0,
    brush_preset: str | None = None,
    rng: random.Random | None = None,
    width: float = 1000.0,
    height: float = 1000.0,
    stroke_id: str | None = None,
    preferred_profile: str | None = None,
) -> Stroke:
    """2D座標点列から筆圧付き Stroke を構築する。"""
    actual_profile = canonical_brush_profile(
        preferred_profile if (preferred_profile and preferred_profile != "auto") else profile_type
    )
    if len(points_2d) < 2:
        if len(points_2d) == 1:
            p_single = points_2d[0]
            points_2d = [p_single, (p_single[0] + 0.5, p_single[1] + 0.5)]
        else:
            points_2d = [(0.0, 0.0), (1.0, 1.0)]

    pts: list[StrokePoint] = []
    n = len(points_2d) - 1
    for i, (px, py) in enumerate(points_2d):
        t = i / max(1, n)
        press = pressure_profile(t, actual_profile, base_pressure, rng)
        bounded_x = max(0.0, min(width - 1.0, px))
        bounded_y = max(0.0, min(height - 1.0, py))
        pts.append(StrokePoint(bounded_x, bounded_y, press, i * 10))

    sid = stroke_id or str(uuid.uuid4())
    return Stroke(
        id=sid,
        points=pts,
        brush_preset=brush_preset or brush_preset_for_profile(actual_profile),
        color=color,
        size_px=size_px,
        layer_name=layer_name,
        opacity=opacity,
    )


def color_palette(name: str) -> dict[str, str]:
    """テーマ別カラーパレット定義。"""
    palettes = {
        "anime": {
            "draft": "#6ba3db",
            "lineart": "#282030",
            "skin_base": "#ffebe0",
            "skin_shadow": "#f5c5b5",
            "hair_main": "#e85d75",
            "hair_shadow": "#9e2a4b",
            "hair_highlight": "#ffd6de",
            "eye_dark": "#1a2a4b",
            "eye_light": "#4a90e2",
            "highlight": "#ffffff",
            "cloth_main": "#3f51b5",
            "cloth_shadow": "#283593",
            "fx": "#ffd700",
        },
        "monochrome": {
            "draft": "#88aacc",
            "lineart": "#1a1a1a",
            "skin_base": "#f0f0f0",
            "skin_shadow": "#a0a0a0",
            "hair_main": "#2b2b2b",
            "hair_shadow": "#111111",
            "hair_highlight": "#ffffff",
            "eye_dark": "#0a0a0a",
            "eye_light": "#666666",
            "highlight": "#ffffff",
            "cloth_main": "#444444",
            "cloth_shadow": "#1f1f1f",
            "fx": "#888888",
        },
        "cyberpunk": {
            "draft": "#00f0ff",
            "lineart": "#0a0614",
            "skin_base": "#fce4ec",
            "skin_shadow": "#ba68c8",
            "hair_main": "#00ffcc",
            "hair_shadow": "#008877",
            "hair_highlight": "#ffffff",
            "eye_dark": "#2a0845",
            "eye_light": "#ff007f",
            "highlight": "#00ffff",
            "cloth_main": "#2c003e",
            "cloth_shadow": "#150020",
            "fx": "#ff007f",
        },
        "nature": {
            "draft": "#a1c181",
            "lineart": "#2b2d42",
            "skin_base": "#fefae0",
            "skin_shadow": "#dda15e",
            "hair_main": "#606c38",
            "hair_shadow": "#283618",
            "hair_highlight": "#dda15e",
            "eye_dark": "#283618",
            "eye_light": "#bc6c25",
            "highlight": "#ffffff",
            "cloth_main": "#bc6c25",
            "cloth_shadow": "#8c4a16",
            "fx": "#e76f51",
        },
        "pastel": {
            "draft": "#b3cde0",
            "lineart": "#4a4e69",
            "skin_base": "#fff0f3",
            "skin_shadow": "#ffccd5",
            "hair_main": "#c8b6ff",
            "hair_shadow": "#9d8df1",
            "hair_highlight": "#ffffff",
            "eye_dark": "#3d348b",
            "eye_light": "#72efdd",
            "highlight": "#ffffff",
            "cloth_main": "#b8f2e6",
            "cloth_shadow": "#90e0ef",
            "fx": "#ffd166",
        },
        "watercolor": {
            "draft": "#90a4ae",
            "lineart": "#2c3e50",
            "skin_base": "#fdf2e9",
            "skin_shadow": "#f5cba7",
            "hair_main": "#5dade2",
            "hair_shadow": "#2e86c1",
            "hair_highlight": "#ebf5fb",
            "eye_dark": "#1b4f72",
            "eye_light": "#48c9b0",
            "highlight": "#ffffff",
            "cloth_main": "#a569bd",
            "cloth_shadow": "#7d3c98",
            "fx": "#f7dc6f",
        },
        "retro_pop": {
            "draft": "#00d2ff",
            "lineart": "#1a0826",
            "skin_base": "#ffeaa7",
            "skin_shadow": "#fab1a0",
            "hair_main": "#ff7675",
            "hair_shadow": "#d63031",
            "hair_highlight": "#fff275",
            "eye_dark": "#2d3436",
            "eye_light": "#00cec9",
            "highlight": "#ffffff",
            "cloth_main": "#6c5ce7",
            "cloth_shadow": "#4834d4",
            "fx": "#fdcb6e",
        },
        "dark_fantasy": {
            "draft": "#535c68",
            "lineart": "#130f40",
            "skin_base": "#f5f6fa",
            "skin_shadow": "#dcdde1",
            "hair_main": "#30336b",
            "hair_shadow": "#130f40",
            "hair_highlight": "#7ed6df",
            "eye_dark": "#191919",
            "eye_light": "#eb4d4b",
            "highlight": "#e056fd",
            "cloth_main": "#2c2c54",
            "cloth_shadow": "#1e1e38",
            "fx": "#f0932b",
        },
        "sepia": {
            "draft": "#bcaaa4",
            "lineart": "#3e2723",
            "skin_base": "#efebe9",
            "skin_shadow": "#d7ccc8",
            "hair_main": "#5d4037",
            "hair_shadow": "#3e2723",
            "hair_highlight": "#f5f5f5",
            "eye_dark": "#271610",
            "eye_light": "#8d6e63",
            "highlight": "#ffffff",
            "cloth_main": "#6d4c41",
            "cloth_shadow": "#4e342e",
            "fx": "#a1887f",
        },
        "botanical": {
            "draft": "#95d5b2",
            "lineart": "#1b4332",
            "skin_base": "#fdf0d5",
            "skin_shadow": "#ddb892",
            "hair_main": "#2d6a4f",
            "hair_shadow": "#081c15",
            "hair_highlight": "#b7e4c7",
            "eye_dark": "#1b4332",
            "eye_light": "#52b788",
            "highlight": "#ffffff",
            "cloth_main": "#ff758f",
            "cloth_shadow": "#c9184a",
            "fx": "#ffb703",
        },
        "sumie": {
            "draft": "#b0bec5",
            "lineart": "#111111",
            "skin_base": "#f9f7f1",
            "skin_shadow": "#cfd8dc",
            "hair_main": "#212121",
            "hair_shadow": "#000000",
            "hair_highlight": "#eceff1",
            "eye_dark": "#111111",
            "eye_light": "#424242",
            "highlight": "#ffffff",
            "cloth_main": "#37474f",
            "cloth_shadow": "#263238",
            "fx": "#c21807",
        },
        "cyber_gold": {
            "draft": "#00f0ff",
            "lineart": "#1a1a24",
            "skin_base": "#fff8e7",
            "skin_shadow": "#d4af37",
            "hair_main": "#ffd700",
            "hair_shadow": "#b8860b",
            "hair_highlight": "#ffffff",
            "eye_dark": "#0a192f",
            "eye_light": "#00f0ff",
            "highlight": "#ffffff",
            "cloth_main": "#1e293b",
            "cloth_shadow": "#0f172a",
            "fx": "#ff9f1c",
        },
    }
    key = name.lower().strip()
    return palettes.get(key, palettes["anime"])


def recolor_strokes_to_palette(strokes: list[Stroke], palette_name: str) -> list[Stroke]:
    """既存モチーフの明暗・色相関係を保ちながら、選択パレット内の色へ量子化する。"""

    def rgb(value: str) -> tuple[int, int, int] | None:
        raw = value.lstrip("#")
        if len(raw) in {3, 4}:
            raw = "".join(character * 2 for character in raw)
        if len(raw) not in {6, 8}:
            return None
        try:
            return int(raw[0:2], 16), int(raw[2:4], 16), int(raw[4:6], 16)
        except ValueError:
            return None

    candidates = tuple(dict.fromkeys(color_palette(palette_name).values()))
    candidate_rgb = tuple((candidate, rgb(candidate)) for candidate in candidates)
    recolored: list[Stroke] = []
    for stroke in strokes:
        source_rgb = rgb(stroke.color)
        if source_rgb is None:
            recolored.append(stroke)
            continue
        red, green, blue = source_rgb
        nearest = min(
            candidate_rgb,
            key=lambda item: (
                float("inf")
                if item[1] is None
                else 0.30 * (red - item[1][0]) ** 2 + 0.59 * (green - item[1][1]) ** 2 + 0.11 * (blue - item[1][2]) ** 2
            ),
        )[0]
        raw_source = stroke.color.lstrip("#")
        alpha = raw_source[-2:] if len(raw_source) == 8 else (raw_source[-1] * 2 if len(raw_source) == 4 else "")
        recolored.append(replace(stroke, color=nearest + alpha))
    return recolored


def sample_strokes_by_priority(strokes: list[Stroke], target_count: int | None) -> list[Stroke]:
    """優先度階層（下書き→輪郭→詳細→ハイライト）を維持しながら指定本数までダウンサンプリングする。
    target_count が None の場合はダウンサンプリングを行わず全ストロークを返す。
    target_count が 0 以下の場合は空リストを返す。
    """
    if not strokes:
        return []
    if target_count is not None and target_count <= 0:
        return []
    if target_count is None or target_count >= len(strokes):
        return list(strokes)

    # レイヤーごとの目標配分比率 (合計 1.0)
    layer_weights: dict[str, float] = {
        "Lineart": 0.45,
        "Flats": 0.25,
        "Shading": 0.15,
        "Highlights": 0.08,
        "Draft": 0.04,
        "FX": 0.03,
    }

    # レイヤーごとにストロークを分類
    by_layer: dict[str, list[Stroke]] = {}
    for s in strokes:
        by_layer.setdefault(s.layer_name, []).append(s)

    # 各レイヤーのサンプリング数を決定
    allocated_counts: dict[str, int] = {}

    # 1. 重み付けによる初期クォータ割り当て（最低1本確保を試みる）
    for layer, l_strokes in by_layer.items():
        weight = layer_weights.get(layer, 0.1)
        quota = max(1, min(len(l_strokes), round(target_count * weight)))
        allocated_counts[layer] = quota

    # 割り当て合計の調整
    total_allocated = sum(allocated_counts.values())
    if total_allocated > target_count:
        # 超過分を優先度の低いレイヤーから削減
        reduce_order = ["Draft", "FX", "Highlights", "Shading", "Flats", "Lineart"]
        diff = total_allocated - target_count
        for lyr in reduce_order:
            if lyr in allocated_counts and allocated_counts[lyr] > 1:
                sub = min(diff, allocated_counts[lyr] - 1)
                allocated_counts[lyr] -= sub
                diff -= sub
                if diff <= 0:
                    break
        # それでも超過している場合は均等にクリップ
        if sum(allocated_counts.values()) > target_count:
            excess = sum(allocated_counts.values()) - target_count
            for lyr in reduce_order:
                if lyr in allocated_counts and allocated_counts[lyr] > 0:
                    sub = min(excess, allocated_counts[lyr])
                    allocated_counts[lyr] -= sub
                    excess -= sub
                    if excess <= 0:
                        break
    elif total_allocated < target_count:
        # 不足分を優先度の高いレイヤーへ追加
        increase_order = ["Lineart", "Flats", "Shading", "Highlights", "Draft", "FX"]
        diff = target_count - total_allocated
        for lyr in increase_order:
            if lyr in allocated_counts and allocated_counts[lyr] < len(by_layer[lyr]):
                add = min(diff, len(by_layer[lyr]) - allocated_counts[lyr])
                allocated_counts[lyr] += add
                diff -= add
                if diff <= 0:
                    break

    # 2. 各レイヤーからストロークを均等サンプリング
    selected_set: set[str] = set()
    selected_strokes: list[Stroke] = []

    for layer, count in allocated_counts.items():
        l_strokes = by_layer[layer]
        if count >= len(l_strokes):
            for s in l_strokes:
                selected_set.add(s.id)
                selected_strokes.append(s)
        elif count > 0:
            step = len(l_strokes) / count
            for i in range(count):
                s = l_strokes[int(i * step)]
                if s.id not in selected_set:
                    selected_set.add(s.id)
                    selected_strokes.append(s)

    # 万が一 target_count に満たない場合は未選択のストロークで補完
    if len(selected_strokes) < target_count:
        for s in strokes:
            if s.id not in selected_set:
                selected_set.add(s.id)
                selected_strokes.append(s)
                if len(selected_strokes) >= target_count:
                    break

    # 元のストローク順序を維持してソート
    orig_index = {s.id: idx for idx, s in enumerate(strokes)}
    selected_strokes.sort(key=lambda s: orig_index.get(s.id, 0))
    return selected_strokes[:target_count]


def generate_ambient_occlusion_stroke(
    points: list[tuple[float, float]],
    color: str,
    *,
    size_px: float = 3.0,
    opacity: float = 0.85,
    layer_name: str = "Shading",
    stroke_id: str | None = None,
    width: float = 1000.0,
    height: float = 1000.0,
    rng: random.Random | None = None,
) -> Stroke:
    """接触面・隙間などの最も光が遮られるアンビエントオクルージョン（AO最暗部）ストロークを生成。"""
    spline = catmull_rom_spline(points, samples_per_segment=6) if len(points) >= 3 else points
    return create_stroke(
        spline,
        profile_type="marupen",
        base_pressure=0.85,
        color=color,
        size_px=size_px,
        layer_name=layer_name,
        opacity=opacity,
        rng=rng,
        width=width,
        height=height,
        stroke_id=stroke_id,
    )


def generate_cast_shadow_stroke(
    points: list[tuple[float, float]],
    color: str,
    *,
    size_px: float = 8.0,
    opacity: float = 0.45,
    layer_name: str = "Shading",
    profile_type: str = "airbrush",
    stroke_id: str | None = None,
    width: float = 1000.0,
    height: float = 1000.0,
    rng: random.Random | None = None,
) -> Stroke:
    """前髪や襟、衣服の重なりから下地へ落ちるソフトなキャストシャドウ（落ち影）ストロークを生成。"""
    spline = catmull_rom_spline(points, samples_per_segment=8) if len(points) >= 3 else points
    return create_stroke(
        spline,
        profile_type=profile_type,
        base_pressure=0.6,
        color=color,
        size_px=size_px,
        layer_name=layer_name,
        opacity=opacity,
        rng=rng,
        width=width,
        height=height,
        stroke_id=stroke_id,
    )


def generate_highlight_stroke(
    points: list[tuple[float, float]],
    color: str = "#ffffff",
    *,
    size_px: float = 3.5,
    opacity: float = 0.95,
    profile_type: str = "gpen",
    layer_name: str = "Highlights",
    stroke_id: str | None = None,
    width: float = 1000.0,
    height: float = 1000.0,
    rng: random.Random | None = None,
) -> Stroke:
    """鼻先、下唇、瞳、髪の稜線などのスペキュラハイライトストロークを生成。"""
    spline = catmull_rom_spline(points, samples_per_segment=6) if len(points) >= 3 else points
    return create_stroke(
        spline,
        profile_type=profile_type,
        base_pressure=0.9,
        color=color,
        size_px=size_px,
        layer_name=layer_name,
        opacity=opacity,
        rng=rng,
        width=width,
        height=height,
        stroke_id=stroke_id,
    )
