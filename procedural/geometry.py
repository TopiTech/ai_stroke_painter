"""幾何学アート・マンダラ・サイバーパンク都市スカイラインのプロシージャル生成エンジン。"""

from __future__ import annotations

import math
import random
import uuid

from ..domain import Stroke
from .base import catmull_rom_spline, create_stroke, sample_strokes_by_priority


def generate_geometry_strokes(
    prompt: str,
    seed: int,
    count: int | None,
    width: float,
    height: float,
) -> list[Stroke]:
    """幾何学マンダラや都市スカイラインストロークを生成する。"""
    rng = random.Random(seed)
    prompt_l = prompt.lower()
    strokes: list[Stroke] = []

    def uid(name: str, idx: int = 0) -> str:
        return str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/geom/{seed}/{name}/{idx}"))

    is_city = any(
        k in prompt_l for k in ["city", "都市", "building", "ビル", "cathedral", "cyber", "cyberpunk", "スカイライン"]
    )

    cx = width * 0.5
    cy = height * 0.5
    scale = min(width, height)

    if is_city:
        # =====================================================================
        # サイバーパンク・都市スカイライン (Cyberpunk Skyline & Perspective)
        # =====================================================================
        building_count = min(max(1, (count // 2) if count is not None else 15), 20)
        base_y = height * 0.85
        # 1. ビル群のアウトライン
        for b_i in range(building_count):
            bx = (b_i / max(1, building_count - 1)) * width * 0.9 + width * 0.05
            bw = (width / building_count) * rng.uniform(0.7, 1.2)
            bh = height * rng.uniform(0.25, 0.65)
            # ビルの四角形輪郭
            b_pts = [(bx, base_y), (bx, base_y - bh), (bx + bw, base_y - bh), (bx + bw, base_y)]
            strokes.append(
                create_stroke(
                    b_pts,
                    profile_type="marker",
                    base_pressure=0.9,
                    color="#0a192f" if b_i % 2 == 0 else "#172a45",
                    size_px=5.0,
                    layer_name="Lineart",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("bldg_outline", b_i),
                )
            )

            # ビルの窓グリッド (Flats / FX Layer)
            window_rows = int(bh / 30)
            for r_i in range(window_rows):
                wy = base_y - bh + r_i * 25 + 15
                w_stroke = [(bx + 5, wy), (bx + bw - 5, wy)]
                strokes.append(
                    create_stroke(
                        w_stroke,
                        profile_type="marupen",
                        base_pressure=0.7,
                        color="#00f0ff" if (b_i + r_i) % 3 == 0 else "#ff007f",
                        size_px=2.5,
                        layer_name="FX",
                        opacity=0.8,
                        rng=rng,
                        width=width,
                        height=height,
                        stroke_id=uid(f"bldg_win_{b_i}", r_i),
                    )
                )

            # アンテナ・スパイア
            if rng.random() > 0.5:
                spire = [(bx + bw * 0.5, base_y - bh), (bx + bw * 0.5, base_y - bh - height * 0.08)]
                strokes.append(
                    create_stroke(
                        spire,
                        profile_type="gpen",
                        base_pressure=0.8,
                        color="#ffd700",
                        size_px=2.5,
                        layer_name="Lineart",
                        rng=rng,
                        width=width,
                        height=height,
                        stroke_id=uid("bldg_spire", b_i),
                    )
                )

    else:
        # =====================================================================
        # 神聖幾何学・万華鏡マンダラ (Sacred Geometry Mandala)
        # =====================================================================
        symmetry = 8  # 8回対称
        rings = 5
        for r_i in range(rings):
            r = scale * (0.08 + r_i * 0.08)
            for s_i in range(symmetry):
                base_ang = (s_i / symmetry) * math.pi * 2.0
                next_ang = ((s_i + 1) / symmetry) * math.pi * 2.0
                mid_ang = (base_ang + next_ang) * 0.5

                p0 = (cx + math.cos(base_ang) * r, cy + math.sin(base_ang) * r)
                p_peak = (cx + math.cos(mid_ang) * (r + scale * 0.04), cy + math.sin(mid_ang) * (r + scale * 0.04))
                p1 = (cx + math.cos(next_ang) * r, cy + math.sin(next_ang) * r)

                petal = catmull_rom_spline([p0, p_peak, p1], 6)
                strokes.append(
                    create_stroke(
                        petal,
                        profile_type="gpen",
                        base_pressure=0.85,
                        color=["#6a0dad", "#b5179e", "#7209b7", "#4361ee", "#4cc9f0"][r_i % 5],
                        size_px=4.0,
                        layer_name="Lineart",
                        rng=rng,
                        width=width,
                        height=height,
                        stroke_id=uid(f"mandala_petal_{r_i}", s_i),
                    )
                )

        # 外周フレーム円
        for frame_r_factor in [0.46, 0.48]:
            fr = scale * frame_r_factor
            frame_pts = []
            segs = 36
            for seg in range(segs + 1):
                ang = (seg / segs) * math.pi * 2.0
                frame_pts.append((cx + math.cos(ang) * fr, cy + math.sin(ang) * fr))
            strokes.append(
                create_stroke(
                    frame_pts,
                    profile_type="marupen",
                    base_pressure=0.9,
                    color="#2b2d42",
                    size_px=3.5,
                    layer_name="Lineart",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("mandala_frame", int(frame_r_factor * 100)),
                )
            )

    return sample_strokes_by_priority(strokes, count)
