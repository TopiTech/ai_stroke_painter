"""プロシージャル・イラスト生成エンジンのパッケージエントリーポイント。"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import replace
import math
import re

from ..brushes import brush_preset_for_profile, canonical_brush_profile
from ..domain import DrawingPlan, Stroke, StrokePoint
from ..stroke_program import (
    FillOperation,
    ProgramBrush,
    ProgramOperation,
    ProgramPoint,
    StrokeProgram,
    compile_stroke_program,
    drawing_plan_to_stroke_program,
)
from .base import color_palette, pressure_profile, recolor_strokes_to_palette
from .character import generate_character_strokes
from .creature import generate_creature_strokes
from .geometry import generate_geometry_strokes
from .landscape import generate_landscape_strokes
from .manga_fx import generate_manga_fx_strokes

__all__ = [
    "generate_character_strokes",
    "generate_creature_strokes",
    "generate_geometry_strokes",
    "generate_landscape_strokes",
    "generate_manga_fx_strokes",
    "generate_procedural_plan",
    "generate_procedural_program",
]

AUTO_PROCEDURAL_STROKE_BUDGET = 500


def generate_procedural_plan(
    prompt: str,
    seed: int,
    count: int | None,
    width: float,
    height: float,
    palette_name: str = "anime",
    brush_profile: str = "auto",
) -> DrawingPlan:
    """共通 StrokeProgram コンパイラを介して描画可能な計画を生成する。"""
    return compile_stroke_program(
        generate_procedural_program(prompt, seed, count, width, height, palette_name, brush_profile),
        count=count if count is not None else AUTO_PROCEDURAL_STROKE_BUDGET,
    )


def _prompt_category(prompt: str) -> str:
    """主題語を効果語より優先し、混合プロンプトを安定して分類する。"""
    normalized = prompt.casefold()

    def contains_keyword(keyword: str) -> bool:
        if any(ord(character) > 127 for character in keyword):
            return keyword in normalized
        # cat in cathedral / man in mandala のような部分一致を主題と誤認しない。
        suffix = r"(?:s|es)?" if " " not in keyword and "-" not in keyword else ""
        return re.search(rf"(?<![a-z0-9]){re.escape(keyword)}{suffix}(?![a-z0-9])", normalized) is not None

    keyword_groups = {
        "character": (
            "girl",
            "boy",
            "woman",
            "man",
            "portrait",
            "character",
            "anime",
            "hero",
            "人物",
            "少女",
            "少年",
            "女性",
            "男性",
        ),
        "creature": (
            "cat",
            "dog",
            "bird",
            "dragon",
            "animal",
            "creature",
            "wolf",
            "猫",
            "犬",
            "鳥",
            "動物",
            "竜",
            "ドラゴン",
        ),
        "landscape": (
            "landscape",
            "mountain",
            "wave",
            "ocean",
            "water",
            "tree",
            "flower",
            "rose",
            "sakura",
            "風景",
            "山",
            "波",
            "海",
            "木",
            "花",
            "桜",
        ),
        "geometry": (
            "mandala",
            "geometry",
            "city",
            "building",
            "cyberpunk",
            "cyber",
            "neon",
            "skyline",
            "cathedral",
            "マンダラ",
            "幾何",
            "都市",
            "ビル",
        ),
        "fx": (
            "fx",
            "focus lines",
            "speed lines",
            "speed",
            "magic circle",
            "magic",
            "rune",
            "hatch",
            "集中線",
            "流線",
            "魔法",
            "魔法陣",
            "ルーン",
            "カケアミ",
        ),
    }
    scores = {
        category: sum(1 for keyword in keywords if contains_keyword(keyword))
        for category, keywords in keyword_groups.items()
    }
    # 人物/生物を「主役」、風景/建築を「舞台」として優先度を分ける。
    # これにより "cat in a cyberpunk city" が都市だけの絵へ化けるのを防ぐ。
    for category_group in (("character", "creature"), ("landscape", "geometry")):
        best_subject = max(category_group, key=lambda category: scores[category])
        if scores[best_subject] > 0:
            return best_subject
    return "fx" if scores["fx"] > 0 else "character"


def _ellipse_points(cx: float, cy: float, rx: float, ry: float, count: int = 18) -> tuple[ProgramPoint, ...]:
    return tuple(
        ProgramPoint(cx + math.cos(index * math.tau / count) * rx, cy + math.sin(index * math.tau / count) * ry)
        for index in range(count)
    )


def _shade_color(color: str, factor: float) -> str:
    red, green, blue = (int(color[index : index + 2], 16) for index in (1, 3, 5))
    return f"#{round(red * factor):02x}{round(green * factor):02x}{round(blue * factor):02x}"


def _prompt_palette(prompt: str, palette_name: str) -> dict[str, str]:
    """明示された髪・瞳色を選択パレットの役割色へ反映する。"""
    colors = dict(color_palette(palette_name))
    normalized = prompt.casefold()
    color_cues = {
        "red": "#d94b58",
        "blue": "#4776d0",
        "green": "#4f9b68",
        "purple": "#8554b3",
        "pink": "#e86f9d",
        "blonde": "#e7bd55",
        "golden": "#e7bd55",
        "black": "#292632",
        "white": "#e8edf5",
        "赤": "#d94b58",
        "青": "#4776d0",
        "緑": "#4f9b68",
        "紫": "#8554b3",
        "ピンク": "#e86f9d",
        "金髪": "#e7bd55",
        "黒髪": "#292632",
        "白髪": "#e8edf5",
    }
    for cue, color in color_cues.items():
        hair_phrases = [f"{cue} hair", f"{cue}-haired", f"{cue}髪", f"{cue}い髪", f"{cue}の髪"]
        if cue in {"金髪", "黒髪", "白髪"}:
            hair_phrases.append(cue)
        if any(phrase in normalized for phrase in hair_phrases):
            colors["hair_main"] = color
            colors["hair_shadow"] = _shade_color(color, 0.62)
            break
    for cue, color in color_cues.items():
        if any(phrase in normalized for phrase in (f"{cue} eyes", f"{cue}-eyed", f"{cue}の瞳", f"{cue}い瞳")):
            colors["eye_light"] = color
            colors["eye_dark"] = _shade_color(color, 0.45)
            break
    return colors


def _foundation_operations(category: str, prompt: str, colors: Mapping[str, str]) -> tuple[ProgramOperation, ...]:
    """線の前に大きな色面を置き、白抜けと線画だけの出力を防ぐ。"""

    def fill(
        operation_id: str,
        polygon: tuple[ProgramPoint, ...],
        color: str,
        *,
        size: float,
        profile: str = "marker",
        opacity: float = 1.0,
    ) -> FillOperation:
        return FillOperation(
            id=f"foundation-{operation_id}",
            polygon=polygon,
            brush=ProgramBrush(profile=profile, color=color, size=size, opacity=opacity),
            layer="Flats",
        )

    canvas = (ProgramPoint(0, 0), ProgramPoint(1, 0), ProgramPoint(1, 1), ProgramPoint(0, 1))
    if category == "character":
        return (
            fill("background", canvas, colors["hair_highlight"], size=0.12, profile="airbrush", opacity=0.35),
            fill("back-hair", _ellipse_points(0.5, 0.45, 0.29, 0.40), colors["hair_shadow"], size=0.075),
            fill(
                "clothing",
                (ProgramPoint(0.22, 1), ProgramPoint(0.32, 0.72), ProgramPoint(0.68, 0.72), ProgramPoint(0.78, 1)),
                colors["cloth_main"],
                size=0.08,
            ),
            fill("face", _ellipse_points(0.5, 0.46, 0.205, 0.285), colors["skin_base"], size=0.065),
        )
    if category == "landscape":
        prompt_lower = prompt.casefold()
        is_wave = any(keyword in prompt_lower for keyword in ("wave", "ocean", "sea", "波", "海", "北斎"))
        is_flower = any(keyword in prompt_lower for keyword in ("flower", "rose", "sakura", "花", "バラ", "桜"))
        if is_wave:
            return (
                fill("sky", canvas, colors["hair_highlight"], size=0.12, profile="airbrush"),
                fill(
                    "sea",
                    (ProgramPoint(0, 0.48), ProgramPoint(1, 0.48), ProgramPoint(1, 1), ProgramPoint(0, 1)),
                    colors["eye_light"],
                    size=0.09,
                    profile="watercolor",
                ),
            )
        if is_flower:
            return (
                fill("background", canvas, colors["hair_highlight"], size=0.12, profile="watercolor", opacity=0.4),
                fill("flower-mass", _ellipse_points(0.5, 0.45, 0.30, 0.28), colors["hair_main"], size=0.055),
            )
        return (
            fill("sky", canvas, colors["hair_highlight"], size=0.12, profile="airbrush"),
            fill(
                "ground",
                (ProgramPoint(0, 0.55), ProgramPoint(1, 0.50), ProgramPoint(1, 1), ProgramPoint(0, 1)),
                colors["hair_main"],
                size=0.09,
                profile="watercolor",
            ),
            fill(
                "mountain",
                (
                    ProgramPoint(0, 0.62),
                    ProgramPoint(0.28, 0.28),
                    ProgramPoint(0.48, 0.58),
                    ProgramPoint(0.72, 0.35),
                    ProgramPoint(1, 0.62),
                ),
                colors["hair_shadow"],
                size=0.07,
            ),
        )
    if category == "creature":
        return (
            fill("background", canvas, colors["hair_highlight"], size=0.12, profile="airbrush", opacity=0.35),
            fill("body", _ellipse_points(0.5, 0.56, 0.28, 0.34), colors["hair_main"], size=0.07),
        )
    dark_background = colors["cloth_shadow"] if category == "geometry" else colors["lineart"]
    return (fill("background", canvas, dark_background, size=0.12, profile="airbrush"),)


def generate_procedural_program(
    prompt: str,
    seed: int,
    count: int | None,
    width: float,
    height: float,
    palette_name: str = "anime",
    brush_profile: str = "auto",
) -> StrokeProgram:
    """自然言語プロンプトの意図を自動解析し、最適なプロシージャルイラスト計画を生成する。"""
    category = _prompt_category(prompt)
    base_colors = color_palette(palette_name)
    effective_colors = _prompt_palette(prompt, palette_name)

    # カテゴリ判定
    if category == "fx":
        strokes = generate_manga_fx_strokes(prompt, seed, None, width, height)
        title = "Manga FX Artwork"
    elif category == "geometry":
        strokes = generate_geometry_strokes(prompt, seed, None, width, height)
        title = "Geometric / City Artwork"
    elif category == "landscape":
        strokes = generate_landscape_strokes(prompt, seed, None, width, height, palette_name)
        title = "Landscape Artwork"
    elif category == "creature":
        strokes = generate_creature_strokes(prompt, seed, None, width, height)
        title = "Creature Artwork"
    else:
        strokes = generate_character_strokes(prompt, seed, None, width, height, palette_name)
        title = "Character Portrait"

    # 歴史的に固定色を持つ風景・動物・幾何・FXも、UIで選んだパレットへ確実に収める。
    strokes = recolor_strokes_to_palette(strokes, palette_name)

    color_replacements = {
        base_colors[key]: effective_colors[key]
        for key in ("hair_main", "hair_shadow", "eye_light", "eye_dark")
        if base_colors[key] != effective_colors[key]
    }
    if color_replacements:
        strokes = [replace(stroke, color=color_replacements.get(stroke.color, stroke.color)) for stroke in strokes]

    has_fx_modifier = any(
        keyword in prompt.casefold()
        for keyword in ("magic", "magical", "spell", "aura", "rune", "魔法", "オーラ", "ルーン")
    )
    if category != "fx" and has_fx_modifier:
        strokes.extend(generate_manga_fx_strokes(prompt, seed + 7_919, None, width, height))

    # 指定されたブラシプロファイルの一括適用
    if brush_profile and brush_profile != "auto":
        normalized_profile = canonical_brush_profile(brush_profile)
        target_preset = brush_preset_for_profile(normalized_profile)
        profile_strokes: list[Stroke] = []
        for s in strokes:
            new_pts: list[StrokePoint] = []
            n = len(s.points) - 1
            for idx, pt in enumerate(s.points):
                t = idx / max(1, n)
                p = pressure_profile(t, normalized_profile, base=pt.pressure)
                new_pts.append(StrokePoint(pt.x, pt.y, p, pt.time_ms))
            profile_strokes.append(
                Stroke(
                    id=s.id,
                    points=new_pts,
                    brush_preset=target_preset,
                    color=s.color,
                    size_px=s.size_px,
                    layer_name=s.layer_name,
                    opacity=s.opacity,
                    is_eraser=s.is_eraser,
                )
            )
        strokes = profile_strokes

    # 固定px値で定義された既存モチーフもキャンバス解像度に追従させる。
    resolution_scale = max(0.5, min(6.0, min(width, height) / 600.0))
    if abs(resolution_scale - 1.0) > 1e-9:
        strokes = [replace(stroke, size_px=stroke.size_px * resolution_scale) for stroke in strokes]

    # レイヤー順序の抽出
    layer_order = ["Draft", "Flats", "Shading", "Lineart", "Highlights", "FX"]
    present_layers: list[str] = []
    for lyr in layer_order:
        if any(s.layer_name == lyr for s in strokes):
            present_layers.append(lyr)

    legacy_program = drawing_plan_to_stroke_program(
        DrawingPlan(
            prompt=prompt,
            seed=seed,
            strokes=strokes,
            title=title,
            iteration=1,
            layers=present_layers,
            metadata={"generator": "procedural"},
            canvas_width=width,
            canvas_height=height,
        )
    )
    foundations = _foundation_operations(category, prompt, effective_colors)
    if brush_profile and brush_profile != "auto":
        normalized_profile = canonical_brush_profile(brush_profile)
        target_preset = brush_preset_for_profile(normalized_profile)
        foundations = tuple(
            replace(
                operation,
                brush=replace(
                    operation.brush,
                    profile=normalized_profile,
                    preset_hint=target_preset,
                ),
            )
            for operation in foundations
        )
    return replace(
        legacy_program,
        operations=(*foundations, *legacy_program.operations),
        metadata={
            **dict(legacy_program.metadata),
            "generator": "procedural_v2",
            "prompt_category": category,
            "requested_count": count,
        },
    )
