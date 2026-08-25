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
from .base import (
    catmull_rom_spline,
    color_palette,
    create_stroke,
    generate_ambient_occlusion_stroke,
    generate_cast_shadow_stroke,
    generate_highlight_stroke,
    sample_strokes_by_priority,
)


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
    masculine_subject = bool(re.search(r"\b(?:boy|male|man|men|gentleman|hero)\b", normalized_prompt)) or any(
        keyword in normalized_prompt for keyword in ("少年", "男の子", "男性", "男子", "青年", "ヒーロー")
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

    # (C) 目・まつ毛・瞳・眉毛 (Eyes, Eyelashes, Irises & Eyebrows)
    for side, side_name in [(-1.0, "left"), (1.0, "right")]:
        ecx = cx + side * eye_offset_x

        # 上まつ毛 (太い力強いGペン主線 + 切れ味のある目尻フリック)
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

        # 二重まぶた (Double Eyelid Crease)
        double_lid = catmull_rom_spline(
            [
                (ecx - side * eye_w * 0.45, eye_y - eye_h * 0.95),
                (ecx + side * eye_w * 0.35, eye_y - eye_h * 0.85),
            ],
            samples_per_segment=6,
        )
        strokes.append(
            create_stroke(
                double_lid,
                profile_type="marupen",
                base_pressure=0.60,
                color=colors["lineart"],
                size_px=2.8,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"double_lid_{side_name}"),
            )
        )

        # 下まつ毛 (Lower Eyelashes)
        lower_lash = catmull_rom_spline(
            [
                (ecx - side * eye_w * 0.30, eye_y + eye_h * 0.65),
                (ecx + side * eye_w * 0.45, eye_y + eye_h * 0.60),
            ],
            samples_per_segment=6,
        )
        strokes.append(
            create_stroke(
                lower_lash,
                profile_type="marupen",
                base_pressure=0.65,
                color=colors["lineart"],
                size_px=3.2,
                layer_name="Lineart",
                rng=rng,
                width=width,
                height=height,
                stroke_id=uid(f"lower_lash_{side_name}"),
            )
        )

        # 瞳の輪郭 (Iris Contour)
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

        # 瞳の虹彩カラーストローク & グラデーション (Flats Layer)
        for h_step in range(3):
            hy = eye_y - eye_h * 0.10 + h_step * eye_h * 0.20
            iris_fill = [(ecx - eye_w * 0.32, hy), (ecx + eye_w * 0.32, hy)]
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

        # 瞳孔 (Pupil - 深みのある中心コア)
        strokes.append(
            create_stroke(
                [(ecx, eye_y - eye_h * 0.05), (ecx, eye_y + eye_h * 0.18)],
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

        # 瞳のハイライト (Highlights Layer - メイン＆サブグリント)
        strokes.append(
            generate_highlight_stroke(
                [(ecx - side * eye_w * 0.20, eye_y - eye_h * 0.22), (ecx - side * eye_w * 0.14, eye_y - eye_h * 0.10)],
                color=colors["highlight"],
                size_px=5.5,
                stroke_id=uid(f"eye_hl_main_{side_name}"),
                width=width,
                height=height,
                rng=rng,
            )
        )
        strokes.append(
            generate_highlight_stroke(
                [(ecx + side * eye_w * 0.20, eye_y + eye_h * 0.22), (ecx + side * eye_w * 0.22, eye_y + eye_h * 0.26)],
                color=colors["highlight"],
                size_px=3.2,
                profile_type="marupen",
                stroke_id=uid(f"eye_hl_sub_{side_name}"),
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

    # (E) 口 (Mouth - 上品な微笑みラインと口角の結節点)
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

    # =========================================================================
    # 5. 髪型 (3-Layer Hair Volume - 後頭部・サイド・前髪束・後れ毛)
    # =========================================================================
    # (A) 後頭部・後ろ髪 (Back Hair Strands)
    back_hair_count = 7 if not masculine_subject else 4
    for i in range(back_hair_count):
        p_offset = (i - (back_hair_count - 1) / 2) * scale * 0.075
        back_hair = catmull_rom_spline(
            [
                (cx + p_offset * 0.6, cy - scale * 0.28),
                (cx + p_offset * 1.1, cy + scale * 0.05),
                (cx + p_offset * 1.3, cy + scale * (0.42 if not masculine_subject else 0.15)),
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

    # (B) 下唇のぷるんとした立体ハイライト (Lip Gloss Highlight)
    strokes.append(
        generate_highlight_stroke(
            [(cx - scale * 0.010, mouth_y + scale * 0.014), (cx + scale * 0.010, mouth_y + scale * 0.014)],
            color=colors["highlight"],
            size_px=2.8,
            stroke_id=uid("lip_highlight"),
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

    # (D) 髪の天使の輪ハイライト (Hair Angel Halo Ring - 多段グラデーション)
    for h_i in range(12):
        hx = cx + (h_i - 5.5) * scale * 0.035
        hy = cy - scale * 0.18 + math.sin(h_i * 0.5) * scale * 0.015
        hl_stroke = [(hx, hy - scale * 0.015), (hx + scale * 0.005, hy + scale * 0.015)]
        strokes.append(
            generate_highlight_stroke(
                hl_stroke,
                color=colors["hair_highlight"],
                size_px=4.8,
                stroke_id=uid("hair_ring_hl", h_i),
                width=width,
                height=height,
                rng=rng,
            )
        )

    # ユーザー指定の本数に合わせてレイヤー優先度付きサンプリング
    return sample_strokes_by_priority(strokes, count)
