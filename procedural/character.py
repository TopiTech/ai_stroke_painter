"""アニメキャラクター・ポートレート・人物イラストのプロシージャル生成エンジン。"""

from __future__ import annotations

import math
import random
import re
import uuid

from ..domain import Stroke
from .base import catmull_rom_spline, color_palette, create_stroke, sample_strokes_by_priority


def generate_character_strokes(
    prompt: str,
    seed: int,
    count: int,
    width: float,
    height: float,
    palette_name: str = "anime",
) -> list[Stroke]:
    """指示文、Seed、本数、寸法からアニメキャラクターの本格イラストストローク群を生成する。"""
    rng = random.Random(seed)
    colors = color_palette(palette_name)
    normalized_prompt = prompt.casefold()
    masculine_subject = bool(re.search(r"\b(?:boy|male|man|men|gentleman)\b", normalized_prompt)) or any(
        keyword in normalized_prompt for keyword in ("少年", "男の子", "男性", "男子", "青年")
    )

    cx = width * 0.5
    cy = height * 0.48
    scale = min(width, height) * 0.85
    jitter = scale * 0.005

    strokes: list[Stroke] = []

    def uid(name: str, idx: int = 0) -> str:
        return str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/char/{seed}/{name}/{idx}"))

    # =========================================================================
    # 1. 下書きレイヤー (Draft Layer)
    # =========================================================================
    # 頭部アタリ円
    draft_pts: list[tuple[float, float]] = []
    head_r = scale * 0.28
    for i in range(17):
        ang = i * (math.pi * 2 / 16)
        draft_pts.append((cx + math.cos(ang) * head_r, cy - scale * 0.05 + math.sin(ang) * head_r))
    strokes.append(
        create_stroke(
            draft_pts,
            profile_type="marupen",
            base_pressure=0.3,
            color=colors["draft"],
            size_px=2.0,
            layer_name="Draft",
            opacity=0.4,
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("draft_head"),
        )
    )

    # 十字アタリ線
    strokes.append(
        create_stroke(
            [(cx, cy - scale * 0.35), (cx, cy + scale * 0.25)],
            profile_type="marupen",
            base_pressure=0.25,
            color=colors["draft"],
            size_px=1.5,
            layer_name="Draft",
            opacity=0.35,
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("draft_center_v"),
        )
    )
    strokes.append(
        create_stroke(
            [(cx - scale * 0.25, cy), (cx + scale * 0.25, cy)],
            profile_type="marupen",
            base_pressure=0.25,
            color=colors["draft"],
            size_px=1.5,
            layer_name="Draft",
            opacity=0.35,
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("draft_eye_h"),
        )
    )

    # =========================================================================
    # 2. 輪郭・顔のライン (Lineart - Face Contour)
    # =========================================================================
    # 左頬〜顎先
    left_jaw = catmull_rom_spline(
        [
            (cx - scale * 0.22, cy - scale * 0.08),
            (cx - scale * 0.20, cy + scale * 0.06),
            (cx - scale * 0.12, cy + scale * 0.18),
            (cx, cy + scale * 0.24),
        ],
        samples_per_segment=8,
    )
    strokes.append(
        create_stroke(
            left_jaw,
            profile_type="gpen",
            base_pressure=0.85,
            color=colors["lineart"],
            size_px=5.5,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("jaw_l"),
        )
    )

    # 右頬〜顎先
    right_jaw = catmull_rom_spline(
        [
            (cx + scale * 0.22, cy - scale * 0.08),
            (cx + scale * 0.20, cy + scale * 0.06),
            (cx + scale * 0.12, cy + scale * 0.18),
            (cx, cy + scale * 0.24),
        ],
        samples_per_segment=8,
    )
    strokes.append(
        create_stroke(
            right_jaw,
            profile_type="gpen",
            base_pressure=0.85,
            color=colors["lineart"],
            size_px=5.5,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("jaw_r"),
        )
    )

    # 首のライン
    neck_l = catmull_rom_spline(
        [(cx - scale * 0.07, cy + scale * 0.20), (cx - scale * 0.08, cy + scale * 0.35)], samples_per_segment=6
    )
    neck_r = catmull_rom_spline(
        [(cx + scale * 0.07, cy + scale * 0.20), (cx + scale * 0.08, cy + scale * 0.35)], samples_per_segment=6
    )
    strokes.append(
        create_stroke(
            neck_l,
            profile_type="gpen",
            base_pressure=0.7,
            color=colors["lineart"],
            size_px=4.0,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("neck_l"),
        )
    )
    strokes.append(
        create_stroke(
            neck_r,
            profile_type="gpen",
            base_pressure=0.7,
            color=colors["lineart"],
            size_px=4.0,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("neck_r"),
        )
    )

    # 首の影 (Shading Layer)
    neck_shadow = catmull_rom_spline(
        [
            (cx - scale * 0.06, cy + scale * 0.22),
            (cx, cy + scale * 0.28),
            (cx + scale * 0.06, cy + scale * 0.22),
        ],
        samples_per_segment=8,
    )
    strokes.append(
        create_stroke(
            neck_shadow,
            profile_type="marupen",
            base_pressure=0.6,
            color=colors["skin_shadow"],
            size_px=8.0,
            layer_name="Shading",
            opacity=0.6,
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("neck_shadow"),
        )
    )

    # =========================================================================
    # 3. 目・眉・鼻・口 (Lineart & Highlights - Eyes, Brows, Nose, Mouth)
    # =========================================================================
    eye_offset_x = scale * 0.11
    eye_y = cy - scale * 0.01
    eye_w = scale * 0.075
    eye_h = scale * 0.055

    for side, side_name in [(-1.0, "left"), (1.0, "right")]:
        ecx = cx + side * eye_offset_x

        # 上まつ毛 (太い力強いGペンストローク)
        upper_lash = catmull_rom_spline(
            [
                (ecx - side * eye_w * 0.7, eye_y + eye_h * 0.2),
                (ecx - side * eye_w * 0.2, eye_y - eye_h * 0.7),
                (ecx + side * eye_w * 0.6, eye_y - eye_h * 0.5),
                (ecx + side * eye_w * 0.85, eye_y - eye_h * 0.1),
            ],
            samples_per_segment=8,
        )
        strokes.append(
            create_stroke(
                upper_lash,
                profile_type="gpen",
                base_pressure=1.0,
                color=colors["lineart"],
                size_px=7.0,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"upper_lash_{side_name}"),
            )
        )

        # 二重まぶた
        double_lid = catmull_rom_spline(
            [
                (ecx - side * eye_w * 0.4, eye_y - eye_h * 0.9),
                (ecx + side * eye_w * 0.4, eye_y - eye_h * 0.8),
            ],
            samples_per_segment=6,
        )
        strokes.append(
            create_stroke(
                double_lid,
                profile_type="marupen",
                base_pressure=0.6,
                color=colors["lineart"],
                size_px=3.0,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"double_lid_{side_name}"),
            )
        )

        # 下まつ毛
        lower_lash = catmull_rom_spline(
            [
                (ecx - side * eye_w * 0.3, eye_y + eye_h * 0.65),
                (ecx + side * eye_w * 0.4, eye_y + eye_h * 0.60),
            ],
            samples_per_segment=6,
        )
        strokes.append(
            create_stroke(
                lower_lash,
                profile_type="marupen",
                base_pressure=0.65,
                color=colors["lineart"],
                size_px=3.5,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"lower_lash_{side_name}"),
            )
        )

        # 瞳の輪郭・ベタ塗り
        iris_pts = catmull_rom_spline(
            [
                (ecx - eye_w * 0.35, eye_y - eye_h * 0.3),
                (ecx - eye_w * 0.45, eye_y + eye_h * 0.2),
                (ecx, eye_y + eye_h * 0.5),
                (ecx + eye_w * 0.45, eye_y + eye_h * 0.2),
                (ecx + eye_w * 0.35, eye_y - eye_h * 0.3),
            ],
            samples_per_segment=6,
        )
        strokes.append(
            create_stroke(
                iris_pts,
                profile_type="gpen",
                base_pressure=0.8,
                color=colors["eye_dark"],
                size_px=5.0,
                layer_name="Flats",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"iris_outline_{side_name}"),
            )
        )

        # 瞳の虹彩カラーストローク (Flats Layer)
        for h_step in range(3):
            hy = eye_y - eye_h * 0.1 + h_step * eye_h * 0.2
            iris_fill = [(ecx - eye_w * 0.3, hy), (ecx + eye_w * 0.3, hy)]
            strokes.append(
                create_stroke(
                    iris_fill,
                    profile_type="marker",
                    base_pressure=0.9,
                    color=colors["eye_light"],
                    size_px=6.0,
                    layer_name="Flats",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid(f"iris_fill_{side_name}_{h_step}"),
                )
            )

        # 瞳孔 (Pupil)
        strokes.append(
            create_stroke(
                [(ecx, eye_y), (ecx, eye_y + eye_h * 0.15)],
                profile_type="gpen",
                base_pressure=1.0,
                color=colors["eye_dark"],
                size_px=7.0,
                layer_name="Flats",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"pupil_{side_name}"),
            )
        )

        # 瞳のハイライト (Highlights Layer)
        strokes.append(
            create_stroke(
                [(ecx - side * eye_w * 0.2, eye_y - eye_h * 0.2), (ecx - side * eye_w * 0.15, eye_y - eye_h * 0.1)],
                profile_type="gpen",
                base_pressure=1.0,
                color=colors["highlight"],
                size_px=6.0,
                layer_name="Highlights",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"eye_hl_main_{side_name}"),
            )
        )
        strokes.append(
            create_stroke(
                [(ecx + side * eye_w * 0.2, eye_y + eye_h * 0.2), (ecx + side * eye_w * 0.22, eye_y + eye_h * 0.25)],
                profile_type="marupen",
                base_pressure=0.8,
                color=colors["highlight"],
                size_px=3.5,
                layer_name="Highlights",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"eye_hl_sub_{side_name}"),
            )
        )

        # 眉毛 (Eyebrows)
        brow_y = eye_y - scale * 0.065
        eyebrow = catmull_rom_spline(
            [
                (ecx - side * eye_w * 0.6, brow_y + scale * 0.008),
                (ecx, brow_y - scale * 0.012),
                (ecx + side * eye_w * 0.7, brow_y + scale * 0.005),
            ],
            samples_per_segment=6,
        )
        strokes.append(
            create_stroke(
                eyebrow,
                profile_type="gpen",
                base_pressure=0.8,
                color=colors["hair_shadow"],
                size_px=4.5,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"brow_{side_name}"),
            )
        )

    # 鼻 (ちょこんとした可愛いアニメ風の鼻)
    nose = [(cx, cy + scale * 0.08), (cx + scale * 0.012, cy + scale * 0.095)]
    strokes.append(
        create_stroke(
            nose,
            profile_type="marupen",
            base_pressure=0.7,
            color=colors["lineart"],
            size_px=3.0,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("nose"),
        )
    )

    # 口 (微笑みの唇ラインと下唇の影)
    mouth = catmull_rom_spline(
        [
            (cx - scale * 0.05, cy + scale * 0.15),
            (cx, cy + scale * 0.16),
            (cx + scale * 0.05, cy + scale * 0.15),
        ],
        samples_per_segment=6,
    )
    strokes.append(
        create_stroke(
            mouth,
            profile_type="gpen",
            base_pressure=0.8,
            color=colors["lineart"],
            size_px=4.0,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("mouth"),
        )
    )

    # 下唇の影
    lower_lip_shadow = [(cx - scale * 0.015, cy + scale * 0.18), (cx + scale * 0.015, cy + scale * 0.18)]
    strokes.append(
        create_stroke(
            lower_lip_shadow,
            profile_type="marupen",
            base_pressure=0.6,
            color=colors["skin_shadow"],
            size_px=3.0,
            layer_name="Shading",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("lip_shadow"),
        )
    )

    # 頬のチーク・紅潮ハッチング (Cheek Blush)
    for c_side, c_name in [(-1.0, "l"), (1.0, "r")]:
        blush_cx = cx + c_side * scale * 0.16
        blush_cy = cy + scale * 0.08
        for b_i in range(3):
            bx = blush_cx + (b_i - 1) * scale * 0.015
            blush_stroke = [
                (bx - scale * 0.01, blush_cy - scale * 0.015),
                (bx + scale * 0.01, blush_cy + scale * 0.015),
            ]
            strokes.append(
                create_stroke(
                    blush_stroke,
                    profile_type="marupen",
                    base_pressure=0.5,
                    color=colors["hair_main"],
                    size_px=2.5,
                    layer_name="Shading",
                    opacity=0.6,
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid(f"blush_{c_name}_{b_i}"),
                )
            )

    # =========================================================================
    # 4. 髪型 (Hair - 前髪、サイド、後頭部、ハイライト、遊び毛)
    # =========================================================================
    # 後頭部・後ろ髪 (Back Hair)
    for i in range(7):
        p_offset = (i - 3) * scale * 0.07
        back_hair = catmull_rom_spline(
            [
                (cx + p_offset * 0.6, cy - scale * 0.28),
                (cx + p_offset * 1.1, cy + scale * 0.05),
                (cx + p_offset * 1.3, cy + scale * 0.40),
            ],
            samples_per_segment=8,
        )
        strokes.append(
            create_stroke(
                back_hair,
                profile_type="gpen",
                base_pressure=0.85,
                color=colors["hair_shadow"],
                size_px=6.5,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid("back_hair", i),
            )
        )

    # 前髪の房 (Bangs - 立体的で美しいアニメ風の束感)
    bang_count = 9
    for i in range(bang_count):
        t_phase = (i - (bang_count - 1) / 2) / (bang_count / 2)
        top_x = cx + t_phase * scale * 0.20 + rng.uniform(-jitter, jitter)
        top_y = cy - scale * 0.26
        mid_x = cx + t_phase * scale * 0.24 + math.sin(t_phase * 1.5) * scale * 0.02
        mid_y = cy - scale * 0.12
        tip_x = cx + t_phase * scale * 0.22 + rng.uniform(-scale * 0.01, scale * 0.01)
        tip_y = cy - scale * 0.02 + abs(t_phase) * scale * 0.04

        bang_strand = catmull_rom_spline([(top_x, top_y), (mid_x, mid_y), (tip_x, tip_y)], samples_per_segment=8)
        strokes.append(
            create_stroke(
                bang_strand,
                profile_type="gpen",
                base_pressure=0.9,
                color=colors["hair_main"],
                size_px=6.0,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid("bang_main", i),
            )
        )

        # 髪のディテール細線
        sub_strand = catmull_rom_spline(
            [
                (top_x + scale * 0.008, top_y + scale * 0.02),
                (mid_x + scale * 0.006, mid_y),
                (tip_x, tip_y - scale * 0.01),
            ],
            samples_per_segment=6,
        )
        strokes.append(
            create_stroke(
                sub_strand,
                profile_type="marupen",
                base_pressure=0.55,
                color=colors["hair_shadow"],
                size_px=2.5,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid("bang_sub", i),
            )
        )

    # サイドの髪 (Side Locks)
    for s_side, s_name in [(-1.0, "l"), (1.0, "r")]:
        for s_idx in range(3):
            soff = s_idx * scale * 0.02
            side_lock = catmull_rom_spline(
                [
                    (cx + s_side * (scale * 0.22 + soff), cy - scale * 0.20),
                    (cx + s_side * (scale * 0.26 + soff), cy + scale * 0.05),
                    (cx + s_side * (scale * 0.21 + soff), cy + scale * 0.28),
                    (cx + s_side * (scale * 0.18 + soff), cy + scale * 0.42),
                ],
                samples_per_segment=10,
            )
            strokes.append(
                create_stroke(
                    side_lock,
                    profile_type="gpen",
                    base_pressure=0.9,
                    color=colors["hair_main"],
                    size_px=5.5,
                    layer_name="Lineart",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid(f"side_lock_{s_name}", s_idx),
                )
            )

    # 髪の天使の輪ハイライト (Hair Highlights - Highlights Layer)
    for h_i in range(12):
        hx = cx + (h_i - 5.5) * scale * 0.035
        hy = cy - scale * 0.18 + math.sin(h_i * 0.5) * scale * 0.015
        hl_stroke = [(hx, hy - scale * 0.015), (hx + scale * 0.005, hy + scale * 0.015)]
        strokes.append(
            create_stroke(
                hl_stroke,
                profile_type="gpen",
                base_pressure=0.95,
                color=colors["hair_highlight"],
                size_px=4.5,
                layer_name="Highlights",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid("hair_ring_hl", h_i),
            )
        )

    # 頭頂部のアホ毛 (Ahoge - 遊び心のあるカーブ)
    ahoge = catmull_rom_spline(
        [
            (cx, cy - scale * 0.28),
            (cx - scale * 0.05, cy - scale * 0.38),
            (cx + scale * 0.04, cy - scale * 0.44),
            (cx + scale * 0.08, cy - scale * 0.39),
        ],
        samples_per_segment=10,
    )
    strokes.append(
        create_stroke(
            ahoge,
            profile_type="gpen",
            base_pressure=0.8,
            color=colors["hair_main"],
            size_px=4.0,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("ahoge"),
        )
    )

    # =========================================================================
    # 5. 衣服・リボン・襟 (Clothes & Accessories)
    # =========================================================================
    # 襟 (Collar)
    collar_l = catmull_rom_spline(
        [(cx - scale * 0.08, cy + scale * 0.34), (cx, cy + scale * 0.42), (cx - scale * 0.15, cy + scale * 0.46)],
        samples_per_segment=8,
    )
    collar_r = catmull_rom_spline(
        [(cx + scale * 0.08, cy + scale * 0.34), (cx, cy + scale * 0.42), (cx + scale * 0.15, cy + scale * 0.46)],
        samples_per_segment=8,
    )
    strokes.append(
        create_stroke(
            collar_l,
            profile_type="gpen",
            base_pressure=0.85,
            color=colors["cloth_main"],
            size_px=5.5,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("collar_l"),
        )
    )
    strokes.append(
        create_stroke(
            collar_r,
            profile_type="gpen",
            base_pressure=0.85,
            color=colors["cloth_main"],
            size_px=5.5,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("collar_r"),
        )
    )

    # 肩のライン (Shoulders)
    shoulder_l = catmull_rom_spline(
        [(cx - scale * 0.12, cy + scale * 0.37), (cx - scale * 0.35, cy + scale * 0.48)], samples_per_segment=8
    )
    shoulder_r = catmull_rom_spline(
        [(cx + scale * 0.12, cy + scale * 0.37), (cx + scale * 0.35, cy + scale * 0.48)], samples_per_segment=8
    )
    strokes.append(
        create_stroke(
            shoulder_l,
            profile_type="gpen",
            base_pressure=0.8,
            color=colors["lineart"],
            size_px=5.0,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("shoulder_l"),
        )
    )
    strokes.append(
        create_stroke(
            shoulder_r,
            profile_type="gpen",
            base_pressure=0.8,
            color=colors["lineart"],
            size_px=5.0,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("shoulder_r"),
        )
    )

    # 胸元のリボン (Ribbon)
    ribbon_l = catmull_rom_spline(
        [(cx, cy + scale * 0.42), (cx - scale * 0.08, cy + scale * 0.44), (cx - scale * 0.05, cy + scale * 0.50)],
        samples_per_segment=8,
    )
    ribbon_r = catmull_rom_spline(
        [(cx, cy + scale * 0.42), (cx + scale * 0.08, cy + scale * 0.44), (cx + scale * 0.05, cy + scale * 0.50)],
        samples_per_segment=8,
    )
    strokes.append(
        create_stroke(
            ribbon_l,
            profile_type="gpen",
            base_pressure=0.9,
            color=colors["hair_main"],
            size_px=5.0,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("ribbon_l"),
        )
    )
    strokes.append(
        create_stroke(
            ribbon_r,
            profile_type="gpen",
            base_pressure=0.9,
            color=colors["hair_main"],
            size_px=5.0,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("ribbon_r"),
        )
    )

    if masculine_subject:
        # 長いサイドロック、頬のチーク、胸元のリボンを短髪の輪郭とシャツの襟へ置き換える。
        # 同じ seed でも "girl" と "boy" が同一の絵にならないよう、形状自体を変える。
        removed_ids = {
            uid("ribbon_l"),
            uid("ribbon_r"),
            uid("ahoge"),
            *(uid("back_hair", index) for index in range(7)),
            *(uid(f"side_lock_{side}", index) for side in ("l", "r") for index in range(3)),
            *(uid(f"blush_{side}_{index}") for side in ("l", "r") for index in range(3)),
        }
        strokes = [stroke for stroke in strokes if stroke.id not in removed_ids]

        short_hair_shapes = [
            [(-0.24, -0.19), (-0.31, -0.27), (-0.20, -0.30)],
            [(-0.20, -0.28), (-0.15, -0.38), (-0.08, -0.29)],
            [(-0.10, -0.30), (-0.03, -0.41), (0.02, -0.30)],
            [(0.00, -0.30), (0.08, -0.40), (0.10, -0.28)],
            [(0.08, -0.29), (0.18, -0.36), (0.17, -0.25)],
            [(0.16, -0.26), (0.29, -0.29), (0.23, -0.18)],
            [(-0.25, -0.20), (-0.29, -0.02), (-0.23, 0.08)],
            [(0.25, -0.20), (0.29, -0.02), (0.23, 0.08)],
        ]
        for index, shape in enumerate(short_hair_shapes):
            hair_points = catmull_rom_spline(
                [(cx + dx * scale, cy + dy * scale) for dx, dy in shape],
                samples_per_segment=7,
            )
            strokes.append(
                create_stroke(
                    hair_points,
                    profile_type="gpen",
                    base_pressure=0.9,
                    color=colors["hair_main"],
                    size_px=6.0,
                    layer_name="Lineart",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("short_hair", index),
                )
            )

        for side, label in ((-1.0, "l"), (1.0, "r")):
            shirt_line = catmull_rom_spline(
                [
                    (cx + side * scale * 0.04, cy + scale * 0.41),
                    (cx + side * scale * 0.12, cy + scale * 0.47),
                    (cx + side * scale * 0.03, cy + scale * 0.51),
                ],
                samples_per_segment=7,
            )
            strokes.append(
                create_stroke(
                    shirt_line,
                    profile_type="gpen",
                    base_pressure=0.85,
                    color=colors["cloth_main"],
                    size_px=5.0,
                    layer_name="Lineart",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid(f"shirt_collar_{label}"),
                )
            )

    # ユーザー指定の本数に合わせてレイヤー優先度付きサンプリング
    return sample_strokes_by_priority(strokes, count)
