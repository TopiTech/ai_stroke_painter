"""自然風景・山岳・波・樹木・雲・花のプロシージャル生成エンジン。"""

from __future__ import annotations

import math
import random
import uuid

from ..domain import Stroke
from .base import (
    bezier_cubic,
    catmull_rom_spline,
    color_palette,
    create_stroke,
    recolor_strokes_to_palette,
    sample_strokes_by_priority,
)


def landscape_feature_stroke_ids(prompt: str, seed: int) -> tuple[str, ...]:
    """風景種別ごとのシルエットと前中後景を保つ特徴を優先順で返す。"""

    prompt_l = prompt.casefold()

    def uid(name: str, index: int = 0) -> str:
        return str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/land/{seed}/{name}/{index}"))

    if any(keyword in prompt_l for keyword in ("wave", "波", "海", "ocean", "sea", "北斎", "hokusai")):
        return (
            uid("wave_arc", 0),
            uid("wave_arc", 2),
            uid("wave_arc", 4),
            uid("wave_arc", 6),
            uid("wave_claw", 0),
            uid("wave_claw", 6),
            uid("fuji"),
            uid("ripple", 0),
        )
    if any(
        keyword in prompt_l for keyword in ("wildflower", "wild flower", "garden", "meadow", "野花", "花畑", "庭園")
    ):
        return (
            uid("wildflower_stem", 0),
            uid("wildflower_stem", 7),
            uid("wildflower_stem", 14),
            uid("wildflower_petals_0", 0),
            uid("wildflower_petals_7", 0),
            uid("wildflower_center", 0),
            uid("meadow_ground"),
            uid("meadow_sky"),
        )
    if any(keyword in prompt_l for keyword in ("rose", "バラ", "薔薇")):
        return (
            uid("stem"),
            uid("leaf_l"),
            uid("leaf_r"),
            uid("rose_petal", 0),
            uid("rose_petal", 20),
            uid("rose_petal", 40),
            uid("rose_petal", 60),
            uid("rose_petal", 80),
        )
    is_sakura = any(keyword in prompt_l for keyword in ("sakura", "桜", "cherry blossom"))
    return (
        uid("mountain_ridge", 0),
        uid("mountain_ridge", 1),
        uid("tree_trunk"),
        uid("tree_bough", 0),
        uid("tree_bough", 1),
        uid("cloud", 0),
        uid("sakura_mass" if is_sakura else "foliage", 0),
        uid("ground_layer", 0),
    )


def generate_landscape_strokes(
    prompt: str,
    seed: int,
    count: int | None,
    width: float,
    height: float,
    palette_name: str = "nature",
) -> list[Stroke]:
    """自然風景（山・樹木・波・雲・花）の本格ストロークを生成する。"""
    rng = random.Random(seed)
    colors = color_palette(palette_name)
    prompt_l = prompt.lower()
    strokes: list[Stroke] = []

    def uid(name: str, idx: int = 0) -> str:
        return str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/land/{seed}/{name}/{idx}"))

    # "water" の部分一致は "watercolor" を波へ誤分類するため、波を示す明示語だけを使う。
    is_wave = any(k in prompt_l for k in ["wave", "波", "海", "ocean", "sea", "北斎", "hokusai"])
    is_rose = any(k in prompt_l for k in ["rose", "バラ", "薔薇"])
    is_sakura = any(k in prompt_l for k in ["sakura", "桜", "cherry blossom"])
    is_wildflower = any(
        k in prompt_l for k in ["wildflower", "wild flower", "garden", "meadow", "野花", "花畑", "庭園"]
    )

    if is_wave:
        # =====================================================================
        # 葛飾北斎風の大波 (The Great Wave)
        # =====================================================================
        cx = width * 0.45
        cy = height * 0.60
        scale = min(width, height)

        # 遠景の富士山
        fuji_pts = [
            (cx + scale * 0.15, cy - scale * 0.05),
            (cx + scale * 0.28, cy - scale * 0.22),
            (cx + scale * 0.32, cy - scale * 0.22),
            (cx + scale * 0.45, cy - scale * 0.05),
        ]
        strokes.append(
            create_stroke(
                catmull_rom_spline(fuji_pts, 8),
                profile_type="gpen",
                base_pressure=0.7,
                color=colors["lineart"],
                size_px=4.0,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid("fuji"),
            )
        )

        # 大波の主曲線 (Main Wave Arc)
        for w_i in range(8):
            w_off = w_i * scale * 0.03
            arc_pts = catmull_rom_spline(
                [
                    (0.0, height * 0.85 + w_off),
                    (scale * 0.25, height * 0.70 + w_off),
                    (scale * 0.45, height * 0.35 + w_off * 0.5),
                    (scale * 0.35, height * 0.18 + w_off * 0.2),
                    (scale * 0.22, height * 0.25 + w_off * 0.2),
                ],
                samples_per_segment=10,
            )
            strokes.append(
                create_stroke(
                    arc_pts,
                    profile_type="gpen",
                    base_pressure=0.9,
                    color="#1a4c80" if w_i % 2 == 0 else "#2e75b6",
                    size_px=6.0,
                    layer_name="Lineart",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("wave_arc", w_i),
                )
            )

        # 波頭の爪・しぶき (Wave Claws & Foam)
        claw_base_x = scale * 0.28
        claw_base_y = height * 0.20
        for c_i in range(12):
            ang = (c_i / 12) * math.pi * 0.8 - math.pi * 0.2
            c_len = scale * (0.05 + rng.uniform(0.01, 0.04))
            claw_pts = [
                (claw_base_x, claw_base_y),
                (claw_base_x + math.cos(ang) * c_len * 0.6, claw_base_y + math.sin(ang) * c_len * 0.6),
                (claw_base_x + math.cos(ang + 0.3) * c_len, claw_base_y + math.sin(ang + 0.3) * c_len),
            ]
            strokes.append(
                create_stroke(
                    catmull_rom_spline(claw_pts, 6),
                    profile_type="marupen",
                    base_pressure=0.85,
                    color="#ffffff",
                    size_px=4.5,
                    layer_name="Highlights",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("wave_claw", c_i),
                )
            )

        # 水面の波紋ストローク
        for r_i in range(10):
            rx = rng.uniform(0, width * 0.9)
            ry = height * 0.75 + rng.uniform(0, height * 0.2)
            rw = scale * rng.uniform(0.08, 0.18)
            ripple = [(rx, ry), (rx + rw * 0.5, ry - scale * 0.01), (rx + rw, ry)]
            strokes.append(
                create_stroke(
                    catmull_rom_spline(ripple, 6),
                    profile_type="brush",
                    base_pressure=0.6,
                    color="#70a4d4",
                    size_px=3.5,
                    layer_name="Flats",
                    opacity=0.7,
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("ripple", r_i),
                )
            )

    elif is_wildflower:
        # =====================================================================
        # 野花の水彩ガーデン。人物や一輪のバラへ流用せず、複数株の奥行きを作る。
        # =====================================================================
        strokes.extend(
            [
                create_stroke(
                    [(0.0, height * 0.28), (width, height * 0.28)],
                    profile_type="airbrush",
                    base_pressure=0.9,
                    color="#dceef2",
                    size_px=max(60.0, height * 0.42),
                    layer_name="Flats",
                    opacity=0.55,
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("meadow_sky"),
                ),
                create_stroke(
                    [(0.0, height * 0.78), (width, height * 0.78)],
                    profile_type="watercolor",
                    base_pressure=0.9,
                    color="#8fbc8f",
                    size_px=max(70.0, height * 0.45),
                    layer_name="Flats",
                    opacity=0.55,
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("meadow_ground"),
                ),
            ]
        )
        flower_colors = ["#d95d8a", "#e9a13a", "#8b6fc0", "#f2d15c", "#de7b72"]
        plant_count = 15
        for plant_index in range(plant_count):
            x = width * (0.08 + 0.84 * plant_index / max(1, plant_count - 1)) + rng.uniform(
                -width * 0.018, width * 0.018
            )
            base_y = height * rng.uniform(0.72, 0.96)
            stem_h = height * rng.uniform(0.16, 0.38)
            flower_y = base_y - stem_h
            stem = catmull_rom_spline(
                [(x, base_y), (x + rng.uniform(-10, 10), base_y - stem_h * 0.55), (x, flower_y)], 5
            )
            strokes.append(
                create_stroke(
                    stem,
                    profile_type="brush",
                    base_pressure=0.72,
                    color="#3f7f58",
                    size_px=max(2.0, min(width, height) * 0.0045),
                    layer_name="Lineart",
                    opacity=0.82,
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("wildflower_stem", plant_index),
                )
            )
            radius = min(width, height) * rng.uniform(0.018, 0.032)
            color = flower_colors[plant_index % len(flower_colors)]
            petals = 5 + plant_index % 3
            for petal_index in range(petals):
                angle = math.tau * petal_index / petals + rng.uniform(-0.10, 0.10)
                inner = (x + math.cos(angle) * radius * 0.18, flower_y + math.sin(angle) * radius * 0.18)
                outer = (x + math.cos(angle) * radius, flower_y + math.sin(angle) * radius)
                control = (
                    x + math.cos(angle + 0.28) * radius * 0.72,
                    flower_y + math.sin(angle + 0.28) * radius * 0.72,
                )
                strokes.append(
                    create_stroke(
                        [inner, control, outer],
                        profile_type="watercolor",
                        base_pressure=0.72,
                        color=color,
                        size_px=max(2.5, radius * 0.38),
                        layer_name="Lineart",
                        opacity=0.72,
                        rng=rng,
                        width=width,
                        height=height,
                        stroke_id=uid(f"wildflower_petals_{plant_index}", petal_index),
                    )
                )
            strokes.append(
                create_stroke(
                    [(x - radius * 0.18, flower_y), (x + radius * 0.18, flower_y)],
                    profile_type="marupen",
                    base_pressure=0.9,
                    color="#7b5a2c",
                    size_px=max(2.0, radius * 0.35),
                    layer_name="Highlights",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("wildflower_center", plant_index),
                )
            )

    elif is_rose:
        # =====================================================================
        # 花（バラ・桜の美しい有機的ストローク）
        # =====================================================================
        cx = width * 0.5
        cy = height * 0.45
        scale = min(width, height) * 0.7

        # バラの花弁の渦巻き螺旋 (Rose Petals Spiral)
        petal_layers = 6
        for p_layer in range(petal_layers):
            petals_in_layer = 4 + p_layer * 2
            r_layer = scale * (0.05 + p_layer * 0.06)
            for p_idx in range(petals_in_layer):
                base_ang = (p_idx / petals_in_layer) * math.pi * 2.0 + p_layer * 0.5
                p0 = (cx + math.cos(base_ang) * r_layer * 0.8, cy + math.sin(base_ang) * r_layer * 0.8)
                p1 = (
                    cx + math.cos(base_ang + 0.3) * r_layer * 1.2 + rng.uniform(-5, 5),
                    cy + math.sin(base_ang + 0.3) * r_layer * 1.2 + rng.uniform(-5, 5),
                )
                p2 = (
                    cx + math.cos(base_ang + 0.6) * r_layer * 1.1,
                    cy + math.sin(base_ang + 0.6) * r_layer * 1.1,
                )
                p3 = (cx + math.cos(base_ang + 0.9) * r_layer * 0.8, cy + math.sin(base_ang + 0.9) * r_layer * 0.8)

                petal = catmull_rom_spline([p0, p1, p2, p3], 8)
                color = "#d62246" if p_layer % 2 == 0 else "#e05370"
                strokes.append(
                    create_stroke(
                        petal,
                        profile_type="gpen",
                        base_pressure=0.9,
                        color=color,
                        size_px=5.0,
                        layer_name="Lineart",
                        rng=rng,
                        width=width,
                        height=height,
                        stroke_id=uid("rose_petal", p_layer * 20 + p_idx),
                    )
                )

        # 茎と葉 (Stem and Leaves)
        stem_pts = catmull_rom_spline(
            [(cx, cy + scale * 0.35), (cx - scale * 0.05, cy + scale * 0.60), (cx + scale * 0.02, height * 0.95)], 8
        )
        strokes.append(
            create_stroke(
                stem_pts,
                profile_type="gpen",
                base_pressure=0.95,
                color="#2d6a4f",
                size_px=7.0,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid("stem"),
            )
        )

        # 葉
        for l_side, l_name in [(-1.0, "l"), (1.0, "r")]:
            lx = cx + l_side * scale * 0.08
            ly = cy + scale * 0.55
            leaf_pts = catmull_rom_spline(
                [
                    (lx, ly),
                    (lx + l_side * scale * 0.15, ly - scale * 0.05),
                    (lx + l_side * scale * 0.22, ly + scale * 0.02),
                    (lx + l_side * scale * 0.10, ly + scale * 0.08),
                    (lx, ly + scale * 0.03),
                ],
                8,
            )
            strokes.append(
                create_stroke(
                    leaf_pts,
                    profile_type="gpen",
                    base_pressure=0.85,
                    color="#40916c",
                    size_px=5.0,
                    layer_name="Lineart",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid(f"leaf_{l_name}"),
                )
            )

    else:
        # =====================================================================
        # 山岳・樹木・雲・丘陵のフル風景 (Mountains, Trees, Clouds, Ground)
        # =====================================================================
        scale = min(width, height)
        is_fantasy = any(k in prompt_l for k in ["fantasy", "magic", "dream", "幻想", "魔法"])

        # 0. 空のグラデーション (Flats Layer - Sky Gradient) - 隙間なく地平線まで覆う
        sky_colors = (
            ["#223a5e", "#3a6094", "#5c8dc9", "#8bbbe8", "#eef6ff"]
            if not is_fantasy
            else ["#2c2459", "#4e3b7b", "#725b9c", "#a280b8", "#fce4ec"]
        )
        sky_limit_y = height * 0.58
        for s_idx, s_col in enumerate(sky_colors):
            s_y = (s_idx / max(1, len(sky_colors) - 1)) * sky_limit_y
            strokes.append(
                create_stroke(
                    [(-width * 0.05, s_y), (width * 0.5, s_y), (width * 1.05, s_y)],
                    profile_type="airbrush",
                    base_pressure=0.85,
                    color=s_col,
                    size_px=max(50.0, height * 0.20),
                    layer_name="Flats",
                    opacity=0.82,
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("sky_grad", s_idx),
                )
            )

        # 1. 遠景〜中景の山並み (Mountains with solid mass and organic ridges)
        mountain_colors = ["#5d6d7e", "#34495e", "#1f2d3d"] if not is_fantasy else ["#5c4a72", "#3e3256", "#251d38"]
        for m_layer in range(2):
            m_pts: list[tuple[float, float]] = []
            steps = 14
            base_y = height * (0.38 + m_layer * 0.12)
            m_pts.append((-width * 0.05, base_y))
            for s_i in range(1, steps):
                sx = (s_i / steps) * width
                sy = base_y - (rng.uniform(0.08, 0.24) * height) / (m_layer + 1.2)
                m_pts.append((sx, sy))
            m_pts.append((width * 1.05, base_y))
            m_spline = catmull_rom_spline(m_pts, 8)

            m_col = mountain_colors[m_layer % len(mountain_colors)]
            # 山体の面塗り (Flats) - 稜線直下・中腹の2段で白抜けを完全に防止
            for fb_idx, fb_frac in enumerate((0.15, 0.55)):
                band_pts = [(x, y + (base_y - y) * fb_frac) for x, y in m_spline]
                strokes.append(
                    create_stroke(
                        band_pts,
                        profile_type="watercolor",
                        base_pressure=0.88,
                        color=m_col,
                        size_px=max(35.0, scale * 0.10),
                        layer_name="Flats",
                        opacity=0.78 if fb_idx == 0 else 0.65,
                        rng=rng,
                        width=width,
                        height=height,
                        stroke_id=uid(f"mountain_body_{m_layer}", fb_idx),
                    )
                )

            # 山稜線の輪郭 (Lineart)
            strokes.append(
                create_stroke(
                    m_spline,
                    profile_type="gpen",
                    base_pressure=0.75,
                    color=["#2c3e50", "#1a252f"][m_layer],
                    size_px=max(2.5, 4.2 - m_layer * 1.0),
                    layer_name="Lineart",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("mountain_ridge", m_layer),
                )
            )

            # 山肌の有機的陰影 (Shading Layer)
            for h_i in range(4):
                hx = (h_i / 4) * width + width * 0.12
                hy = base_y - height * 0.08
                h_stroke = catmull_rom_spline(
                    [(hx, hy), (hx - width * 0.03, hy + height * 0.04), (hx - width * 0.05, hy + height * 0.08)],
                    6,
                )
                strokes.append(
                    create_stroke(
                        h_stroke,
                        profile_type="watercolor",
                        base_pressure=0.55,
                        color="#2c3e50",
                        size_px=max(4.0, scale * 0.012),
                        layer_name="Shading",
                        opacity=0.50,
                        rng=rng,
                        width=width,
                        height=height,
                        stroke_id=uid(f"mountain_shade_{m_layer}", h_i),
                    )
                )

        # 2. 中景の丘陵・大地のベース (Ground, Hills & Meadow - 白抜け根絶)
        ground_colors = ["#4a7c59", "#3b6e4c", "#2d5a3d"] if not is_fantasy else ["#4a6b5d", "#365345", "#253b30"]
        for g_layer in range(2):
            g_base_y = height * (0.54 + g_layer * 0.20)
            g_ctrls = [
                (-width * 0.05, g_base_y),
                (width * 0.35, g_base_y - scale * 0.04),
                (width * 0.70, g_base_y + scale * 0.02),
                (width * 1.05, g_base_y - scale * 0.02),
            ]
            g_spline = catmull_rom_spline(g_ctrls, 8)
            strokes.append(
                create_stroke(
                    g_spline,
                    profile_type="watercolor",
                    base_pressure=0.90,
                    color=ground_colors[g_layer % len(ground_colors)],
                    size_px=max(50.0, height * 0.25),
                    layer_name="Flats",
                    opacity=0.85,
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("ground_layer", g_layer),
                )
            )

        # 3. 前景の樹木 (Organic Tree Anatomy - 根張り・階層分岐・テーパー)
        tree_x = width * 0.68
        tree_y = height * 0.94
        tree_h = height * 0.52
        top_y = tree_y - tree_h
        trunk_color = "#3d2817"

        # 幹 (根張り・うねり・テーパー)
        trunk_ctrls = [
            (tree_x, tree_y),
            (tree_x - width * 0.020, tree_y - tree_h * 0.25),
            (tree_x + width * 0.015, tree_y - tree_h * 0.55),
            (tree_x - width * 0.010, top_y),
        ]
        trunk = catmull_rom_spline(trunk_ctrls, 10)
        # 幹の陰影
        strokes.append(
            create_stroke(
                [(x + scale * 0.005, y) for x, y in trunk],
                profile_type="brush",
                base_pressure=0.85,
                color="#201309",
                size_px=max(12.0, scale * 0.028),
                layer_name="Shading",
                opacity=0.60,
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid("tree_trunk_shadow"),
            )
        )
        strokes.append(
            create_stroke(
                trunk,
                profile_type="brush",
                base_pressure=0.95,
                color=trunk_color,
                size_px=max(10.0, scale * 0.022),
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid("tree_trunk"),
            )
        )
        # 根張り
        root_l = catmull_rom_spline([(tree_x, tree_y - tree_h * 0.08), (tree_x - scale * 0.04, tree_y)], 6)
        root_r = catmull_rom_spline([(tree_x, tree_y - tree_h * 0.08), (tree_x + scale * 0.035, tree_y)], 6)
        strokes.append(
            create_stroke(
                root_l,
                profile_type="brush",
                base_pressure=0.8,
                color=trunk_color,
                size_px=max(4.5, scale * 0.012),
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid("root_l"),
            )
        )
        strokes.append(
            create_stroke(
                root_r,
                profile_type="brush",
                base_pressure=0.8,
                color=trunk_color,
                size_px=max(4.5, scale * 0.012),
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid("root_r"),
            )
        )

        # 枝の階層的有機分岐 (Boughs with natural curvature)
        bough_configs = [
            (-1.0, 0.45, 0.35, 0.15, 0.25),
            (1.0, 0.55, 0.38, 0.12, 0.22),
            (-1.0, 0.70, 0.28, 0.08, 0.16),
            (1.0, 0.78, 0.25, 0.06, 0.14),
            (-0.7, 0.88, 0.16, 0.04, 0.09),
        ]
        for b_idx, (side, h_frac, spread, lift, arc) in enumerate(bough_configs):
            b_start_y = tree_y - tree_h * h_frac
            b_start_x = tree_x + side * scale * 0.010
            b_ctrl1_x = b_start_x + side * scale * spread * 0.40
            b_ctrl1_y = b_start_y - scale * lift * 0.3
            b_end_x = b_start_x + side * scale * spread
            b_end_y = b_start_y - scale * (lift + arc)
            b_pts = catmull_rom_spline(
                [(b_start_x, b_start_y), (b_ctrl1_x, b_ctrl1_y), (b_end_x, b_end_y)],
                8,
            )
            strokes.append(
                create_stroke(
                    b_pts,
                    profile_type="brush",
                    base_pressure=0.80,
                    color=trunk_color,
                    size_px=max(3.0, scale * 0.009 * (1.0 - h_frac * 0.3)),
                    layer_name="Lineart",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("tree_bough", b_idx),
                )
            )

        # 4. 樹冠の花房・葉のボリューム (Blossom Canopy / Foliage Mass)
        if is_sakura:
            blossom_colors = ["#f7a8c4", "#ffb8cd", "#ffd6e5", "#fff0f5"]
            canopy_centers = [
                (tree_x - scale * 0.14, tree_y - tree_h * 0.65, scale * 0.15),
                (tree_x + scale * 0.16, tree_y - tree_h * 0.72, scale * 0.16),
                (tree_x - scale * 0.05, top_y - scale * 0.04, scale * 0.18),
                (tree_x + scale * 0.08, tree_y - tree_h * 0.55, scale * 0.14),
                (tree_x - scale * 0.22, tree_y - tree_h * 0.50, scale * 0.12),
                (tree_x + scale * 0.24, tree_y - tree_h * 0.58, scale * 0.13),
            ]
            # 奥の陰影塊 (Deep Shadow Mass)
            for c_i, (cc_x, cc_y, cc_r) in enumerate(canopy_centers[:3]):
                c_pts = [
                    (
                        cc_x + math.cos(deg) * cc_r * (0.9 + rng.uniform(-0.06, 0.06)),
                        cc_y + math.sin(deg) * cc_r * (0.9 + rng.uniform(-0.06, 0.06)),
                    )
                    for deg in (0.0, math.pi * 0.5, math.pi, math.pi * 1.5, math.pi * 2.0)
                ]
                strokes.append(
                    create_stroke(
                        catmull_rom_spline(c_pts, 6),
                        profile_type="watercolor",
                        base_pressure=0.75,
                        color="#9d4b68",
                        size_px=max(25.0, cc_r * 0.75),
                        layer_name="Flats",
                        opacity=0.62,
                        rng=rng,
                        width=width,
                        height=height,
                        stroke_id=uid("sakura_shadow", c_i),
                    )
                )

            # 主花房クラスタ (Flats Blossom Clusters)
            for c_i, (cc_x, cc_y, cc_r) in enumerate(canopy_centers):
                col = blossom_colors[c_i % len(blossom_colors)]
                c_pts = [
                    (
                        cc_x + math.cos(deg) * cc_r * (1.0 + rng.uniform(-0.08, 0.08)),
                        cc_y + math.sin(deg) * cc_r * (1.0 + rng.uniform(-0.08, 0.08)),
                    )
                    for deg in (0.0, math.pi * 0.5, math.pi, math.pi * 1.5, math.pi * 2.0)
                ]
                strokes.append(
                    create_stroke(
                        catmull_rom_spline(c_pts, 8),
                        profile_type="watercolor",
                        base_pressure=0.85,
                        color=col,
                        size_px=max(20.0, cc_r * 0.65),
                        layer_name="Flats",
                        opacity=0.78,
                        rng=rng,
                        width=width,
                        height=height,
                        stroke_id=uid("sakura_mass", c_i),
                    )
                )

            # 舞い散る花びら (Drifting Petals - Highlights)
            for p_i in range(10):
                px = tree_x + rng.uniform(-scale * 0.28, scale * 0.35)
                py = tree_y - tree_h * rng.uniform(0.15, 0.95)
                drift_pts = [(px, py), (px + scale * rng.uniform(0.015, 0.035), py + scale * rng.uniform(0.01, 0.025))]
                strokes.append(
                    create_stroke(
                        drift_pts,
                        profile_type="gpen",
                        base_pressure=0.85,
                        color="#fff0f5" if p_i % 2 == 0 else "#ffc2d6",
                        size_px=max(2.5, scale * 0.0045),
                        layer_name="Highlights",
                        opacity=0.90,
                        rng=rng,
                        width=width,
                        height=height,
                        stroke_id=uid("sakura_blossom", p_i),
                    )
                )
        else:
            # 通常樹木のモコモコした葉の塊
            for b_i in range(8):
                b_ang = (b_i / 8) * math.pi * 1.8 - math.pi * 0.9
                bx = tree_x + math.cos(b_ang) * width * 0.12
                by = tree_y - tree_h * 0.7 + math.sin(b_ang) * height * 0.10
                foliage = bezier_cubic(
                    (bx - width * 0.04, by),
                    (bx - width * 0.02, by - height * 0.05),
                    (bx + width * 0.02, by - height * 0.05),
                    (bx + width * 0.04, by),
                    samples=8,
                )
                strokes.append(
                    create_stroke(
                        foliage,
                        profile_type="gpen",
                        base_pressure=0.85,
                        color="#2d6a4f" if b_i % 2 == 0 else "#40916c",
                        size_px=5.5,
                        layer_name="Lineart",
                        rng=rng,
                        width=width,
                        height=height,
                        stroke_id=uid("foliage", b_i),
                    )
                )

        # 5. 雲 (Clouds - 立体フォーム＆光彩エッジ)
        for c_i in range(3):
            cx_cloud = width * (0.18 + c_i * 0.32)
            cy_cloud = height * (0.12 + c_i * 0.05)
            cw = width * 0.24
            cloud_pts = catmull_rom_spline(
                [
                    (cx_cloud - cw * 0.5, cy_cloud),
                    (cx_cloud - cw * 0.25, cy_cloud - height * 0.04),
                    (cx_cloud + cw * 0.1, cy_cloud - height * 0.05),
                    (cx_cloud + cw * 0.4, cy_cloud - height * 0.02),
                    (cx_cloud + cw * 0.5, cy_cloud),
                    (cx_cloud, cy_cloud + height * 0.02),
                    (cx_cloud - cw * 0.5, cy_cloud),
                ],
                8,
            )
            strokes.append(
                create_stroke(
                    cloud_pts,
                    profile_type="brush",
                    base_pressure=0.7,
                    color="#e2e8f0",
                    size_px=max(4.0, scale * 0.008),
                    layer_name="Lineart",
                    opacity=0.8,
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("cloud", c_i),
                )
            )
            # 雲上面のハイライト (Highlights)
            strokes.append(
                create_stroke(
                    cloud_pts[1:4],
                    profile_type="airbrush",
                    base_pressure=0.85,
                    color="#ffffff",
                    size_px=max(6.0, scale * 0.012),
                    layer_name="Highlights",
                    opacity=0.88,
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("cloud_hl", c_i),
                )
            )

        # 6. Fantasy 光彩エフェクト (Sunbeams & Magic Sparkles)
        if is_fantasy:
            # 天空から斜めに差し込む木漏れ日・光芒 (Sunbeams)
            beam_x0 = width * 0.20
            beam_y0 = 0.0
            for b_i in range(3):
                bx_end = width * (0.35 + b_i * 0.22)
                by_end = height * 0.75
                beam_pts = [(beam_x0 + b_i * width * 0.08, beam_y0), (bx_end, by_end)]
                strokes.append(
                    create_stroke(
                        beam_pts,
                        profile_type="airbrush",
                        base_pressure=0.60,
                        color="#fff9e6",
                        size_px=max(30.0, scale * 0.06),
                        layer_name="Highlights",
                        opacity=0.38,
                        rng=rng,
                        width=width,
                        height=height,
                        stroke_id=uid("fantasy_sunbeam", b_i),
                    )
                )
            # 神秘的な光の粒子 (Sparkles)
            for s_i in range(6):
                sp_x = tree_x + rng.uniform(-scale * 0.35, scale * 0.35)
                sp_y = tree_y - tree_h * rng.uniform(0.3, 0.9)
                strokes.append(
                    create_stroke(
                        [(sp_x, sp_y), (sp_x + scale * 0.003, sp_y + scale * 0.003)],
                        profile_type="marupen",
                        base_pressure=0.95,
                        color="#ffffff",
                        size_px=max(2.5, scale * 0.005),
                        layer_name="Highlights",
                        opacity=0.95,
                        rng=rng,
                        width=width,
                        height=height,
                        stroke_id=uid("fantasy_sparkle", s_i),
                    )
                )

    return sample_strokes_by_priority(recolor_strokes_to_palette(strokes, palette_name), count)
