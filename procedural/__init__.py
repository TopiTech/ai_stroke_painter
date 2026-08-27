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
    "infer_palette_from_prompt",
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
            "wildflower",
            "wildflowers",
            "garden",
            "meadow",
            "petal",
            "petals",
            "bouquet",
            "blossom",
            "cherry blossom",
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


def infer_palette_from_prompt(prompt: str) -> str:
    """自然言語プロンプトの内容・画風・主題から最適なカラーパレットを自律的に推定する。"""
    normalized = prompt.casefold()

    def contains_keyword(keyword: str) -> bool:
        if any(ord(character) > 127 for character in keyword):
            return keyword in normalized
        suffix = r"(?:s|es)?" if " " not in keyword and "-" not in keyword else ""
        return re.search(rf"(?<![a-z0-9]){re.escape(keyword)}{suffix}(?![a-z0-9])", normalized) is not None

    style_keywords: dict[str, tuple[str, ...]] = {
        "sumie": (
            "sumie",
            "sumi-e",
            "ink wash",
            "suibokuga",
            "pine tree",
            "zen",
            "水墨画",
            "水墨",
            "墨絵",
            "墨",
            "毛筆",
            "枯山水",
            "竹林",
            "松",
        ),
        "cyber_gold": (
            "cyber gold",
            "cybergold",
            "cyber_gold",
            "golden dragon",
            "gold leaf",
            "luxury gold",
            "サイバーゴールド",
            "金箔",
            "黄金",
        ),
        "cyberpunk": (
            "cyberpunk",
            "cyber",
            "neon",
            "sci-fi",
            "scifi",
            "futuristic",
            "synthwave",
            "matrix",
            "サイバーパンク",
            "サイバー",
            "ネオン",
            "近未来",
            "電脳",
        ),
        "watercolor": (
            "watercolor",
            "watercolour",
            "aquarelle",
            "wet on wet",
            "soft wash",
            "水彩",
            "透明水彩",
            "水彩画",
            "滲み",
        ),
        "botanical": (
            "botanical",
            "wildflower",
            "wildflowers",
            "rose",
            "roses",
            "flower",
            "flowers",
            "bouquet",
            "plant",
            "plants",
            "floral",
            "herbal",
            "leaves",
            "petals",
            "ボタニカル",
            "野花",
            "草花",
            "薔薇",
            "バラ",
            "花束",
            "花柄",
            "植物",
        ),
        "nature": (
            "nature",
            "landscape",
            "mountain",
            "mountains",
            "forest",
            "ocean",
            "sea",
            "wave",
            "hokusai",
            "sakura",
            "cherry blossom",
            "meadow",
            "cliff",
            "自然",
            "風景",
            "山",
            "海",
            "森",
            "桜",
            "大自然",
            "アースカラー",
            "浮世絵",
        ),
        "pastel": (
            "pastel",
            "yumekawa",
            "fairy",
            "dreamy",
            "cute",
            "kawaii",
            "sweet",
            "cotton candy",
            "ribbon",
            "パステル",
            "ゆめかわ",
            "ファンシー",
            "メルヘン",
            "ふんわり",
            "かわいい",
            "可愛い",
        ),
        "retro_pop": (
            "retro pop",
            "retro_pop",
            "80s",
            "eighties",
            "vintage pop",
            "city pop",
            "disco",
            "レトロポップ",
            "80年代",
            "昭和レトロ",
            "シティポップ",
        ),
        "dark_fantasy": (
            "dark fantasy",
            "dark_fantasy",
            "gothic",
            "vampire",
            "horror",
            "demon",
            "shadow",
            "darkness",
            "abyss",
            "blood",
            "cursed",
            "ダークファンタジー",
            "ゴシック",
            "ホラー",
            "暗黒",
            "漆黒",
            "魔界",
            "深淵",
            "吸血鬼",
        ),
        "sepia": (
            "sepia",
            "antique",
            "vintage",
            "old photo",
            "nostalgic",
            "parchment",
            "historical",
            "セピア",
            "アンティーク",
            "ヴィンテージ",
            "古写真",
            "古風",
            "ノスタルジック",
            "セピア調",
        ),
        "monochrome": (
            "monochrome",
            "monoral",
            "black and white",
            "b&w",
            "grayscale",
            "sketch",
            "lineart",
            "speed lines",
            "focus lines",
            "crosshatch",
            "manga lines",
            "モノクロ",
            "白黒",
            "グレースケール",
            "線画",
            "集中線",
            "流線",
            "カケアミ",
            "漫画原稿",
        ),
        "anime": (
            "anime",
            "manga",
            "girl",
            "boy",
            "hero",
            "heroine",
            "portrait",
            "character",
            "アニメ",
            "美少女",
            "美少年",
            "キャラクター",
        ),
    }

    medium_keywords: dict[str, tuple[str, ...]] = {
        "sumie": ("sumie", "sumi-e", "ink wash", "suibokuga", "水墨画", "水墨", "墨絵"),
        "cyber_gold": ("cyber gold", "cybergold", "cyber_gold", "サイバーゴールド"),
        "cyberpunk": ("cyberpunk", "サイバーパンク"),
        "watercolor": ("watercolor", "watercolour", "aquarelle", "水彩", "透明水彩", "水彩画"),
        "botanical": ("botanical", "ボタニカル"),
        "pastel": ("pastel", "yumekawa", "パステル", "ゆめかわ"),
        "retro_pop": ("retro pop", "retro_pop", "80s", "eighties", "レトロポップ", "80年代"),
        "dark_fantasy": ("dark fantasy", "dark_fantasy", "gothic", "ダークファンタジー", "ゴシック"),
        "sepia": ("sepia", "セピア", "セピア調"),
        "monochrome": (
            "monochrome",
            "monoral",
            "grayscale",
            "speed lines",
            "focus lines",
            "モノクロ",
            "白黒",
            "グレースケール",
            "集中線",
        ),
    }

    scores: dict[str, int] = {}
    for palette_key, kws in style_keywords.items():
        score = sum(1 for kw in kws if contains_keyword(kw))
        med_kws = medium_keywords.get(palette_key, ())
        med_score = sum(3 for kw in med_kws if contains_keyword(kw))
        total_score = score + med_score
        if total_score > 0:
            scores[palette_key] = total_score

    if scores:
        priority_order = (
            "sumie",
            "cyber_gold",
            "cyberpunk",
            "watercolor",
            "dark_fantasy",
            "sepia",
            "retro_pop",
            "monochrome",
            "botanical",
            "pastel",
            "nature",
            "anime",
        )
        return max(
            scores.keys(),
            key=lambda p: (scores[p], -priority_order.index(p) if p in priority_order else -999),
        )

    category = _prompt_category(prompt)
    if category == "landscape":
        return "nature"
    if category == "geometry":
        return "cyberpunk"
    if category == "creature":
        return "nature"
    if category == "fx":
        return "monochrome"
    return "anime"


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
        "brown": "#6b4226",
        "black": "#292632",
        "white": "#e8edf5",
        "赤": "#d94b58",
        "青": "#4776d0",
        "緑": "#4f9b68",
        "紫": "#8554b3",
        "ピンク": "#e86f9d",
        "茶": "#6b4226",
        "茶髪": "#6b4226",
        "金髪": "#e7bd55",
        "黒髪": "#292632",
        "白髪": "#e8edf5",
    }
    for cue, color in color_cues.items():
        hair_phrases = [f"{cue} hair", f"{cue}-haired", f"{cue}髪", f"{cue}い髪", f"{cue}の髪"]
        if cue in {"金髪", "黒髪", "白髪", "茶髪"}:
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
        is_rose = any(keyword in prompt_lower for keyword in ("rose", "バラ", "薔薇"))
        is_wildflower = any(
            keyword in prompt_lower
            for keyword in ("wildflower", "wild flower", "garden", "meadow", "野花", "花畑", "庭園")
        )
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
        if is_rose or is_wildflower:
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
    if category == "fx":
        # FX は既存作品へ重ねる用途が主なので、全面背景でキャンバスを覆わない。
        return ()
    return (fill("background", canvas, colors["cloth_shadow"], size=0.12, profile="airbrush"),)


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
    is_auto_palette = not palette_name or palette_name.strip().lower() == "auto"
    resolved_palette = infer_palette_from_prompt(prompt) if is_auto_palette else palette_name.strip().lower()
    base_colors = color_palette(resolved_palette)
    effective_colors = _prompt_palette(prompt, resolved_palette)

    # カテゴリ判定
    if category == "fx":
        strokes = generate_manga_fx_strokes(prompt, seed, None, width, height)
        title = "Manga FX Artwork"
    elif category == "geometry":
        strokes = generate_geometry_strokes(prompt, seed, None, width, height)
        title = "Geometric / City Artwork"
    elif category == "landscape":
        strokes = generate_landscape_strokes(prompt, seed, None, width, height, resolved_palette)
        title = "Landscape Artwork"
    elif category == "creature":
        strokes = generate_creature_strokes(prompt, seed, None, width, height)
        title = "Creature Artwork"
    else:
        strokes = generate_character_strokes(prompt, seed, None, width, height, resolved_palette)
        title = "Character Portrait"

    # 主役と舞台を排他的にせず、混合プロンプトでは軽量な環境輪郭を主役の背後へ追加する。
    prompt_lower = prompt.casefold()
    has_landscape_environment = any(
        keyword in prompt_lower
        for keyword in ("landscape", "mountain", "ocean", "forest", "garden", "山", "海", "森", "庭")
    )
    has_geometry_environment = any(
        keyword in prompt_lower for keyword in ("city", "building", "skyline", "street", "都市", "街", "ビル")
    )
    if category in {"character", "creature"} and has_landscape_environment:
        environment = generate_landscape_strokes(prompt, seed + 3_571, None, width, height, resolved_palette)
        strokes = [stroke for stroke in environment if stroke.layer_name in {"Lineart", "Shading"}] + strokes
    if category in {"character", "creature"} and has_geometry_environment:
        environment = generate_geometry_strokes(prompt, seed + 4_267, None, width, height)
        strokes = [stroke for stroke in environment if stroke.layer_name in {"Lineart", "Shading"}] + strokes

    has_fx_modifier = any(
        keyword in prompt.casefold()
        for keyword in ("magic", "magical", "spell", "aura", "rune", "魔法", "オーラ", "ルーン")
    )
    if category != "fx" and has_fx_modifier:
        strokes.extend(generate_manga_fx_strokes(prompt, seed + 7_919, None, width, height))

    # 歴史的に固定色を持つ風景・動物・幾何・FXも、UIで選んだパレットへ確実に収める。
    strokes = recolor_strokes_to_palette(strokes, resolved_palette)

    hair_replacements = {
        base_colors[key]: effective_colors[key]
        for key in ("hair_main", "hair_shadow")
        if base_colors[key] != effective_colors[key]
    }
    eye_replacements = {
        base_colors[key]: effective_colors[key]
        for key in ("eye_light", "eye_dark")
        if base_colors[key] != effective_colors[key]
    }
    if hair_replacements or eye_replacements:

        def _safe_color_replace(stroke: Stroke) -> Stroke:
            # Lineart レイヤーの主線はパレット共有色による意図せぬ置換から保護する
            if stroke.layer_name == "Lineart":
                return stroke
            sid = stroke.id.lower()
            # 背景ストローク（空、山、波、雲、木等）も保護する
            if any(
                bg in sid for bg in ("sky", "mountain", "wave", "ground", "sea", "cloud", "tree", "mandala", "city")
            ):
                return stroke
            if (
                hair_replacements
                and stroke.color in hair_replacements
                and (
                    any(k in sid for k in ("hair", "bang", "ahoge", "flyaway", "ponytail", "twintail"))
                    or stroke.layer_name in {"Flats", "Shading"}
                )
            ):
                return replace(stroke, color=hair_replacements[stroke.color])
            if (
                eye_replacements
                and stroke.color in eye_replacements
                and (
                    any(k in sid for k in ("eye", "iris", "pupil"))
                    or stroke.layer_name in {"Flats", "Shading", "Highlights"}
                )
            ):
                return replace(stroke, color=eye_replacements[stroke.color])
            return stroke

        strokes = [_safe_color_replace(stroke) for stroke in strokes]

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
    # 基準解像度 600px（基準最小辺）に対し、超高解像度（4K/8K）から低解像度まで自然にスケーリング
    resolution_scale = max(0.35, min(12.0, min(width, height) / 600.0))
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
            "palette": palette_name,
            "resolved_palette": resolved_palette,
            "effective_palette": resolved_palette,
            "requested_count": count,
            "overlay": category == "fx",
        },
    )
