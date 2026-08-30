"""動物・クリーチャー（猫、犬、鳥、ドラゴン）のプロシージャル生成エンジン。"""

from __future__ import annotations

import math
import random
import uuid

from ..domain import Stroke
from .base import catmull_rom_spline, create_stroke, sample_strokes_by_priority


def creature_feature_stroke_ids(prompt: str, seed: int) -> tuple[str, ...]:
    """種ごとのシルエット・目・口を、認識に重要な順で返す。"""

    prompt_l = prompt.casefold()
    if any(key in prompt_l for key in ("dragon", "ドラゴン", "竜", "龍")):
        namespace = "dragon"
        names = ("spine", "belly", "snout", "wing_l", "wing_r", "eye", "horn_top", "horn_back")
    elif any(key in prompt_l for key in ("bird", "鳥", "小鳥", "eagle", "鷲")):
        namespace = "bird"
        names = ("body", "wing_l", "wing_r", "beak_top", "beak_bottom", "eye", "tail_2", "perch")
    elif any(key in prompt_l for key in ("dog", "犬", "puppy", "子犬")):
        namespace = "dog"
        names = ("head", "ear_l", "ear_r", "eye_l", "eye_r", "muzzle_l", "muzzle_r", "nose")
    else:
        names = (
            "cat_head",
            "cat_ear_l",
            "cat_ear_r",
            "cat_eye_l",
            "cat_eye_r",
            "cat_nose",
            "cat_mouth_l",
            "cat_mouth_r",
        )
        return tuple(str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/creature/{seed}/{name}/0")) for name in names)
    return tuple(str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/{namespace}/{seed}/{name}")) for name in names)


def generate_creature_strokes(
    prompt: str,
    seed: int,
    count: int | None,
    width: float,
    height: float,
) -> list[Stroke]:
    """動物（猫、犬、鳥、ドラゴン等）の本格イラストストロークを生成する。"""
    prompt_l = prompt.lower()
    if any(key in prompt_l for key in ("dragon", "ドラゴン", "竜", "龍")):
        return _generate_dragon_strokes(seed, count, width, height)
    if any(key in prompt_l for key in ("bird", "鳥", "小鳥", "eagle", "鷲")):
        return _generate_bird_strokes(seed, count, width, height)
    if any(key in prompt_l for key in ("dog", "犬", "puppy", "子犬")):
        return _generate_dog_strokes(seed, count, width, height)

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

    return sample_strokes_by_priority(strokes, count)


def _generate_dog_strokes(seed: int, count: int | None, width: float, height: float) -> list[Stroke]:
    rng = random.Random(seed)
    strokes: list[Stroke] = []
    cx, cy = width * 0.5, height * 0.52
    scale = min(width, height) * 0.78

    def add(name: str, points: list[tuple[float, float]], size: float = 4.0, layer: str = "Lineart") -> None:
        strokes.append(
            create_stroke(
                catmull_rom_spline(points, 6),
                profile_type="gpen",
                base_pressure=0.78,
                color="#4b3428" if layer == "Lineart" else "#b9835a",
                size_px=size,
                layer_name=layer,
                opacity=0.9,
                rng=rng,
                width=width,
                height=height,
                stroke_id=str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/dog/{seed}/{name}")),
            )
        )

    add(
        "head",
        [
            (cx - scale * 0.24, cy - scale * 0.18),
            (cx - scale * 0.30, cy + scale * 0.02),
            (cx, cy + scale * 0.27),
            (cx + scale * 0.30, cy + scale * 0.02),
            (cx + scale * 0.24, cy - scale * 0.18),
        ],
        6.0,
    )
    for side, label in ((-1.0, "l"), (1.0, "r")):
        add(
            f"ear_{label}",
            [
                (cx + side * scale * 0.18, cy - scale * 0.17),
                (cx + side * scale * 0.36, cy - scale * 0.08),
                (cx + side * scale * 0.34, cy + scale * 0.13),
                (cx + side * scale * 0.22, cy + scale * 0.06),
            ],
            7.0,
        )
        eye_x = cx + side * scale * 0.105
        add(
            f"eye_{label}",
            [(eye_x - scale * 0.025, cy - scale * 0.03), (eye_x + scale * 0.025, cy - scale * 0.03)],
            5.0,
        )
        for fur_idx in range(4):
            y = cy + scale * (0.02 + fur_idx * 0.035)
            add(
                f"cheek_fur_{label}_{fur_idx}",
                [(cx + side * scale * 0.17, y), (cx + side * scale * (0.25 + fur_idx * 0.01), y + scale * 0.018)],
                2.5,
            )
    add("muzzle_l", [(cx, cy + scale * 0.05), (cx - scale * 0.12, cy + scale * 0.13), (cx, cy + scale * 0.19)], 3.5)
    add("muzzle_r", [(cx, cy + scale * 0.05), (cx + scale * 0.12, cy + scale * 0.13), (cx, cy + scale * 0.19)], 3.5)
    add(
        "nose",
        [(cx - scale * 0.04, cy + scale * 0.08), (cx, cy + scale * 0.11), (cx + scale * 0.04, cy + scale * 0.08)],
        7.0,
    )
    add(
        "tongue",
        [(cx - scale * 0.035, cy + scale * 0.19), (cx, cy + scale * 0.27), (cx + scale * 0.035, cy + scale * 0.19)],
        4.0,
        "Highlights",
    )
    return sample_strokes_by_priority(strokes, count)


def _generate_bird_strokes(seed: int, count: int | None, width: float, height: float) -> list[Stroke]:
    rng = random.Random(seed)
    strokes: list[Stroke] = []
    cx, cy = width * 0.5, height * 0.54
    scale = min(width, height) * 0.75

    def add(name: str, points: list[tuple[float, float]], size: float = 3.5, layer: str = "Lineart") -> None:
        strokes.append(
            create_stroke(
                catmull_rom_spline(points, 5),
                profile_type="marupen",
                base_pressure=0.75,
                color="#25364a" if layer == "Lineart" else "#72a6c9",
                size_px=size,
                layer_name=layer,
                opacity=0.92,
                rng=rng,
                width=width,
                height=height,
                stroke_id=str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/bird/{seed}/{name}")),
            )
        )

    body = [
        (cx, cy - scale * 0.26),
        (cx - scale * 0.18, cy - scale * 0.08),
        (cx - scale * 0.12, cy + scale * 0.25),
        (cx + scale * 0.12, cy + scale * 0.25),
        (cx + scale * 0.18, cy - scale * 0.08),
        (cx, cy - scale * 0.26),
    ]
    add("body", body, 5.0)
    add("beak_top", [(cx, cy - scale * 0.16), (cx + scale * 0.20, cy - scale * 0.10), (cx, cy - scale * 0.07)], 3.0)
    add("beak_bottom", [(cx, cy - scale * 0.07), (cx + scale * 0.15, cy - scale * 0.06)], 2.5)
    add("eye", [(cx + scale * 0.045, cy - scale * 0.18), (cx + scale * 0.065, cy - scale * 0.18)], 5.0, "Highlights")
    for side, label in ((-1.0, "l"), (1.0, "r")):
        add(
            f"wing_{label}",
            [
                (cx + side * scale * 0.05, cy - scale * 0.05),
                (cx + side * scale * 0.35, cy + scale * 0.02),
                (cx + side * scale * 0.12, cy + scale * 0.20),
            ],
            5.0,
        )
        for feather in range(6):
            y = cy + scale * (0.01 + feather * 0.025)
            add(
                f"feather_{label}_{feather}",
                [(cx + side * scale * 0.08, y), (cx + side * scale * (0.19 + feather * 0.018), y + scale * 0.055)],
                2.2,
                "Shading",
            )
    for tail in range(5):
        x_off = (tail - 2) * scale * 0.035
        add("tail_" + str(tail), [(cx + x_off, cy + scale * 0.21), (cx + x_off * 1.8, cy + scale * 0.39)], 3.0)
    add("perch", [(cx - scale * 0.30, cy + scale * 0.31), (cx + scale * 0.32, cy + scale * 0.31)], 5.0, "Lineart")
    return sample_strokes_by_priority(strokes, count)


def _generate_dragon_strokes(seed: int, count: int | None, width: float, height: float) -> list[Stroke]:
    rng = random.Random(seed)
    strokes: list[Stroke] = []
    cx, cy = width * 0.5, height * 0.53
    scale = min(width, height) * 0.78

    def add(name: str, points: list[tuple[float, float]], size: float = 4.0, layer: str = "Lineart") -> None:
        strokes.append(
            create_stroke(
                catmull_rom_spline(points, 5),
                profile_type="gpen",
                base_pressure=0.82,
                color="#39294f" if layer == "Lineart" else "#7c4fa3",
                size_px=size,
                layer_name=layer,
                opacity=0.9,
                rng=rng,
                width=width,
                height=height,
                stroke_id=str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/dragon/{seed}/{name}")),
            )
        )

    spine = [
        (cx - scale * 0.30, cy - scale * 0.05),
        (cx - scale * 0.10, cy - scale * 0.18),
        (cx + scale * 0.12, cy - scale * 0.05),
        (cx + scale * 0.20, cy + scale * 0.18),
        (cx + scale * 0.38, cy + scale * 0.26),
    ]
    add("spine", spine, 7.0)
    add(
        "belly",
        [(cx - scale * 0.25, cy + scale * 0.03), (cx, cy + scale * 0.13), (cx + scale * 0.30, cy + scale * 0.28)],
        5.0,
    )
    add(
        "snout",
        [(cx - scale * 0.32, cy - scale * 0.06), (cx - scale * 0.43, cy), (cx - scale * 0.28, cy + scale * 0.05)],
        5.0,
    )
    add("horn_top", [(cx - scale * 0.20, cy - scale * 0.15), (cx - scale * 0.24, cy - scale * 0.34)], 4.0)
    add("horn_back", [(cx - scale * 0.12, cy - scale * 0.18), (cx - scale * 0.09, cy - scale * 0.36)], 4.0)
    add("eye", [(cx - scale * 0.28, cy - scale * 0.06), (cx - scale * 0.23, cy - scale * 0.07)], 5.0, "Highlights")
    for side, label in ((-1.0, "l"), (1.0, "r")):
        wing_y = cy - scale * (0.02 if side < 0 else 0.08)
        add(
            f"wing_{label}",
            [
                (cx, wing_y),
                (cx + side * scale * 0.16, cy - scale * 0.36),
                (cx + side * scale * 0.38, cy - scale * 0.22),
                (cx + side * scale * 0.16, cy + scale * 0.02),
            ],
            6.0,
        )
        for rib in range(4):
            angle = -0.9 + rib * 0.28
            add(
                f"wing_rib_{label}_{rib}",
                [(cx, wing_y), (cx + side * math.cos(angle) * scale * 0.30, cy + math.sin(angle) * scale * 0.30)],
                2.5,
                "Shading",
            )
    for plate in range(9):
        t = plate / 8
        px = cx - scale * 0.10 + t * scale * 0.42
        py = cy - scale * 0.04 + math.sin(t * math.pi) * scale * 0.13
        add(
            f"scale_{plate}",
            [(px - scale * 0.025, py), (px, py + scale * 0.035), (px + scale * 0.025, py)],
            2.2,
            "Shading",
        )
    return sample_strokes_by_priority(strokes, count)
