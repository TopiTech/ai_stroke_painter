"""アニメキャラクター・ポートレート・人物イラストのプロシージャル生成エンジン。
顔の立体面構造（Planes of the Face）、耳、鼻筋、唇、3層髪ボリューム、首・鎖骨・胸元、衣服のシワ、
および5大ライティング理論（フォームシャドウ、アンビエントオクルージョン、キャストシャドウ、ハイライト）を統合。
"""

from __future__ import annotations

import math
import random
import re
import uuid

from ..domain import Stroke
from ..prompt_analyzer import analyze_prompt
from .base import (
    catmull_rom_spline,
    color_palette,
    create_stroke,
    generate_ambient_occlusion_stroke,
    generate_cast_shadow_stroke,
    generate_highlight_stroke,
    sample_strokes_by_priority,
)


def character_feature_stroke_ids(seed: int) -> tuple[str, ...]:
    """低予算でも顔として読める、左右対称の必須輪郭を優先順で返す。"""

    names = (
        "jaw_l",
        "jaw_r",
        "upper_lash_left",
        "upper_lash_right",
        "iris_outline_left",
        "iris_outline_right",
        "nose",
        "mouth",
        "brow_left",
        "brow_right",
    )
    return tuple(str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/char/{seed}/{name}/0")) for name in names)


def _adjust_color_luminance(hex_str: str, factor: float) -> str:
    """指定された16進カラーの輝度を調整する（factor < 1.0 で暗く、factor > 1.0 で明るく）。"""
    h = hex_str.lstrip("#")
    if len(h) == 6:
        try:
            r, g, b = int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)
            return f"#{max(0, min(255, round(r * factor))):02x}{max(0, min(255, round(g * factor))):02x}{max(0, min(255, round(b * factor))):02x}"
        except ValueError:
            pass
    return hex_str


def generate_character_strokes(
    prompt: str,
    seed: int,
    count: int | None,
    width: float,
    height: float,
    palette_name: str = "anime",
) -> list[Stroke]:
    """指示文、Seed、本数、寸法から本格的な人物イラストストローク群を生成する。"""
    rng = random.Random(seed)
    colors = color_palette(palette_name)
    normalized_prompt = prompt.casefold()
    sem = analyze_prompt(prompt)

    masculine_subject = (
        sem.character.gender == "male"
        or bool(re.search(r"\b(?:boy|male|man|men|gentleman|hero)\b", normalized_prompt))
        or any(keyword in normalized_prompt for keyword in ("少年", "男の子", "男性", "男子", "青年", "ヒーロー"))
    )

    cx = width * 0.5
    cy = height * 0.47
    scale = min(width, height) * 0.85
    jitter = scale * 0.004

    strokes: list[Stroke] = []

    def uid(name: str, idx: int = 0) -> str:
        return str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/char/{seed}/{name}/{idx}"))

    # =========================================================================
    # 1. 下書きレイヤー (Draft Layer - 構造アタリ & パース線)
    # =========================================================================
    # 頭部アタリ球体
    draft_pts: list[tuple[float, float]] = []
    head_r = scale * 0.28
    for i in range(17):
        ang = i * (math.pi * 2 / 16)
        draft_pts.append((cx + math.cos(ang) * head_r, cy - scale * 0.04 + math.sin(ang) * head_r))
    strokes.append(
        create_stroke(
            draft_pts,
            profile_type="marupen",
            base_pressure=0.3,
            color=colors["draft"],
            size_px=2.0,
            layer_name="Draft",
            opacity=0.35,
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("draft_head"),
        )
    )

    # 十字中心アタリ線
    strokes.append(
        create_stroke(
            [(cx, cy - scale * 0.35), (cx, cy + scale * 0.30)],
            profile_type="marupen",
            base_pressure=0.25,
            color=colors["draft"],
            size_px=1.5,
            layer_name="Draft",
            opacity=0.30,
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("draft_center_v"),
        )
    )
    # アイライン・眉ライン・鼻・口アタリ
    strokes.append(
        create_stroke(
            [(cx - scale * 0.26, cy), (cx + scale * 0.26, cy)],
            profile_type="marupen",
            base_pressure=0.25,
            color=colors["draft"],
            size_px=1.5,
            layer_name="Draft",
            opacity=0.30,
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("draft_eye_h"),
        )
    )
    strokes.append(
        create_stroke(
            [(cx - scale * 0.20, cy - scale * 0.07), (cx + scale * 0.20, cy - scale * 0.07)],
            profile_type="marupen",
            base_pressure=0.2,
            color=colors["draft"],
            size_px=1.2,
            layer_name="Draft",
            opacity=0.25,
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("draft_brow_h"),
        )
    )

    # 顎先アタリV字
    strokes.append(
        create_stroke(
            [(cx - scale * 0.20, cy + scale * 0.06), (cx, cy + scale * 0.24), (cx + scale * 0.20, cy + scale * 0.06)],
            profile_type="marupen",
            base_pressure=0.25,
            color=colors["draft"],
            size_px=1.5,
            layer_name="Draft",
            opacity=0.30,
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("draft_jaw"),
        )
    )

    # =========================================================================
    # 2. 下塗りレイヤー (Flats Layer - インナーヘア・肌・瞳ベース)
    # =========================================================================
    # 奥の後ろ髪（インナーヘア）のダークベース塗り
    for i in range(6):
        p_off = (i - 2.5) * scale * 0.08
        inner_hair_sweep = catmull_rom_spline(
            [
                (cx + p_off * 0.7, cy - scale * 0.25),
                (cx + p_off * 1.2, cy + scale * 0.10),
                (cx + p_off * 1.4, cy + scale * 0.42),
            ],
            samples_per_segment=7,
        )
        strokes.append(
            create_stroke(
                inner_hair_sweep,
                profile_type="marker",
                base_pressure=0.9,
                color=colors["hair_shadow"],
                size_px=12.0,
                layer_name="Flats",
                opacity=0.85,
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid("inner_hair_flat", i),
            )
        )

    # 顔・首のベース肌塗り
    for r_step in range(4):
        skin_r = (scale * 0.05) * (r_step + 1)
        skin_patch = catmull_rom_spline(
            [
                (cx - skin_r * 2.0, cy - scale * 0.10 + skin_r * 0.8),
                (cx, cy + scale * 0.05 + skin_r * 0.5),
                (cx + skin_r * 2.0, cy - scale * 0.10 + skin_r * 0.8),
            ],
            samples_per_segment=6,
        )
        strokes.append(
            create_stroke(
                skin_patch,
                profile_type="marker",
                base_pressure=0.8,
                color=colors["skin_base"],
                size_px=18.0,
                layer_name="Flats",
                opacity=0.75,
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid("skin_base_patch", r_step),
            )
        )

    # =========================================================================
    # 3. 陰影・立体感レイヤー (Shading Layer - 3D Facial Planes & Lighting)
    # =========================================================================
    # (A) 額・こめかみ・側頭部の立体フォームシャドウ (Planes of Forehead & Temples)
    temple_shading_l = catmull_rom_spline(
        [
            (cx - scale * 0.24, cy - scale * 0.22),
            (cx - scale * 0.20, cy - scale * 0.10),
            (cx - scale * 0.16, cy - scale * 0.02),
        ],
        samples_per_segment=6,
    )
    temple_shading_r = catmull_rom_spline(
        [
            (cx + scale * 0.24, cy - scale * 0.22),
            (cx + scale * 0.20, cy - scale * 0.10),
            (cx + scale * 0.16, cy - scale * 0.02),
        ],
        samples_per_segment=6,
    )
    strokes.append(
        create_stroke(
            temple_shading_l,
            profile_type="airbrush",
            base_pressure=0.55,
            color=colors["skin_shadow"],
            size_px=12.0,
            layer_name="Shading",
            opacity=0.40,
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("temple_shade_l"),
        )
    )
    strokes.append(
        create_stroke(
            temple_shading_r,
            profile_type="airbrush",
            base_pressure=0.55,
            color=colors["skin_shadow"],
            size_px=12.0,
            layer_name="Shading",
            opacity=0.40,
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("temple_shade_r"),
        )
    )

    # (B) 前髪の額への落ち影 (Cast Shadows from Bangs to Forehead)
    bang_cast_shadow = catmull_rom_spline(
        [
            (cx - scale * 0.20, cy - scale * 0.08),
            (cx - scale * 0.10, cy - scale * 0.04),
            (cx, cy - scale * 0.06),
            (cx + scale * 0.10, cy - scale * 0.04),
            (cx + scale * 0.20, cy - scale * 0.08),
        ],
        samples_per_segment=10,
    )
    strokes.append(
        generate_cast_shadow_stroke(
            bang_cast_shadow,
            colors["skin_shadow"],
            size_px=8.0,
            opacity=0.55,
            stroke_id=uid("bangs_cast_shadow"),
            width=width,
            height=height,
            rng=rng,
        )
    )

    # (C) 眼窩（アイソケット）と鼻梁両脇の立体シャドウ (Eye Sockets & Nose Bridge Shadow)
    eye_offset_x = scale * 0.11
    eye_y = cy - scale * 0.01
    eye_w = scale * 0.075
    eye_h = scale * 0.055

    for side, sname in [(-1.0, "l"), (1.0, "r")]:
        ecx = cx + side * eye_offset_x
        # 眉下の窪みシャドウ
        socket_shade = catmull_rom_spline(
            [
                (ecx - side * eye_w * 0.5, eye_y - eye_h * 0.9),
                (ecx, eye_y - eye_h * 0.65),
                (ecx + side * eye_w * 0.4, eye_y - eye_h * 0.8),
            ],
            samples_per_segment=6,
        )
        strokes.append(
            create_stroke(
                socket_shade,
                profile_type="airbrush",
                base_pressure=0.5,
                color=colors["skin_shadow"],
                size_px=6.0,
                layer_name="Shading",
                opacity=0.45,
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"socket_shade_{sname}"),
            )
        )

        # まぶたから白目への落ち影 (Sclera Top Cast Shadow)
        sclera_shadow = catmull_rom_spline(
            [
                (ecx - eye_w * 0.4, eye_y - eye_h * 0.2),
                (ecx, eye_y - eye_h * 0.1),
                (ecx + eye_w * 0.4, eye_y - eye_h * 0.2),
            ],
            samples_per_segment=6,
        )
        strokes.append(
            create_stroke(
                sclera_shadow,
                profile_type="marupen",
                base_pressure=0.6,
                color=colors["skin_shadow"],
                size_px=3.5,
                layer_name="Shading",
                opacity=0.60,
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"sclera_shadow_{sname}"),
            )
        )

        # 涙袋の立体ふくらみシャドウ (Tear Trough / Aegyo Sal)
        tear_trough = catmull_rom_spline(
            [
                (ecx - side * eye_w * 0.3, eye_y + eye_h * 0.75),
                (ecx, eye_y + eye_h * 0.85),
                (ecx + side * eye_w * 0.3, eye_y + eye_h * 0.70),
            ],
            samples_per_segment=6,
        )
        strokes.append(
            create_stroke(
                tear_trough,
                profile_type="marupen",
                base_pressure=0.45,
                color=colors["skin_shadow"],
                size_px=2.5,
                layer_name="Shading",
                opacity=0.40,
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"tear_trough_{sname}"),
            )
        )

    # (D) 鼻筋・小鼻・鼻下の立体陰影 (Nose Bridge, Nostril & Subnasal Shadow)
    nose_y = cy + scale * 0.08
    # 鼻筋の陰影（左光源を想定し、右側にソフトな陰影）
    nose_side_shadow = catmull_rom_spline(
        [
            (cx + scale * 0.012, cy + scale * 0.02),
            (cx + scale * 0.018, cy + scale * 0.06),
            (cx + scale * 0.022, nose_y),
        ],
        samples_per_segment=6,
    )
    strokes.append(
        create_stroke(
            nose_side_shadow,
            profile_type="airbrush",
            base_pressure=0.55,
            color=colors["skin_shadow"],
            size_px=5.0,
            layer_name="Shading",
            opacity=0.50,
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("nose_side_shadow"),
        )
    )
    # 鼻下の落ち影 (Subnasal Cast Shadow)
    nose_cast = [(cx - scale * 0.008, nose_y + scale * 0.012), (cx + scale * 0.015, nose_y + scale * 0.016)]
    strokes.append(
        generate_ambient_occlusion_stroke(
            nose_cast,
            colors["skin_shadow"],
            size_px=3.0,
            opacity=0.75,
            stroke_id=uid("nose_cast_shadow"),
            width=width,
            height=height,
            rng=rng,
        )
    )

    # (E) 頬の立体チーク＆頬骨ハイライト (Cheek Blush & Zygomatic Form)
    for c_side, c_name in [(-1.0, "l"), (1.0, "r")]:
        blush_cx = cx + c_side * scale * 0.16
        blush_cy = cy + scale * 0.075
        # ソフトチークエアブラシ
        strokes.append(
            create_stroke(
                [(blush_cx - scale * 0.02, blush_cy), (blush_cx + scale * 0.02, blush_cy)],
                profile_type="airbrush",
                base_pressure=0.6,
                color=colors["hair_main"],
                size_px=14.0,
                layer_name="Shading",
                opacity=0.35,
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"blush_glow_{c_name}"),
            )
        )
        # チークハッチング線
        for b_i in range(3):
            bx = blush_cx + (b_i - 1) * scale * 0.014
            blush_stroke = [
                (bx - scale * 0.008, blush_cy - scale * 0.012),
                (bx + scale * 0.008, blush_cy + scale * 0.012),
            ]
            strokes.append(
                create_stroke(
                    blush_stroke,
                    profile_type="marupen",
                    base_pressure=0.55,
                    color=colors["hair_main"],
                    size_px=2.5,
                    layer_name="Shading",
                    opacity=0.55,
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid(f"blush_line_{c_name}_{b_i}"),
                )
            )

    # (F) 唇の立体陰影 (Lips Shading & Lower Lip Occlusion)
    mouth_y = cy + scale * 0.15
    # 上唇の薄い影
    upper_lip_shade = [(cx - scale * 0.035, mouth_y - scale * 0.004), (cx + scale * 0.035, mouth_y - scale * 0.004)]
    strokes.append(
        create_stroke(
            upper_lip_shade,
            profile_type="airbrush",
            base_pressure=0.5,
            color=colors["skin_shadow"],
            size_px=4.0,
            layer_name="Shading",
            opacity=0.45,
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("upper_lip_shade"),
        )
    )
    # 下唇下のくぼみ影（オクルージョン）
    lower_lip_ao = [(cx - scale * 0.018, mouth_y + scale * 0.022), (cx + scale * 0.018, mouth_y + scale * 0.022)]
    strokes.append(
        generate_ambient_occlusion_stroke(
            lower_lip_ao,
            colors["skin_shadow"],
            size_px=3.2,
            opacity=0.70,
            stroke_id=uid("lower_lip_ao"),
            width=width,
            height=height,
            rng=rng,
        )
    )

    # (G) 顎下のアンビエントオクルージョン＆首全体の立体グラデーション (Submandibular AO & Neck Anatomy)
    jaw_y = cy + scale * 0.24
    neck_ao_curve = catmull_rom_spline(
        [
            (cx - scale * 0.08, jaw_y - scale * 0.02),
            (cx, jaw_y + scale * 0.015),
            (cx + scale * 0.08, jaw_y - scale * 0.02),
        ],
        samples_per_segment=8,
    )
    strokes.append(
        generate_ambient_occlusion_stroke(
            neck_ao_curve,
            colors["skin_shadow"],
            size_px=5.5,
            opacity=0.85,
            stroke_id=uid("neck_ao_top"),
            width=width,
            height=height,
            rng=rng,
        )
    )
    # 首のメイン落ち影
    neck_cast_shade = catmull_rom_spline(
        [
            (cx - scale * 0.07, jaw_y + scale * 0.01),
            (cx, jaw_y + scale * 0.06),
            (cx + scale * 0.07, jaw_y + scale * 0.01),
        ],
        samples_per_segment=8,
    )
    strokes.append(
        generate_cast_shadow_stroke(
            neck_cast_shade,
            colors["skin_shadow"],
            size_px=10.0,
            opacity=0.60,
            stroke_id=uid("neck_main_shadow"),
            width=width,
            height=height,
            rng=rng,
        )
    )

    # 首落ち影の境界のSSS血色・赤みライン (Subsurface Scattering Flush Border)
    neck_sss_border = catmull_rom_spline(
        [
            (cx - scale * 0.08, jaw_y + scale * 0.015),
            (cx, jaw_y + scale * 0.065),
            (cx + scale * 0.08, jaw_y + scale * 0.015),
        ],
        samples_per_segment=8,
    )
    strokes.append(
        create_stroke(
            neck_sss_border,
            profile_type="airbrush",
            base_pressure=0.55,
            color=colors.get("skin_sss", colors["skin_shadow"]),
            size_px=4.0,
            layer_name="Shading",
            opacity=0.45,
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("neck_sss_flush"),
        )
    )

    # (H) 胸鎖乳突筋＆鎖骨の陰影 (Sternocleidomastoid & Clavicle Shading)
    scm_l = catmull_rom_spline(
        [(cx - scale * 0.07, jaw_y + scale * 0.02), (cx - scale * 0.02, cy + scale * 0.38)], samples_per_segment=6
    )
    scm_r = catmull_rom_spline(
        [(cx + scale * 0.07, jaw_y + scale * 0.02), (cx + scale * 0.02, cy + scale * 0.38)], samples_per_segment=6
    )
    strokes.append(
        create_stroke(
            scm_l,
            profile_type="marupen",
            base_pressure=0.55,
            color=colors["skin_shadow"],
            size_px=3.5,
            layer_name="Shading",
            opacity=0.45,
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("scm_shade_l"),
        )
    )
    strokes.append(
        create_stroke(
            scm_r,
            profile_type="marupen",
            base_pressure=0.55,
            color=colors["skin_shadow"],
            size_px=3.5,
            layer_name="Shading",
            opacity=0.45,
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("scm_shade_r"),
        )
    )

    # 鎖骨（クラビクル）の窪みシャドウ
    clavicle_shade_l = catmull_rom_spline(
        [(cx - scale * 0.02, cy + scale * 0.385), (cx - scale * 0.16, cy + scale * 0.395)], samples_per_segment=6
    )
    clavicle_shade_r = catmull_rom_spline(
        [(cx + scale * 0.02, cy + scale * 0.385), (cx + scale * 0.16, cy + scale * 0.395)], samples_per_segment=6
    )
    strokes.append(
        create_stroke(
            clavicle_shade_l,
            profile_type="marupen",
            base_pressure=0.6,
            color=colors["skin_shadow"],
            size_px=3.0,
            layer_name="Shading",
            opacity=0.55,
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("clavicle_shade_l"),
        )
    )
    strokes.append(
        create_stroke(
            clavicle_shade_r,
            profile_type="marupen",
            base_pressure=0.6,
            color=colors["skin_shadow"],
            size_px=3.0,
            layer_name="Shading",
            opacity=0.55,
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("clavicle_shade_r"),
        )
    )

    # (I) 衣服のシワとドレープの落ち影 (Clothing Folds & Drapery Shadows)
    cloth_fold_l = catmull_rom_spline(
        [(cx - scale * 0.12, cy + scale * 0.44), (cx - scale * 0.22, cy + scale * 0.52)], samples_per_segment=6
    )
    cloth_fold_r = catmull_rom_spline(
        [(cx + scale * 0.12, cy + scale * 0.44), (cx + scale * 0.22, cy + scale * 0.52)], samples_per_segment=6
    )
    strokes.append(
        create_stroke(
            cloth_fold_l,
            profile_type="gpen",
            base_pressure=0.75,
            color=colors["cloth_shadow"],
            size_px=5.5,
            layer_name="Shading",
            opacity=0.65,
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("cloth_fold_l"),
        )
    )
    strokes.append(
        create_stroke(
            cloth_fold_r,
            profile_type="gpen",
            base_pressure=0.75,
            color=colors["cloth_shadow"],
            size_px=5.5,
            layer_name="Shading",
            opacity=0.65,
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("cloth_fold_r"),
        )
    )

    # =========================================================================
    # 4. 主線・輪郭レイヤー (Lineart Layer - Exquisite Anatomy Contours)
    # =========================================================================
    # (A) フェイスライン輪郭 (Jawline & Chin)
    jaw_thickness = 5.8 if not masculine_subject else 6.5
    jaw_pts_l = [
        (cx - scale * 0.22, cy - scale * 0.08),
        (cx - scale * 0.20, cy + scale * 0.06),
        (cx - scale * 0.12, cy + scale * 0.18),
        (cx, cy + scale * (0.24 if not masculine_subject else 0.25)),
    ]
    jaw_pts_r = [
        (cx + scale * 0.22, cy - scale * 0.08),
        (cx + scale * 0.20, cy + scale * 0.06),
        (cx + scale * 0.12, cy + scale * 0.18),
        (cx, cy + scale * (0.24 if not masculine_subject else 0.25)),
    ]
    left_jaw = catmull_rom_spline(jaw_pts_l, samples_per_segment=8)
    right_jaw = catmull_rom_spline(jaw_pts_r, samples_per_segment=8)
    strokes.append(
        create_stroke(
            left_jaw,
            profile_type="gpen",
            base_pressure=0.88,
            color=colors["lineart"],
            size_px=jaw_thickness,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("jaw_l"),
        )
    )
    strokes.append(
        create_stroke(
            right_jaw,
            profile_type="gpen",
            base_pressure=0.88,
            color=colors["lineart"],
            size_px=jaw_thickness,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("jaw_r"),
        )
    )

    # (B) 耳 (Ears - Helix, Antihelix, Tragus & Lobule)
    ear_y = cy - scale * 0.02
    ear_h = scale * 0.11
    ear_w = scale * 0.045
    for eside, esname in [(-1.0, "l"), (1.0, "r")]:
        ear_base_x = cx + eside * scale * 0.205
        # 耳輪外郭 (Outer Helix)
        ear_outer = catmull_rom_spline(
            [
                (ear_base_x, ear_y - ear_h * 0.4),
                (ear_base_x + eside * ear_w * 1.1, ear_y - ear_h * 0.2),
                (ear_base_x + eside * ear_w * 1.0, ear_y + ear_h * 0.3),
                (ear_base_x + eside * ear_w * 0.3, ear_y + ear_h * 0.6),
                (ear_base_x, ear_y + ear_h * 0.45),
            ],
            samples_per_segment=8,
        )
        strokes.append(
            create_stroke(
                ear_outer,
                profile_type="gpen",
                base_pressure=0.80,
                color=colors["lineart"],
                size_px=4.2,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"ear_outer_{esname}"),
            )
        )
        # 対耳輪・耳珠の内側ライン (Inner Antihelix & Concha)
        ear_inner = catmull_rom_spline(
            [
                (ear_base_x + eside * ear_w * 0.6, ear_y - ear_h * 0.15),
                (ear_base_x + eside * ear_w * 0.4, ear_y + ear_h * 0.1),
                (ear_base_x + eside * ear_w * 0.55, ear_y + ear_h * 0.25),
            ],
            samples_per_segment=6,
        )
        strokes.append(
            create_stroke(
                ear_inner,
                profile_type="marupen",
                base_pressure=0.65,
                color=colors["lineart"],
                size_px=2.8,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"ear_inner_{esname}"),
            )
        )
        # 耳介のくぼみシャドウ (Ear Concha Shading)
        strokes.append(
            create_stroke(
                [(ear_base_x + eside * ear_w * 0.3, ear_y), (ear_base_x + eside * ear_w * 0.5, ear_y + ear_h * 0.15)],
                profile_type="airbrush",
                base_pressure=0.6,
                color=colors["skin_shadow"],
                size_px=5.0,
                layer_name="Shading",
                opacity=0.60,
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"ear_shade_{esname}"),
            )
        )

    # (C) 目・まつ毛・瞳・眉毛 (Eyes, Eyelashes, Irises & Eyebrows - Exquisite Multi-layer Detailing)
    for side, side_name in [(-1.0, "left"), (1.0, "right")]:
        ecx = cx + side * eye_offset_x

        # 0. 白目の下塗り (Sclera Base - Flats Layer)
        sclera_fill = catmull_rom_spline(
            [
                (ecx - side * eye_w * 0.70, eye_y + eye_h * 0.10),
                (ecx, eye_y + eye_h * 0.40),
                (ecx + side * eye_w * 0.70, eye_y + eye_h * 0.05),
            ],
            samples_per_segment=6,
        )
        strokes.append(
            create_stroke(
                sclera_fill,
                profile_type="marker",
                base_pressure=0.9,
                color="#f8f9fa",
                size_px=10.0,
                layer_name="Flats",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"sclera_base_{side_name}"),
            )
        )

        # 1. 上まつ毛 (太い力強いGペン主線 + 美麗なアーチ)
        upper_lash = catmull_rom_spline(
            [
                (ecx - side * eye_w * 0.75, eye_y + eye_h * 0.15),
                (ecx - side * eye_w * 0.25, eye_y - eye_h * 0.75),
                (ecx + side * eye_w * 0.60, eye_y - eye_h * 0.55),
                (ecx + side * eye_w * 0.90, eye_y - eye_h * 0.05),
            ],
            samples_per_segment=8,
        )
        strokes.append(
            create_stroke(
                upper_lash,
                profile_type="gpen",
                base_pressure=1.0,
                color=colors["lineart"],
                size_px=7.2,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"upper_lash_{side_name}"),
            )
        )

        # 2. まつ毛の先端フリック束 (Lash Strand Flicks - 目尻の跳ね上げ毛束)
        flick_offsets = (
            [
                (0.85, -0.05, 1.08, -0.22, 2.8),
                (0.70, -0.30, 0.92, -0.52, 2.4),
                (0.50, -0.50, 0.68, -0.72, 2.0),
            ]
            if not masculine_subject
            else [
                (0.85, -0.05, 1.02, -0.15, 2.6),
            ]
        )
        for f_i, (fx0, fy0, fx1, fy1, fsz) in enumerate(flick_offsets):
            flick_pts = [
                (ecx + side * eye_w * fx0, eye_y + eye_h * fy0),
                (ecx + side * eye_w * fx1, eye_y + eye_h * fy1),
            ]
            strokes.append(
                create_stroke(
                    flick_pts,
                    profile_type="marupen",
                    base_pressure=0.75,
                    color=colors["lineart"],
                    size_px=fsz,
                    layer_name="Lineart",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid(f"lash_flick_{side_name}", f_i),
                )
            )

        # 3. 目頭の繊細な切開ライン (Inner Canthus Accent)
        inner_canthus = [
            (ecx - side * eye_w * 0.75, eye_y + eye_h * 0.15),
            (ecx - side * eye_w * 0.88, eye_y + eye_h * 0.28),
        ]
        strokes.append(
            create_stroke(
                inner_canthus,
                profile_type="marupen",
                base_pressure=0.60,
                color=colors["lineart"],
                size_px=2.2,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"inner_canthus_{side_name}"),
            )
        )

        # 4. 二重まぶた (Double Eyelid Crease - 自然な抑揚を持つアーチ)
        double_lid = catmull_rom_spline(
            [
                (ecx - side * eye_w * 0.50, eye_y - eye_h * 0.90),
                (ecx - side * eye_w * 0.05, eye_y - eye_h * 1.05),
                (ecx + side * eye_w * 0.40, eye_y - eye_h * 0.82),
            ],
            samples_per_segment=6,
        )
        strokes.append(
            create_stroke(
                double_lid,
                profile_type="marupen",
                base_pressure=0.65,
                color=colors["lineart"],
                size_px=2.6,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"double_lid_{side_name}"),
            )
        )

        # 5. 下まつ毛 (Lower Eyelashes - 繊細な分離毛束)
        lower_lash_points = [
            (ecx - side * eye_w * 0.30, eye_y + eye_h * 0.65),
            (ecx + side * eye_w * 0.45, eye_y + eye_h * 0.60),
        ]
        strokes.append(
            create_stroke(
                catmull_rom_spline(lower_lash_points, samples_per_segment=6),
                profile_type="marupen",
                base_pressure=0.60,
                color=colors["lineart"],
                size_px=2.8,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"lower_lash_{side_name}"),
            )
        )
        if not masculine_subject:
            for l_i, (lx, ly, ldx, ldy) in enumerate([(0.20, 0.63, 0.25, 0.78), (0.40, 0.60, 0.48, 0.74)]):
                lower_flick = [
                    (ecx + side * eye_w * lx, eye_y + eye_h * ly),
                    (ecx + side * eye_w * ldx, eye_y + eye_h * ldy),
                ]
                strokes.append(
                    create_stroke(
                        lower_flick,
                        profile_type="marupen",
                        base_pressure=0.55,
                        color=colors["lineart"],
                        size_px=1.8,
                        layer_name="Lineart",
                        rng=rng,
                        width=width,
                        height=height,
                        stroke_id=uid(f"lower_flick_{side_name}", l_i),
                    )
                )

        # 6. 瞳の輪郭 (Iris Contour)
        iris_pts = catmull_rom_spline(
            [
                (ecx - eye_w * 0.38, eye_y - eye_h * 0.35),
                (ecx - eye_w * 0.46, eye_y + eye_h * 0.20),
                (ecx, eye_y + eye_h * 0.52),
                (ecx + eye_w * 0.46, eye_y + eye_h * 0.20),
                (ecx + eye_w * 0.38, eye_y - eye_h * 0.35),
            ],
            samples_per_segment=6,
        )
        strokes.append(
            create_stroke(
                iris_pts,
                profile_type="gpen",
                base_pressure=0.82,
                color=colors["eye_dark"],
                size_px=4.8,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"iris_outline_{side_name}"),
            )
        )

        # 7. 瞳の虹彩カラーグラデーション (Flats Layer - 多層グラデーション)
        for h_step in range(2):
            hy = eye_y - eye_h * 0.15 + h_step * eye_h * 0.15
            iris_dark_fill = [(ecx - eye_w * 0.36, hy), (ecx + eye_w * 0.36, hy)]
            strokes.append(
                create_stroke(
                    iris_dark_fill,
                    profile_type="marker",
                    base_pressure=0.9,
                    color=colors["eye_dark"],
                    size_px=6.5,
                    layer_name="Flats",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid(f"iris_dark_fill_{side_name}_{h_step}"),
                )
            )
        for h_step in range(3):
            hy = eye_y + eye_h * 0.15 + h_step * eye_h * 0.12
            iw = eye_w * (0.35 - h_step * 0.05)
            iris_light_fill = [(ecx - iw, hy), (ecx + iw, hy)]
            strokes.append(
                create_stroke(
                    iris_light_fill,
                    profile_type="marker",
                    base_pressure=0.85,
                    color=colors["eye_light"],
                    size_px=5.5,
                    layer_name="Flats",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid(f"iris_light_fill_{side_name}_{h_step}"),
                )
            )

        # 8. 瞳孔 (Pupil Core - 深みのある中心核)
        strokes.append(
            create_stroke(
                [(ecx, eye_y - eye_h * 0.05), (ecx, eye_y + eye_h * 0.20)],
                profile_type="gpen",
                base_pressure=1.0,
                color=colors["eye_dark"],
                size_px=7.5,
                layer_name="Flats",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"pupil_{side_name}"),
            )
        )

        # 9. 虹彩の放射状ディテール線 (Iris Radial Strands - 宝石のようなテクスチャ)
        for r_i, (rx_angle, ry_sign) in enumerate([(-0.20, 0.35), (0.0, 0.40), (0.20, 0.35)]):
            radial_strand = [
                (ecx + eye_w * rx_angle * 0.5, eye_y + eye_h * 0.15),
                (ecx + eye_w * rx_angle, eye_y + eye_h * ry_sign),
            ]
            strokes.append(
                create_stroke(
                    radial_strand,
                    profile_type="marupen",
                    base_pressure=0.50,
                    color=colors.get("eye_crescent", colors["highlight"]),
                    size_px=1.8,
                    layer_name="Flats",
                    opacity=0.75,
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid(f"iris_radial_{side_name}", r_i),
                )
            )

        # 10. 瞳のハイライト (Highlights Layer - メイン、サブ、マイクログロー、三日月光)
        crescent_pts = catmull_rom_spline(
            [
                (ecx - eye_w * 0.28, eye_y + eye_h * 0.15),
                (ecx, eye_y + eye_h * 0.38),
                (ecx + eye_w * 0.28, eye_y + eye_h * 0.15),
            ],
            samples_per_segment=6,
        )
        strokes.append(
            generate_highlight_stroke(
                crescent_pts,
                color=colors.get("eye_crescent", colors["highlight"]),
                size_px=3.0,
                opacity=0.80,
                profile_type="marupen",
                stroke_id=uid(f"eye_crescent_{side_name}"),
                width=width,
                height=height,
                rng=rng,
            )
        )
        strokes.append(
            generate_highlight_stroke(
                [(ecx - side * eye_w * 0.22, eye_y - eye_h * 0.24), (ecx - side * eye_w * 0.12, eye_y - eye_h * 0.08)],
                color=colors["highlight"],
                size_px=5.8,
                stroke_id=uid(f"eye_hl_main_{side_name}"),
                width=width,
                height=height,
                rng=rng,
            )
        )
        strokes.append(
            generate_highlight_stroke(
                [(ecx + side * eye_w * 0.20, eye_y + eye_h * 0.22), (ecx + side * eye_w * 0.24, eye_y + eye_h * 0.27)],
                color=colors["highlight"],
                size_px=3.4,
                profile_type="marupen",
                stroke_id=uid(f"eye_hl_sub_{side_name}"),
                width=width,
                height=height,
                rng=rng,
            )
        )
        strokes.append(
            generate_highlight_stroke(
                [(ecx - side * eye_w * 0.05, eye_y + eye_h * 0.05), (ecx - side * eye_w * 0.02, eye_y + eye_h * 0.08)],
                color=colors["highlight"],
                size_px=2.0,
                profile_type="marupen",
                stroke_id=uid(f"eye_hl_sparkle_{side_name}"),
                width=width,
                height=height,
                rng=rng,
            )
        )

        # 眉毛 (Eyebrows - しなやかなアーチと毛流れ)
        brow_y = eye_y - scale * 0.065
        brow_pts = [
            (ecx - side * eye_w * 0.65, brow_y + scale * 0.008),
            (ecx, brow_y - scale * 0.012),
            (ecx + side * eye_w * 0.75, brow_y + scale * 0.005),
        ]
        eyebrow = catmull_rom_spline(brow_pts, samples_per_segment=6)
        strokes.append(
            create_stroke(
                eyebrow,
                profile_type="gpen",
                base_pressure=0.82 if not masculine_subject else 0.95,
                color=colors["hair_shadow"],
                size_px=4.2 if not masculine_subject else 5.5,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"brow_{side_name}"),
            )
        )

    # (D) 鼻 (Nose - 鼻先アクセントと小鼻フック)
    if not masculine_subject:
        nose_line = [(cx, cy + scale * 0.08), (cx + scale * 0.014, cy + scale * 0.095)]
    else:
        # 男性は鼻筋をシャープに強調
        nose_line = [(cx + scale * 0.005, cy + scale * 0.03), (cx + scale * 0.016, cy + scale * 0.095)]
    strokes.append(
        create_stroke(
            nose_line,
            profile_type="marupen" if not masculine_subject else "gpen",
            base_pressure=0.75,
            color=colors["lineart"],
            size_px=3.2 if not masculine_subject else 4.2,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("nose"),
        )
    )

    # (E) 口 (Mouth - 表情に応じたラインと口角の結節点)
    if sem.character.expression == "smile":
        mouth_curve = catmull_rom_spline(
            [
                (cx - scale * 0.054, mouth_y - scale * 0.007),
                (cx - scale * 0.026, mouth_y + scale * 0.012),
                (cx, mouth_y + scale * 0.014),
                (cx + scale * 0.026, mouth_y + scale * 0.012),
                (cx + scale * 0.054, mouth_y - scale * 0.007),
            ],
            samples_per_segment=6,
        )
    elif sem.character.expression == "sad":
        mouth_curve = catmull_rom_spline(
            [
                (cx - scale * 0.048, mouth_y + scale * 0.008),
                (cx, mouth_y - scale * 0.004),
                (cx + scale * 0.048, mouth_y + scale * 0.008),
            ],
            samples_per_segment=6,
        )
    elif sem.character.expression == "serious":
        mouth_curve = catmull_rom_spline(
            [
                (cx - scale * 0.042, mouth_y),
                (cx, mouth_y - scale * 0.002),
                (cx + scale * 0.042, mouth_y),
            ],
            samples_per_segment=6,
        )
    else:
        mouth_curve = catmull_rom_spline(
            [
                (cx - scale * 0.052, mouth_y),
                (cx, mouth_y + scale * 0.010),
                (cx + scale * 0.052, mouth_y),
            ],
            samples_per_segment=6,
        )
    strokes.append(
        create_stroke(
            mouth_curve,
            profile_type="gpen",
            base_pressure=0.85,
            color=colors["lineart"],
            size_px=4.2,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("mouth"),
        )
    )
    # 口角アクセント結節点
    strokes.append(
        create_stroke(
            [(cx - scale * 0.054, mouth_y - scale * 0.002), (cx - scale * 0.050, mouth_y + scale * 0.004)],
            profile_type="marupen",
            base_pressure=0.8,
            color=colors["lineart"],
            size_px=3.0,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("mouth_corner_l"),
        )
    )
    strokes.append(
        create_stroke(
            [(cx + scale * 0.050, mouth_y + scale * 0.004), (cx + scale * 0.054, mouth_y - scale * 0.002)],
            profile_type="marupen",
            base_pressure=0.8,
            color=colors["lineart"],
            size_px=3.0,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("mouth_corner_r"),
        )
    )

    # (F) 首・鎖骨・肩の輪郭 (Neck, Clavicles & Shoulders Lineart)
    neck_w = scale * 0.075 if not masculine_subject else scale * 0.095
    neck_l = catmull_rom_spline(
        [(cx - neck_w, cy + scale * 0.20), (cx - neck_w * 1.1, cy + scale * 0.36)], samples_per_segment=6
    )
    neck_r = catmull_rom_spline(
        [(cx + neck_w, cy + scale * 0.20), (cx + neck_w * 1.1, cy + scale * 0.36)], samples_per_segment=6
    )
    strokes.append(
        create_stroke(
            neck_l,
            profile_type="gpen",
            base_pressure=0.75,
            color=colors["lineart"],
            size_px=4.2,
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
            base_pressure=0.75,
            color=colors["lineart"],
            size_px=4.2,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("neck_r"),
        )
    )

    if masculine_subject:
        # 男性の喉仏（アダムズアップル）
        adams_apple = [(cx, cy + scale * 0.29), (cx + scale * 0.012, cy + scale * 0.305)]
        strokes.append(
            create_stroke(
                adams_apple,
                profile_type="gpen",
                base_pressure=0.8,
                color=colors["lineart"],
                size_px=3.8,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid("adams_apple"),
            )
        )

    # 鎖骨ライン (Clavicle Lineart)
    clavicle_line_l = catmull_rom_spline(
        [(cx - scale * 0.02, cy + scale * 0.38), (cx - scale * 0.15, cy + scale * 0.39)], samples_per_segment=6
    )
    clavicle_line_r = catmull_rom_spline(
        [(cx + scale * 0.02, cy + scale * 0.38), (cx + scale * 0.15, cy + scale * 0.39)], samples_per_segment=6
    )
    strokes.append(
        create_stroke(
            clavicle_line_l,
            profile_type="marupen",
            base_pressure=0.70,
            color=colors["lineart"],
            size_px=3.2,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("clavicle_line_l"),
        )
    )
    strokes.append(
        create_stroke(
            clavicle_line_r,
            profile_type="marupen",
            base_pressure=0.70,
            color=colors["lineart"],
            size_px=3.2,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("clavicle_line_r"),
        )
    )

    # 肩のライン (Shoulders)
    shoulder_l = catmull_rom_spline(
        [(cx - scale * 0.12, cy + scale * 0.37), (cx - scale * 0.36, cy + scale * 0.49)], samples_per_segment=8
    )
    shoulder_r = catmull_rom_spline(
        [(cx + scale * 0.12, cy + scale * 0.37), (cx + scale * 0.36, cy + scale * 0.49)], samples_per_segment=8
    )
    strokes.append(
        create_stroke(
            shoulder_l,
            profile_type="gpen",
            base_pressure=0.82,
            color=colors["lineart"],
            size_px=5.2,
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
            base_pressure=0.82,
            color=colors["lineart"],
            size_px=5.2,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("shoulder_r"),
        )
    )

    # (G) 装飾品 - メガネ (Accessories: Glasses)
    if "glasses" in sem.character.accessories:
        frame_col = "#2a2228" if not masculine_subject else "#1e2229"
        eye_w_g = scale * 0.082
        eye_h_g = scale * 0.062
        for side, side_name in ((-1.0, "l"), (1.0, "r")):
            gcx = cx + side * eye_offset_x
            gcy = eye_y + scale * 0.005
            # オーバルフレーム
            glass_pts = [
                (gcx + math.cos(ang) * eye_w_g, gcy + math.sin(ang) * eye_h_g)
                for ang in [i * (math.pi * 2 / 12) for i in range(13)]
            ]
            glass_spline = catmull_rom_spline(glass_pts, samples_per_segment=4)
            strokes.append(
                create_stroke(
                    glass_spline,
                    profile_type="marupen",
                    base_pressure=0.88,
                    color=frame_col,
                    size_px=2.8,
                    layer_name="Lineart",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid(f"glasses_rim_{side_name}"),
                )
            )
            # レンズの斜め光彩反射ハイライト
            strokes.append(
                create_stroke(
                    [(gcx - eye_w_g * 0.45, gcy - eye_h_g * 0.45), (gcx + eye_w_g * 0.25, gcy + eye_h_g * 0.25)],
                    profile_type="marupen",
                    base_pressure=0.75,
                    color="#ffffff",
                    size_px=2.0,
                    layer_name="Highlights",
                    opacity=0.65,
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid(f"glasses_glint_{side_name}"),
                )
            )
        # 左右フレームを繋ぐブリッジ線
        bridge_pts = catmull_rom_spline(
            [
                (cx - eye_offset_x + eye_w_g, eye_y + scale * 0.002),
                (cx, eye_y - scale * 0.003),
                (cx + eye_offset_x - eye_w_g, eye_y + scale * 0.002),
            ],
            samples_per_segment=5,
        )
        strokes.append(
            create_stroke(
                bridge_pts,
                profile_type="marupen",
                base_pressure=0.9,
                color=frame_col,
                size_px=2.6,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid("glasses_bridge"),
            )
        )

    # =========================================================================
    # 5. 髪型 (3-Layer Hair Volume - 後頭部・サイド・前髪束・後れ毛)
    # =========================================================================
    # (A) 後頭部・後ろ髪 (Back Hair Strands)
    is_twintails = sem.character.hair_style == "twintails"
    is_short_or_bob = sem.character.hair_style in ("bob", "short")

    if is_twintails:
        # ツインテール描画: 左右の頭部横から外側に広がり胸元へ下りるダイナミックな2大毛束
        for side, side_name in ((-1.0, "l"), (1.0, "r")):
            root_x = cx + side * scale * 0.22
            root_y = cy - scale * 0.16
            # 結び目リボン/ヘアゴム
            knot_pts = [
                (root_x - scale * 0.02, root_y - scale * 0.015),
                (root_x + scale * 0.02, root_y + scale * 0.015),
                (root_x, root_y),
            ]
            strokes.append(
                create_stroke(
                    knot_pts,
                    profile_type="gpen",
                    base_pressure=0.95,
                    color=colors.get("cloth_main", "#ff4081"),
                    size_px=5.0,
                    layer_name="Lineart",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid(f"twintail_knot_{side_name}"),
                )
            )
            # ツインテール下塗り (Flats)
            for t_i in range(3):
                t_off = (t_i - 1.0) * scale * 0.025
                tail_flat_pts = catmull_rom_spline(
                    [
                        (root_x, root_y),
                        (root_x + side * scale * (0.16 + t_off), cy + scale * 0.02),
                        (root_x + side * scale * (0.18 + t_off), cy + scale * 0.22),
                        (root_x + side * scale * 0.06, cy + scale * 0.44),
                    ],
                    samples_per_segment=8,
                )
                strokes.append(
                    create_stroke(
                        tail_flat_pts,
                        profile_type="marker",
                        base_pressure=0.9,
                        color=colors["hair_main"],
                        size_px=14.0,
                        layer_name="Flats",
                        opacity=0.88,
                        rng=rng,
                        width=width,
                        height=height,
                        stroke_id=uid(f"twintail_flat_{side_name}", t_i),
                    )
                )
            # ツインテール輪郭・毛流れ (Lineart)
            for t_i in range(4):
                t_off = (t_i - 1.5) * scale * 0.02
                tail_line_pts = catmull_rom_spline(
                    [
                        (root_x + t_off * 0.5, root_y),
                        (root_x + side * scale * (0.17 + t_off), cy + scale * 0.03),
                        (root_x + side * scale * (0.19 + t_off), cy + scale * 0.24),
                        (root_x + side * scale * (0.07 + t_off * 0.4), cy + scale * 0.45),
                    ],
                    samples_per_segment=9,
                )
                strokes.append(
                    create_stroke(
                        tail_line_pts,
                        profile_type="gpen",
                        base_pressure=0.92,
                        color=colors["hair_shadow"] if t_i % 2 == 0 else colors["hair_main"],
                        size_px=5.5,
                        layer_name="Lineart",
                        rng=rng,
                        width=width,
                        height=height,
                        stroke_id=uid(f"twintail_line_{side_name}", t_i),
                    )
                )
            # ツインテールハイライト (Highlights)
            tail_hl_pts = catmull_rom_spline(
                [
                    (root_x + side * scale * 0.12, cy - scale * 0.04),
                    (root_x + side * scale * 0.17, cy + scale * 0.08),
                    (root_x + side * scale * 0.15, cy + scale * 0.18),
                ],
                samples_per_segment=7,
            )
            strokes.append(
                create_stroke(
                    tail_hl_pts,
                    profile_type="marupen",
                    base_pressure=0.8,
                    color=colors["hair_highlight"],
                    size_px=3.5,
                    layer_name="Highlights",
                    opacity=0.85,
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid(f"twintail_hl_{side_name}"),
                )
            )

    back_hair_count = 3 if is_short_or_bob else (4 if is_twintails else (7 if not masculine_subject else 4))
    back_hair_reach = 0.08 if is_short_or_bob else (0.28 if is_twintails else (0.42 if not masculine_subject else 0.15))
    for i in range(back_hair_count):
        p_offset = (i - (back_hair_count - 1) / 2) * scale * 0.075
        back_hair = catmull_rom_spline(
            [
                (cx + p_offset * 0.6, cy - scale * 0.28),
                (cx + p_offset * 1.1, cy + scale * 0.05),
                (cx + p_offset * 1.3, cy + scale * back_hair_reach),
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

    # (B) 前髪の束 (Bangs - 立体的な房感・稜線・毛先抜き)
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
                base_pressure=0.92,
                color=colors["hair_main"],
                size_px=6.2,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid("bang_main", i),
            )
        )

        # 髪房のディテール稜線細線
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

        # 毛先の繊細な枝分かれ (Split Tip Flicks)
        if i % 2 == 0:
            split_dir = -1.0 if t_phase < 0 else 1.0
            split_tip = [
                (tip_x - split_dir * scale * 0.004, tip_y - scale * 0.018),
                (tip_x + split_dir * scale * 0.012, tip_y + scale * 0.008),
            ]
            strokes.append(
                create_stroke(
                    split_tip,
                    profile_type="marupen",
                    base_pressure=0.60,
                    color=colors["hair_main"],
                    size_px=2.0,
                    layer_name="Lineart",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("bang_split", i),
                )
            )

    # 前髪表面をふわりと横切る微細な遊び毛 (Surface Flyaway Strands)
    for fly_i, (fx_start, fy_start, fx_end, fy_end) in enumerate(
        [
            (-0.16, -0.22, 0.08, -0.08),
            (0.14, -0.20, -0.06, -0.06),
        ]
    ):
        surface_flyaway = catmull_rom_spline(
            [
                (cx + fx_start * scale, cy + fy_start * scale),
                (
                    cx + (fx_start + fx_end) * 0.5 * scale + (0.01 if fly_i == 0 else -0.01) * scale,
                    cy + (fy_start + fy_end) * 0.5 * scale - 0.02 * scale,
                ),
                (cx + fx_end * scale, cy + fy_end * scale),
            ],
            samples_per_segment=8,
        )
        strokes.append(
            create_stroke(
                surface_flyaway,
                profile_type="marupen",
                base_pressure=0.45,
                color=colors["hair_main"],
                size_px=1.8,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid("bang_surface_flyaway", fly_i),
            )
        )

    # (C) サイドの髪 (Side Locks)
    if not masculine_subject:
        for s_side, s_name in [(-1.0, "l"), (1.0, "r")]:
            for s_idx in range(3):
                soff = s_idx * scale * 0.022
                side_lock = catmull_rom_spline(
                    [
                        (cx + s_side * (scale * 0.22 + soff), cy - scale * 0.20),
                        (cx + s_side * (scale * 0.26 + soff), cy + scale * 0.05),
                        (cx + s_side * (scale * 0.21 + soff), cy + scale * 0.28),
                        (cx + s_side * (scale * 0.18 + soff), cy + scale * 0.44),
                    ],
                    samples_per_segment=10,
                )
                strokes.append(
                    create_stroke(
                        side_lock,
                        profile_type="gpen",
                        base_pressure=0.90,
                        color=colors["hair_main"],
                        size_px=5.8,
                        layer_name="Lineart",
                        rng=rng,
                        width=width,
                        height=height,
                        stroke_id=uid(f"side_lock_{s_name}", s_idx),
                    )
                )

    # (D) 頭頂部のアホ毛 & 後れ毛 (Ahoge & Flyaway Strands)
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
            base_pressure=0.80,
            color=colors["hair_main"],
            size_px=3.8,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("ahoge"),
        )
    )

    # 左右の後れ毛 (Flyaway Strands)
    flyaway_l = catmull_rom_spline(
        [
            (cx - scale * 0.18, cy - scale * 0.15),
            (cx - scale * 0.26, cy - scale * 0.05),
            (cx - scale * 0.28, cy + scale * 0.08),
        ],
        samples_per_segment=8,
    )
    strokes.append(
        create_stroke(
            flyaway_l,
            profile_type="marupen",
            base_pressure=0.50,
            color=colors["hair_main"],
            size_px=2.2,
            layer_name="Lineart",
            rng=rng,
            width=width,
            height=height,
            stroke_id=uid("flyaway_l"),
        )
    )

    # =========================================================================
    # 6. 衣服・リボン・襟 (Clothes, Ribbon, Collar & Drapery)
    # =========================================================================
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

    if not masculine_subject:
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
    else:
        # 男性のシャツ襟・ネクタイライン
        for side, label in ((-1.0, "l"), (1.0, "r")):
            shirt_line = catmull_rom_spline(
                [
                    (cx + side * scale * 0.04, cy + scale * 0.41),
                    (cx + side * scale * 0.12, cy + scale * 0.47),
                    (cx + side * scale * 0.03, cy + scale * 0.52),
                ],
                samples_per_segment=7,
            )
            strokes.append(
                create_stroke(
                    shirt_line,
                    profile_type="gpen",
                    base_pressure=0.85,
                    color=colors["cloth_main"],
                    size_px=5.2,
                    layer_name="Lineart",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid(f"shirt_collar_{label}"),
                )
            )

        # 短髪のシャープな毛束
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
                    base_pressure=0.92,
                    color=colors["hair_main"],
                    size_px=6.2,
                    layer_name="Lineart",
                    rng=rng,
                    width=width,
                    height=height,
                    stroke_id=uid("short_hair", index),
                )
            )

    # =========================================================================
    # 7. ハイライト・スペキュラレイヤー (Highlights Layer - 5-Point Lighting Speculars)
    # =========================================================================
    # (A) 鼻先のスペキュラハイライト (Nose Specular Glint)
    strokes.append(
        generate_highlight_stroke(
            [(cx + scale * 0.005, cy + scale * 0.082), (cx + scale * 0.009, cy + scale * 0.086)],
            color=colors["highlight"],
            size_px=3.2,
            stroke_id=uid("nose_highlight"),
            width=width,
            height=height,
            rng=rng,
        )
    )

    # (B) 下唇のぷるんとした立体ハイライト (Lip Gloss Multi-Point Specular)
    strokes.append(
        generate_highlight_stroke(
            [(cx - scale * 0.012, mouth_y + scale * 0.013), (cx + scale * 0.012, mouth_y + scale * 0.013)],
            color=colors["highlight"],
            size_px=3.2,
            opacity=0.90,
            stroke_id=uid("lip_highlight"),
            width=width,
            height=height,
            rng=rng,
        )
    )
    if not masculine_subject:
        # 下唇両端の微細な水分光沢アクセント
        strokes.append(
            generate_highlight_stroke(
                [(cx - scale * 0.022, mouth_y + scale * 0.010), (cx - scale * 0.018, mouth_y + scale * 0.011)],
                color=colors["highlight"],
                size_px=2.0,
                opacity=0.75,
                profile_type="marupen",
                stroke_id=uid("lip_highlight_sub_l"),
                width=width,
                height=height,
                rng=rng,
            )
        )
        strokes.append(
            generate_highlight_stroke(
                [(cx + scale * 0.018, mouth_y + scale * 0.011), (cx + scale * 0.022, mouth_y + scale * 0.010)],
                color=colors["highlight"],
                size_px=2.0,
                opacity=0.75,
                profile_type="marupen",
                stroke_id=uid("lip_highlight_sub_r"),
                width=width,
                height=height,
                rng=rng,
            )
        )

    # (C) 鎖骨の稜線ハイライト (Clavicle Specular Accents)
    strokes.append(
        generate_highlight_stroke(
            [(cx - scale * 0.05, cy + scale * 0.375), (cx - scale * 0.12, cy + scale * 0.385)],
            color=colors["highlight"],
            size_px=2.5,
            opacity=0.80,
            stroke_id=uid("clavicle_hl_l"),
            width=width,
            height=height,
            rng=rng,
        )
    )
    strokes.append(
        generate_highlight_stroke(
            [(cx + scale * 0.05, cy + scale * 0.375), (cx + scale * 0.12, cy + scale * 0.385)],
            color=colors["highlight"],
            size_px=2.5,
            opacity=0.80,
            stroke_id=uid("clavicle_hl_r"),
            width=width,
            height=height,
            rng=rng,
        )
    )

    # (D) 髪の天使の輪ハイライト (Hair Angel Halo Ring - 毛束立体ジグザグハイライト)
    halo_count = 14
    for h_i in range(halo_count):
        t_phase = (h_i - (halo_count - 1) / 2) / (halo_count / 2)
        hx = cx + t_phase * scale * 0.22
        # 毛束の段差を反映したジグザグオフセット
        zigzag = (0.012 if h_i % 2 == 0 else -0.008) * scale
        hy = cy - scale * 0.18 + math.sin(t_phase * math.pi * 0.8) * scale * 0.02 + zigzag
        hl_stroke = [(hx, hy - scale * 0.018), (hx + scale * 0.006, hy + scale * 0.018)]
        strokes.append(
            generate_highlight_stroke(
                hl_stroke,
                color=colors["hair_highlight"],
                size_px=5.2 if h_i % 2 == 0 else 3.8,
                stroke_id=uid("hair_ring_hl", h_i),
                width=width,
                height=height,
                rng=rng,
            )
        )

    # (E) シルエット・環境リムライト (Silhouette Rim Light - 逆光・輪郭発光)
    rim_color = colors.get("rim_light", colors["highlight"])
    for r_side, r_name in [(-1.0, "l"), (1.0, "r")]:
        rim_pts = catmull_rom_spline(
            [
                (cx + r_side * scale * 0.32, cy - scale * 0.15),
                (cx + r_side * scale * 0.35, cy + scale * 0.05),
                (cx + r_side * scale * 0.33, cy + scale * 0.25),
                (cx + r_side * scale * 0.38, cy + scale * 0.45),
            ],
            samples_per_segment=6,
        )
        strokes.append(
            generate_highlight_stroke(
                rim_pts,
                color=rim_color,
                size_px=3.8,
                opacity=0.70,
                profile_type="gpen",
                stroke_id=uid(f"rim_light_{r_name}"),
                width=width,
                height=height,
                rng=rng,
            )
        )

    # ユーザー指定の本数に合わせてレイヤー優先度付きサンプリング
    return sample_strokes_by_priority(strokes, count)
