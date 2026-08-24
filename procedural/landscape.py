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

    is_wave = any(k in prompt_l for k in ["wave", "波", "海", "ocean", "北斎", "hokusai", "water"])
    is_flower = any(k in prompt_l for k in ["flower", "花", "rose", "バラ", "薔薇", "sakura", "桜", "cherry"])

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

    elif is_flower:
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
        # 山岳・樹木・雲・丘陵のフル風景 (Mountains, Trees, Clouds)
        # =====================================================================
        # 1. 遠景の山並み (Distant Mountains)
        for m_layer in range(3):
            m_pts: list[tuple[float, float]] = []
            steps = 8
            base_y = height * (0.42 + m_layer * 0.12)
            m_pts.append((0.0, base_y))
            for s_i in range(1, steps):
                sx = (s_i / steps) * width
                sy = base_y - (rng.uniform(0.08, 0.22) * height) / (m_layer + 1)
                m_pts.append((sx, sy))
            m_pts.append((width, base_y))

            m_spline = catmull_rom_spline(m_pts, 8)
            strokes.append(
                create_stroke(
                    m_spline,
                    profile_type="gpen",
                    base_pressure=0.75,
                    color=["#4a5568", "#2d3748", "#1a202c"][m_layer],
                    size_px=4.5 - m_layer * 0.8,
                    layer_name="Lineart",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("mountain_ridge", m_layer),
                )
            )

            # 山肌のハッチング陰影 (Shading Layer)
            for h_i in range(6):
                hx = (h_i / 6) * width + width * 0.08
                hy = base_y - height * 0.05
                h_stroke = [(hx, hy), (hx - width * 0.04, hy + height * 0.06)]
                strokes.append(
                    create_stroke(
                        h_stroke,
                        profile_type="marupen",
                        base_pressure=0.5,
                        color="#718096",
                        size_px=2.5,
                        layer_name="Shading",
                        opacity=0.6,
                        rng=rng,
                        width=width,
                        height=height,
                        stroke_id=uid(f"mountain_hatch_{m_layer}", h_i),
                    )
                )

        # 2. 前景の樹木 (Foreground Tree)
        tree_x = width * 0.78
        tree_y = height * 0.88
        tree_h = height * 0.45

        # 幹 (Trunk)
        trunk = catmull_rom_spline(
            [
                (tree_x, tree_y),
                (tree_x - width * 0.02, tree_y - tree_h * 0.4),
                (tree_x + width * 0.01, tree_y - tree_h * 0.7),
                (tree_x - width * 0.01, tree_y - tree_h),
            ],
            8,
        )
        strokes.append(
            create_stroke(
                trunk,
                profile_type="brush",
                base_pressure=1.0,
                color="#3d2b1f",
                size_px=9.0,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid("tree_trunk"),
            )
        )

        # 枝と葉のクラスタ (Branches & Foliage)
        for b_i in range(8):
            b_ang = (b_i / 8) * math.pi * 1.8 - math.pi * 0.9
            bx = tree_x + math.cos(b_ang) * width * 0.12
            by = tree_y - tree_h * 0.7 + math.sin(b_ang) * height * 0.10
            # 葉のモコモコしたストローク
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

        # 3. 雲 (Clouds)
        for c_i in range(3):
            cx_cloud = width * (0.2 + c_i * 0.3)
            cy_cloud = height * (0.12 + c_i * 0.05)
            cw = width * 0.22
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
                    size_px=4.5,
                    layer_name="Lineart",
                    opacity=0.8,
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("cloud", c_i),
                )
            )

    return sample_strokes_by_priority(recolor_strokes_to_palette(strokes, palette_name), count)
