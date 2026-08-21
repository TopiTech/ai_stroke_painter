"""動物・クリーチャー（猫、鳥、ドラゴン）のプロシージャル生成エンジン。"""

from __future__ import annotations

import random
import uuid

from ..domain import Stroke
from .base import catmull_rom_spline, create_stroke


def generate_creature_strokes(
    prompt: str,
    seed: int,
    count: int,
    width: float,
    height: float,
) -> list[Stroke]:
    """動物（猫、鳥、ドラゴン等）の本格イラストストロークを生成する。"""
    rng = random.Random(seed)
    strokes: list[Stroke] = []

    def uid(name: str, idx: int = 0) -> str:
        return str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/creature/{seed}/{name}/{idx}"))

    cx = width * 0.5
    cy = height * 0.52
    scale = min(width, height) * 0.8

    # =========================================================================
    # 猫のイラスト (Cat Illustration)
    # =========================================================================
    # 1. 顔の輪郭
    cat_head = catmull_rom_spline(
        [
            (cx - scale * 0.22, cy - scale * 0.05),
            (cx - scale * 0.20, cy + scale * 0.12),
            (cx, cy + scale * 0.20),
            (cx + scale * 0.20, cy + scale * 0.12),
            (cx + scale * 0.22, cy - scale * 0.05),
        ],
        8,
    )
    strokes.append(
        create_stroke(
            cat_head,
            profile_type="gpen",
            base_pressure=0.85,
            color="#2b2d42",
            size_px=5.5,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("cat_head"),
        )
    )

    # 2. 三角の猫耳 (Left & Right Ears)
    for e_side, e_name in [(-1.0, "l"), (1.0, "r")]:
        ear_pts = catmull_rom_spline(
            [
                (cx + e_side * scale * 0.08, cy - scale * 0.18),
                (cx + e_side * scale * 0.20, cy - scale * 0.35),
                (cx + e_side * scale * 0.24, cy - scale * 0.10),
            ],
            8,
        )
        strokes.append(
            create_stroke(
                ear_pts,
                profile_type="gpen",
                base_pressure=0.9,
                color="#2b2d42",
                size_px=5.5,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"cat_ear_{e_name}"),
            )
        )

        # 耳の内側ピンク (Flats Layer)
        ear_inner = [
            (cx + e_side * scale * 0.12, cy - scale * 0.16),
            (cx + e_side * scale * 0.19, cy - scale * 0.30),
            (cx + e_side * scale * 0.21, cy - scale * 0.12),
        ]
        strokes.append(
            create_stroke(
                catmull_rom_spline(ear_inner, 6),
                profile_type="marupen",
                base_pressure=0.7,
                color="#ffb4a2",
                size_px=4.0,
                layer_name="Flats",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"cat_ear_inner_{e_name}"),
            )
        )

    # 3. 大きなアーモンド型の瞳 (Cat Eyes)
    for eye_side, eye_name in [(-1.0, "l"), (1.0, "r")]:
        ecx = cx + eye_side * scale * 0.11
        ecy = cy - scale * 0.02
        cat_eye = catmull_rom_spline(
            [
                (ecx - scale * 0.05, ecy),
                (ecx, ecy - scale * 0.035),
                (ecx + scale * 0.05, ecy),
                (ecx, ecy + scale * 0.035),
                (ecx - scale * 0.05, ecy),
            ],
            8,
        )
        strokes.append(
            create_stroke(
                cat_eye,
                profile_type="gpen",
                base_pressure=0.9,
                color="#2b2d42",
                size_px=4.5,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"cat_eye_{eye_name}"),
            )
        )

        # 瞳のカラー (エメラルドグリーン)
        strokes.append(
            create_stroke(
                [(ecx, ecy - scale * 0.02), (ecx, ecy + scale * 0.02)],
                profile_type="marker",
                base_pressure=0.95,
                color="#06d6a0",
                size_px=7.0,
                layer_name="Flats",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"cat_iris_{eye_name}"),
            )
        )

        # 縦長の猫の瞳孔 (Slit Pupil)
        strokes.append(
            create_stroke(
                [(ecx, ecy - scale * 0.025), (ecx, ecy + scale * 0.025)],
                profile_type="gpen",
                base_pressure=1.0,
                color="#111111",
                size_px=3.5,
                layer_name="Flats",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"cat_pupil_{eye_name}"),
            )
        )

        # ハイライト
        strokes.append(
            create_stroke(
                [(ecx - scale * 0.015, ecy - scale * 0.015), (ecx - scale * 0.01, ecy - scale * 0.01)],
                profile_type="gpen",
                base_pressure=1.0,
                color="#ffffff",
                size_px=4.0,
                layer_name="Highlights",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"cat_hl_{eye_name}"),
            )
        )

    # 4. 鼻と口 (Nose & Muzzle)
    cat_nose = [(cx - scale * 0.02, cy + scale * 0.06), (cx + scale * 0.02, cy + scale * 0.06), (cx, cy + scale * 0.08)]
    strokes.append(
        create_stroke(
            catmull_rom_spline(cat_nose, 6),
            profile_type="gpen",
            base_pressure=0.85,
            color="#ffb4a2",
            size_px=3.5,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("cat_nose"),
        )
    )

    # ω型の口
    mouth_l = catmull_rom_spline([(cx, cy + scale * 0.08), (cx - scale * 0.03, cy + scale * 0.11)], 6)
    mouth_r = catmull_rom_spline([(cx, cy + scale * 0.08), (cx + scale * 0.03, cy + scale * 0.11)], 6)
    strokes.append(
        create_stroke(
            mouth_l,
            profile_type="gpen",
            base_pressure=0.8,
            color="#2b2d42",
            size_px=3.5,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("cat_mouth_l"),
        )
    )
    strokes.append(
        create_stroke(
            mouth_r,
            profile_type="gpen",
            base_pressure=0.8,
            color="#2b2d42",
            size_px=3.5,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("cat_mouth_r"),
        )
    )

    # 5. ヒゲ (Whiskers - 左右3本ずつ)
    for w_side, w_name in [(-1.0, "l"), (1.0, "r")]:
        for w_i in range(3):
            w_start = (cx + w_side * scale * 0.08, cy + scale * 0.08 + (w_i - 1) * scale * 0.02)
            w_end = (cx + w_side * scale * 0.32, cy + scale * 0.06 + (w_i - 1) * scale * 0.04)
            whisker = catmull_rom_spline([w_start, w_end], 6)
            strokes.append(
                create_stroke(
                    whisker,
                    profile_type="gpen",
                    base_pressure=0.6,
                    color="#4a4e69",
                    size_px=2.5,
                    layer_name="Lineart",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid(f"cat_whisker_{w_name}", w_i),
                )
            )

    if len(strokes) > count:
        step = len(strokes) / count
        chosen = [strokes[int(i * step)] for i in range(count)]
        return chosen
    return strokes
