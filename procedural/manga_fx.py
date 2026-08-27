"""マンガ・アニメ効果線（迫力の集中線、流線、カケアミ、魔法陣）のプロシージャル生成エンジン。"""

from __future__ import annotations

import math
import random
import uuid

from ..domain import Stroke
from .base import create_stroke
from .semantic_budget import SemanticStrokeGroup, allocate_semantic_groups


def generate_manga_fx_strokes(
    prompt: str,
    seed: int,
    count: int | None,
    width: float,
    height: float,
) -> list[Stroke]:
    """マンガ・アニメ効果線（集中線、流線、カケアミ、魔法陣）ストロークを生成する。"""
    rng = random.Random(seed)
    prompt_l = prompt.lower()
    strokes: list[Stroke] = []

    def uid(name: str, idx: int = 0) -> str:
        return str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/fx/{seed}/{name}/{idx}"))

    is_magic = any(k in prompt_l for k in ["magic", "magical", "spell", "魔法", "circle", "ルーン", "rune"])
    is_focus = any(k in prompt_l for k in ["focus", "radial", "集中線", "放射線"])
    is_speed = any(k in prompt_l for k in ["speed", "流線", "スピード", "motion"])
    is_hatch = any(k in prompt_l for k in ["hatch", "カケアミ", "ハッチング", "shadow", "トーン"])
    is_glow = any(k in prompt_l for k in ["glow", "aura", "radiant", "発光", "オーラ", "光芒"])

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

    elif is_focus:
        # =====================================================================
        # 迫力の集中線 (Focus / Radial Speed Lines - 中心抜け)
        # =====================================================================
        # 理想密度で生成してから意味グループ予算で間引く。要求本数に合わせて
        # 先に生成数を減らすと UUID と角度分布が品質段階ごとに変わってしまう。
        line_count = 40
        hole_r = scale * 0.20
        outer_r = scale * 0.72

        for i in range(line_count):
            ang = (i / line_count) * math.pi * 2.0 + rng.uniform(-0.03, 0.03)
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
                    # 黒い効果線を加算レイヤーへ置くと明るい背景で消えるため、
                    # 通常合成の Lineart として重ねる。
                    layer_name="Lineart",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("focus_line", i),
                )
            )

    elif is_speed:
        # =====================================================================
        # 流線・スピード線 (Speed Lines)
        # =====================================================================
        line_count = 35
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

    elif is_glow:
        # =====================================================================
        # 発光オーラ (Aura / Glow) - 加算合成向けの不均一な多重光輪と粒子
        # =====================================================================
        ring_count = 6
        for ring_index in range(ring_count):
            radius_x = scale * (0.28 + ring_index * 0.026)
            radius_y = radius_x * (1.12 + ring_index * 0.015)
            ring_points: list[tuple[float, float]] = []
            for segment in range(33):
                angle = math.tau * segment / 32
                ripple = 1.0 + 0.025 * math.sin(angle * 3.0 + ring_index)
                ring_points.append(
                    (
                        cx + math.cos(angle) * radius_x * ripple,
                        cy + math.sin(angle) * radius_y * ripple,
                    )
                )
            strokes.append(
                create_stroke(
                    ring_points,
                    profile_type="airbrush",
                    base_pressure=0.45 + ring_index * 0.05,
                    color="#56e8ff" if ring_index % 2 == 0 else "#ff72d2",
                    size_px=10.0 + ring_index * 1.8,
                    layer_name="FX",
                    opacity=max(0.22, 0.58 - ring_index * 0.055),
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("glow_ring", ring_index),
                )
            )
        for particle_index in range(12):
            angle = math.tau * particle_index / 12 + rng.uniform(-0.12, 0.12)
            radius = scale * rng.uniform(0.32, 0.45)
            length = scale * rng.uniform(0.010, 0.025)
            px = cx + math.cos(angle) * radius
            py = cy + math.sin(angle) * radius * 1.08
            strokes.append(
                create_stroke(
                    [(px - length, py), (px + length, py)],
                    profile_type="airbrush",
                    base_pressure=0.75,
                    color="#ffffff",
                    size_px=rng.uniform(5.0, 10.0),
                    layer_name="Highlights",
                    opacity=0.75,
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("glow_particle", particle_index),
                )
            )

    else:
        # =====================================================================
        # 指定が曖昧なFXは、既存互換として中心抜けの集中線を生成する。
        # =====================================================================
        line_count = 40
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
                    layer_name="Lineart",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("focus_line", i),
                )
            )

    groups = semantic_groups_for_manga_fx(prompt, tuple(strokes))
    return allocate_semantic_groups(groups, count)


def semantic_groups_for_manga_fx(prompt: str, strokes: tuple[Stroke, ...]) -> tuple[SemanticStrokeGroup, ...]:
    """生成済みFXを、予算配分可能な視覚部品へ分解する。"""
    if not strokes:
        return ()
    prompt_l = prompt.casefold()
    is_magic = any(k in prompt_l for k in ["magic", "magical", "spell", "魔法", "circle", "ルーン", "rune"])
    is_focus = any(k in prompt_l for k in ["focus", "radial", "集中線", "放射線"])
    is_speed = any(k in prompt_l for k in ["speed", "流線", "スピード", "motion"])
    is_hatch = any(k in prompt_l for k in ["hatch", "カケアミ", "ハッチング", "shadow", "トーン"])
    is_glow = any(k in prompt_l for k in ["glow", "aura", "radiant", "発光", "オーラ", "光芒"])

    groups: tuple[SemanticStrokeGroup, ...]
    if is_magic:
        groups = (
            SemanticStrokeGroup("magic-rings", tuple(strokes[:5]), "frame", priority=90, minimum_count=3),
            SemanticStrokeGroup("magic-symbol", tuple(strokes[5:7]), "subject", priority=100, atomic=True),
            SemanticStrokeGroup("magic-rays", tuple(strokes[7:]), "accent", priority=60, minimum_count=1),
        )
    elif is_focus or is_speed:
        groups = (
            SemanticStrokeGroup(
                "focus-lines" if is_focus else "speed-lines",
                tuple(strokes),
                "motion",
                priority=80,
                minimum_count=min(8, len(strokes)),
            ),
        )
    elif is_hatch:
        groups = (
            SemanticStrokeGroup("hatch-forward", tuple(strokes[:8]), "shading", priority=80, minimum_count=1),
            SemanticStrokeGroup("hatch-backward", tuple(strokes[8:]), "shading", priority=80, minimum_count=1),
        )
    elif is_glow:
        groups = (
            SemanticStrokeGroup("glow-aura", tuple(strokes[:6]), "subject", priority=90, minimum_count=3),
            SemanticStrokeGroup("glow-particles", tuple(strokes[6:]), "accent", priority=60, minimum_count=2),
        )
    else:
        groups = (
            SemanticStrokeGroup(
                "focus-lines",
                tuple(strokes),
                "motion",
                priority=80,
                minimum_count=min(8, len(strokes)),
            ),
        )
    return groups
