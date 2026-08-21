"""マンガ・アニメ効果線（迫力の集中線、流線、カケアミ、魔法陣）のプロシージャル生成エンジン。"""

from __future__ import annotations

import math
import random
import uuid

from ..domain import Stroke
from .base import create_stroke


def generate_manga_fx_strokes(
    prompt: str,
    seed: int,
    count: int,
    width: float,
    height: float,
) -> list[Stroke]:
    """マンガ・アニメ効果線（集中線、流線、カケアミ、魔法陣）ストロークを生成する。"""
    rng = random.Random(seed)
    prompt_l = prompt.lower()
    strokes: list[Stroke] = []

    def uid(name: str, idx: int = 0) -> str:
        return str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/fx/{seed}/{name}/{idx}"))

    is_magic = any(k in prompt_l for k in ["magic", "魔法", "circle", "ルーン", "rune", "エフェクト", "fx"])
    is_speed = any(k in prompt_l for k in ["speed", "流線", "スピード", "motion"])
    is_hatch = any(k in prompt_l for k in ["hatch", "カケアミ", "ハッチング", "shadow", "トーン"])

    cx = width * 0.5
    cy = height * 0.5
    scale = min(width, height)

    if is_magic:
        # =====================================================================
        # 魔法陣・ルーンエフェクト (Magic Circle & Runes)
        # =====================================================================
        # 1. 多重同心円 (Concentric Rings)
        rings = [0.45, 0.42, 0.35, 0.28, 0.15]
        for r_i, r_factor in enumerate(rings):
            r = scale * r_factor
            ring_pts: list[tuple[float, float]] = []
            segments = 32
            for s in range(segments + 1):
                ang = (s / segments) * math.pi * 2.0
                ring_pts.append((cx + math.cos(ang) * r, cy + math.sin(ang) * r))
            strokes.append(
                create_stroke(
                    ring_pts,
                    profile_type="marupen",
                    base_pressure=0.85,
                    color="#ffd700" if r_i % 2 == 0 else "#ffaa00",
                    size_px=4.0 if r_i in (0, 2) else 2.5,
                    layer_name="FX",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("magic_ring", r_i),
                )
            )

        # 2. 幾何学多角形・星型 (Hexagram / Geometric Stars)
        star_points = 6
        star_r = scale * 0.35
        for star_offset in [0.0, math.pi / star_points]:
            star_pts = []
            for s in range(star_points + 1):
                ang = star_offset + (s / star_points) * math.pi * 2.0
                star_pts.append((cx + math.cos(ang) * star_r, cy + math.sin(ang) * star_r))
            strokes.append(
                create_stroke(
                    star_pts,
                    profile_type="gpen",
                    base_pressure=0.8,
                    color="#00ffff",
                    size_px=3.5,
                    layer_name="FX",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("magic_star", int(star_offset * 10)),
                )
            )

        # 3. 放射状光芒 (Radiant Rays)
        ray_count = 12
        for ray_i in range(ray_count):
            ang = (ray_i / ray_count) * math.pi * 2.0
            r_inner = scale * 0.15
            r_outer = scale * 0.48
            ray_stroke = [
                (cx + math.cos(ang) * r_inner, cy + math.sin(ang) * r_inner),
                (cx + math.cos(ang) * r_outer, cy + math.sin(ang) * r_outer),
            ]
            strokes.append(
                create_stroke(
                    ray_stroke,
                    profile_type="gpen",
                    base_pressure=0.9,
                    color="#ffffff",
                    size_px=3.0,
                    layer_name="Highlights",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("magic_ray", ray_i),
                )
            )

    elif is_speed:
        # =====================================================================
        # 流線・スピード線 (Speed Lines)
        # =====================================================================
        line_count = min(count, 40)
        for i in range(line_count):
            y_base = (i / max(1, line_count - 1)) * height
            x_start = rng.uniform(0.0, width * 0.25)
            x_end = width * (0.75 + rng.uniform(0.1, 0.25))
            y_offset = rng.uniform(-height * 0.02, height * 0.02)
            pts = [(x_start, y_base + y_offset), (x_end, y_base + y_offset)]
            strokes.append(
                create_stroke(
                    pts,
                    profile_type="gpen",
                    base_pressure=rng.uniform(0.6, 1.0),
                    color="#1a1a1a",
                    size_px=rng.uniform(3.0, 7.0),
                    layer_name="Lineart",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("speed_line", i),
                )
            )

    elif is_hatch:
        # =====================================================================
        # カケアミ・クロスハッチング (Manga Tone & Cross-Hatching)
        # =====================================================================
        grid_steps = 8
        spacing = scale * 0.05
        # 斜め45度線
        for d1 in range(grid_steps):
            pts1 = [
                (cx - scale * 0.2 + d1 * spacing, cy - scale * 0.2),
                (cx - scale * 0.2 + d1 * spacing - scale * 0.15, cy + scale * 0.2),
            ]
            strokes.append(
                create_stroke(
                    pts1,
                    profile_type="marupen",
                    base_pressure=0.7,
                    color="#232323",
                    size_px=2.5,
                    layer_name="Shading",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("hatch_d1", d1),
                )
            )
        # 逆斜め45度線 (クロス)
        for d2 in range(grid_steps):
            pts2 = [
                (cx - scale * 0.35 + d2 * spacing, cy + scale * 0.2),
                (cx - scale * 0.35 + d2 * spacing + scale * 0.15, cy - scale * 0.2),
            ]
            strokes.append(
                create_stroke(
                    pts2,
                    profile_type="marupen",
                    base_pressure=0.7,
                    color="#232323",
                    size_px=2.5,
                    layer_name="Shading",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("hatch_d2", d2),
                )
            )

    else:
        # =====================================================================
        # 迫力の集中線 (Focus / Radial Speed Lines - 中心抜け)
        # =====================================================================
        line_count = min(count, 50)
        hole_r = scale * 0.20  # 中心部の抜け（キャラクターを配置するスペース）
        outer_r = scale * 0.72

        for i in range(line_count):
            ang = (i / line_count) * math.pi * 2.0 + rng.uniform(-0.03, 0.03)
            # 各線の長さ・太さにランダムなリズムをつける
            r_start = outer_r * rng.uniform(0.9, 1.1)
            r_end = hole_r * rng.uniform(1.0, 1.4)

            p_outer = (cx + math.cos(ang) * r_start, cy + math.sin(ang) * r_start)
            p_inner = (cx + math.cos(ang) * r_end, cy + math.sin(ang) * r_end)

            strokes.append(
                create_stroke(
                    [p_outer, p_inner],
                    profile_type="gpen",
                    base_pressure=rng.uniform(0.7, 1.0),
                    color="#1a1a1a",
                    size_px=rng.uniform(3.5, 8.0),
                    layer_name="FX",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("focus_line", i),
                )
            )

    if len(strokes) > count:
        step = len(strokes) / count
        chosen = [strokes[int(i * step)] for i in range(count)]
        return chosen
    return strokes
