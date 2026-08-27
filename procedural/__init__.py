"""プロシージャル・イラスト生成エンジンのパッケージエントリーポイント。"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import replace
import math
import re

from ..brushes import brush_preset_for_profile, canonical_brush_profile
from ..domain import DrawingPlan, Stroke, StrokePoint
from ..scene_spec import SceneSpec, analyze_scene
from ..stroke_program import (
    FILL_STROKE_BUDGET_RATIO,
    FillOperation,
    GradientFillOperation,
    ProgramBrush,
    ProgramOperation,
    ProgramPoint,
    StrokeProgram,
    compile_stroke_program,
    drawing_plan_to_stroke_program,
)
from .base import color_palette, pressure_profile, recolor_strokes_to_palette
from .character import character_feature_stroke_ids, generate_character_strokes
from .color_plan import ColorPlan, build_color_plan
from .composition import CompositionBox, CompositionPlan, plan_scene_composition, transform_strokes_to_box
from .creature import creature_feature_stroke_ids, generate_creature_strokes
from .geometry import generate_geometry_strokes, geometry_feature_stroke_ids
from .landscape import generate_landscape_strokes, landscape_feature_stroke_ids
from .manga_fx import generate_manga_fx_strokes, semantic_groups_for_manga_fx
from .render_graph import RenderGraph, RenderNode, compose_render_graph

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

_CREATURE_ELEMENTS = {"cat", "dog", "bird", "dragon", "wolf"}
_LANDSCAPE_ELEMENTS = {
    "mountain",
    "sea",
    "forest",
    "garden",
    "sakura",
    "clouds",
    "wave",
    "tree",
    "wildflowers",
    "rose",
}
_GEOMETRY_ELEMENTS = {"city", "cathedral", "mandala", "cyber_city"}


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
    # 空文字はパレット推定などの既存フォールバック経路で許可される。
    if isinstance(prompt, str) and prompt.strip():
        scene = analyze_scene(prompt)
        if scene.primary_domain != "unknown":
            return scene.primary_domain
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


def _required_subset(scene_spec: SceneSpec, supported: set[str]) -> tuple[str, ...]:
    return tuple(element for element in scene_spec.required_elements if element in supported)


def _effect_requirements(group_id: str, scene_spec: SceneSpec) -> tuple[str, ...]:
    if group_id == "magic-symbol":
        supported = {"magic_circle", "magic_aura"}
    elif group_id == "magic-rays":
        supported = {"glow"}
    elif group_id == "focus-lines":
        supported = {"focus_lines"}
    elif group_id == "speed-lines":
        supported = {"speed_lines"}
    elif group_id in {"hatch-forward", "hatch-backward"}:
        supported = {"hatching"}
    elif group_id == "glow-aura":
        supported = {"glow"}
    else:
        supported = set()
    return tuple(element for element in scene_spec.required_elements if element in supported)


def _fx_render_nodes(
    scene_spec: SceneSpec,
    prompt: str,
    seed: int,
    width: float,
    height: float,
    effect_box: CompositionBox,
    *,
    primary: bool,
) -> list[RenderNode]:
    strokes = transform_strokes_to_box(
        generate_manga_fx_strokes(prompt, seed, None, width, height),
        canvas_width=width,
        canvas_height=height,
        box=effect_box,
    )
    groups = semantic_groups_for_manga_fx(prompt, tuple(strokes))
    nodes: list[RenderNode] = []
    for index, group in enumerate(groups):
        minimum = group.minimum_count if primary else min(group.minimum_count, 4)
        nodes.append(
            RenderNode(
                id=f"effect-{group.id}",
                domain="fx",
                role="effect",
                strokes=group.strokes,
                z_index=80 + index,
                priority=group.priority if primary else max(55, group.priority - 10),
                minimum_count=minimum,
                bounds=effect_box.as_tuple(),
                required_for=_effect_requirements(group.id, scene_spec),
                atomic=group.atomic,
                selection_strategy=group.selection_strategy,
            )
        )
    return nodes


def _build_procedural_render_graph(
    scene_spec: SceneSpec,
    prompt: str,
    seed: int,
    width: float,
    height: float,
    palette_name: str,
    composition: CompositionPlan,
) -> tuple[RenderGraph, str]:
    """単一カテゴリ選択ではなく、背景・主役・効果を独立ノードとして構成する。"""
    nodes: list[RenderNode] = []
    primary = scene_spec.primary_domain
    natural_requirements = _required_subset(scene_spec, _LANDSCAPE_ELEMENTS)
    geometry_requirements = _required_subset(scene_spec, _GEOMETRY_ELEMENTS)

    if natural_requirements and primary != "landscape":
        environment_seed = seed + 3_571
        environment = generate_landscape_strokes(prompt, environment_seed, None, width, height, palette_name)
        background = tuple(stroke for stroke in environment if stroke.layer_name in {"Lineart", "Shading"})
        if background:
            background_ids = {stroke.id for stroke in background}
            nodes.append(
                RenderNode(
                    "background-landscape",
                    "landscape",
                    "background",
                    background,
                    z_index=5,
                    priority=78,
                    minimum_count=min(3, len(background)),
                    required_for=natural_requirements,
                    featured_ids=tuple(
                        stroke_id
                        for stroke_id in landscape_feature_stroke_ids(prompt, environment_seed)
                        if stroke_id in background_ids
                    ),
                )
            )
    if geometry_requirements and primary != "geometry":
        environment_seed = seed + 4_267
        environment = generate_geometry_strokes(prompt, environment_seed, None, width, height)
        background = tuple(stroke for stroke in environment if stroke.layer_name in {"Lineart", "Shading", "FX"})
        if background:
            background_ids = {stroke.id for stroke in background}
            nodes.append(
                RenderNode(
                    "background-geometry",
                    "geometry",
                    "background",
                    background,
                    z_index=10,
                    priority=80,
                    minimum_count=min(5, len(background)),
                    required_for=geometry_requirements,
                    featured_ids=tuple(
                        stroke_id
                        for stroke_id in geometry_feature_stroke_ids(prompt, environment_seed)
                        if stroke_id in background_ids
                    ),
                )
            )

    if primary == "character":
        character = transform_strokes_to_box(
            generate_character_strokes(prompt, seed, None, width, height, palette_name),
            canvas_width=width,
            canvas_height=height,
            box=composition.primary,
        )
        nodes.append(
            RenderNode(
                "subject-character",
                "character",
                "subject",
                character,
                z_index=45,
                priority=100,
                minimum_count=min(10, len(character)),
                bounds=composition.primary.as_tuple(),
                required_for=("character",) if "character" in scene_spec.subjects else (),
                featured_ids=tuple(
                    stroke_id
                    for stroke_id in character_feature_stroke_ids(seed)
                    if stroke_id in {s.id for s in character}
                ),
            )
        )
        title = "Character Portrait"
    elif primary == "creature":
        title = "Creature Artwork"
    elif primary == "landscape":
        landscape = tuple(generate_landscape_strokes(prompt, seed, None, width, height, palette_name))
        landscape_ids = {stroke.id for stroke in landscape}
        nodes.append(
            RenderNode(
                "subject-landscape",
                "landscape",
                "subject",
                landscape,
                z_index=30,
                priority=100,
                minimum_count=min(8, len(landscape)),
                required_for=natural_requirements,
                featured_ids=tuple(
                    stroke_id for stroke_id in landscape_feature_stroke_ids(prompt, seed) if stroke_id in landscape_ids
                ),
            )
        )
        title = "Landscape Artwork"
    elif primary == "geometry":
        geometry = tuple(generate_geometry_strokes(prompt, seed, None, width, height))
        geometry_ids = {stroke.id for stroke in geometry}
        nodes.append(
            RenderNode(
                "subject-geometry",
                "geometry",
                "subject",
                geometry,
                z_index=30,
                priority=100,
                minimum_count=min(6, len(geometry)),
                required_for=geometry_requirements,
                featured_ids=tuple(
                    stroke_id for stroke_id in geometry_feature_stroke_ids(prompt, seed) if stroke_id in geometry_ids
                ),
            )
        )
        title = "Geometric / City Artwork"
    elif primary == "fx":
        title = "Manga FX Artwork"
    else:
        fallback = transform_strokes_to_box(
            generate_character_strokes(prompt, seed, None, width, height, palette_name),
            canvas_width=width,
            canvas_height=height,
            box=composition.primary,
        )
        nodes.append(
            RenderNode(
                "subject-fallback",
                "unknown",
                "subject",
                fallback,
                z_index=45,
                priority=100,
                minimum_count=min(10, len(fallback)),
                bounds=composition.primary.as_tuple(),
                featured_ids=tuple(
                    stroke_id
                    for stroke_id in character_feature_stroke_ids(seed)
                    if stroke_id in {s.id for s in fallback}
                ),
            )
        )
        title = "Character Portrait"

    # 人物と動物、または複数動物を別ノード・別位置へ配置する。
    creature_subjects = tuple(subject for subject in scene_spec.subjects if subject in _CREATURE_ELEMENTS)
    creature_boxes = (composition.primary, *composition.companions)
    for index, subject in enumerate(creature_subjects):
        creature_seed = seed if primary == "creature" and len(creature_subjects) == 1 else seed + 5_101 + index * 97
        if primary == "creature" and len(creature_subjects) == 1:
            raw = generate_creature_strokes(prompt, creature_seed, None, width, height)
        else:
            raw = generate_creature_strokes(subject, creature_seed, None, width, height)
        box = creature_boxes[min(index + (1 if primary == "character" else 0), len(creature_boxes) - 1)]
        creature = transform_strokes_to_box(
            raw,
            canvas_width=width,
            canvas_height=height,
            box=box,
        )
        nodes.append(
            RenderNode(
                f"subject-{subject}",
                "creature",
                "subject",
                creature,
                z_index=50 + index,
                priority=96 - index,
                minimum_count=min(8, len(creature)),
                bounds=box.as_tuple(),
                required_for=(subject,),
                featured_ids=tuple(
                    stroke_id
                    for stroke_id in creature_feature_stroke_ids(subject, creature_seed)
                    if stroke_id in {s.id for s in creature}
                ),
            )
        )

    supported_fx = {"focus_lines", "speed_lines", "hatching", "glow", "magic_aura", "magic_circle"}
    has_supported_fx = any(element in supported_fx for element in scene_spec.required_elements)
    if primary == "fx" or has_supported_fx:
        nodes.extend(
            _fx_render_nodes(
                scene_spec,
                prompt,
                seed if primary == "fx" else seed + 7_919,
                width,
                height,
                composition.effect,
                primary=primary == "fx",
            )
        )

    return RenderGraph(scene_spec, tuple(nodes)), title


def _apply_color_roles(strokes: list[Stroke], graph: RenderGraph, color_plan: ColorPlan) -> list[Stroke]:
    node_by_stroke = {stroke.id: node for node in graph.nodes for stroke in node.strokes}
    adjusted: list[Stroke] = []
    for stroke in strokes:
        node = node_by_stroke.get(stroke.id)
        role = node.role if node is not None else None
        if role == "background":
            if stroke.layer_name == "Lineart":
                stroke = replace(stroke, color=color_plan.background_detail, opacity=min(stroke.opacity, 0.62))
            elif stroke.layer_name == "Shading":
                stroke = replace(stroke, color=color_plan.background_deep, opacity=min(stroke.opacity, 0.45))
            elif stroke.layer_name == "FX":
                stroke = replace(
                    stroke,
                    color=color_plan.background_detail,
                    opacity=min(stroke.opacity, 0.52),
                    layer_name="Lineart",
                )
        elif role == "subject" and stroke.layer_name == "Lineart":
            subject_line = (
                color_plan.accent
                if color_plan.dark_background and node is not None and node.domain in {"creature", "geometry"}
                else color_plan.lineart
            )
            stroke = replace(stroke, color=subject_line)
        elif role == "effect":
            if stroke.layer_name in {"Lineart", "Shading"}:
                effect_line = color_plan.accent if color_plan.dark_background else color_plan.lineart
                stroke = replace(stroke, color=effect_line)
            elif stroke.layer_name == "FX":
                stroke = replace(stroke, color=color_plan.accent)
            elif stroke.layer_name == "Highlights":
                stroke = replace(stroke, color=color_plan.highlight)
        adjusted.append(stroke)
    return adjusted


def _foundation_operations(
    category: str,
    prompt: str,
    colors: Mapping[str, str],
    color_plan: ColorPlan,
    composition: CompositionPlan,
    scene_spec: SceneSpec,
    stroke_budget: int | None,
) -> tuple[ProgramOperation, ...]:
    """背景・環境・主役の順に大きな明暗面を置き、少数筆でも立体を成立させる。"""

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

    strict_palette = color_plan.palette_name in {"monochrome", "sumie", "sumi_e", "ink"}

    def role_color(name: str, fallback: str) -> str:
        value = colors.get(name, fallback)
        return value if isinstance(value, str) and value else fallback

    def value_fill(
        operation_id: str,
        polygon: tuple[ProgramPoint, ...],
        value_colors: tuple[str, ...],
        *,
        size: float,
        profile: str = "airbrush",
        opacity: float = 1.0,
        angle_deg: float = 0.0,
        low_budget_color: str | None = None,
    ) -> ProgramOperation:
        unique_colors = tuple(dict.fromkeys(value_colors))
        # 大きな面に十分な走査線を割けない場合は、段階状グラデーションより
        # 意図した代表値の一枚面を優先する。水墨系は常に登録色だけを使う。
        constrained_solid = low_budget_color is not None and stroke_budget is not None and stroke_budget < 48
        if strict_palette or len(unique_colors) < 2 or constrained_solid:
            solid_color = low_budget_color if constrained_solid and low_budget_color is not None else unique_colors[-1]
            return fill(
                operation_id,
                polygon,
                solid_color,
                size=size,
                profile=profile,
                opacity=opacity,
            )
        return GradientFillOperation(
            id=f"foundation-{operation_id}",
            polygon=polygon,
            colors=unique_colors,
            brush=ProgramBrush(
                profile=profile,
                color=unique_colors[0],
                size=size,
                opacity=opacity,
            ),
            layer="Flats",
            spacing=0.55,
            style="linear",
            angle_deg=angle_deg,
        )

    canvas = (ProgramPoint(0, 0), ProgramPoint(1, 0), ProgramPoint(1, 1), ProgramPoint(0, 1))

    def mapped(polygon: tuple[ProgramPoint, ...], box: CompositionBox) -> tuple[ProgramPoint, ...]:
        return tuple(ProgramPoint(*box.map_point(point.x, point.y), pressure=point.pressure) for point in polygon)

    def primary(polygon: tuple[ProgramPoint, ...]) -> tuple[ProgramPoint, ...]:
        return mapped(polygon, composition.primary)

    background = value_fill(
        "background",
        canvas,
        (color_plan.background_deep, color_plan.background_detail, color_plan.background),
        size=0.12,
        profile="airbrush",
        low_budget_color=color_plan.background,
    )
    foundations: list[ProgramOperation] = [background]

    geometry_elements = set(scene_spec.environments) | set(scene_spec.motifs)
    if geometry_elements & {"city", "cyber_city", "cathedral"}:
        # 建物の細かな段差は Lineart 側へ任せ、低予算の面は地平線を横断する
        # 大きな都市シルエットに限定する。狭い塔を太筆で丸く潰すのを避ける。
        skyline = (
            ProgramPoint(0.0, 0.58),
            ProgramPoint(1.0, 0.58),
            ProgramPoint(1.0, 0.90),
            ProgramPoint(0.0, 0.90),
        )
        foundations.append(
            value_fill(
                "skyline-mass",
                skyline,
                (color_plan.background_detail, color_plan.background_deep),
                size=0.075,
                profile="marker",
                low_budget_color=color_plan.background_detail,
            )
        )

    if category == "character":
        foundations.extend(
            (
                value_fill(
                    "back-hair",
                    primary(_ellipse_points(0.5, 0.45, 0.29, 0.40)),
                    (
                        role_color("hair_main", color_plan.subject_mid),
                        role_color("hair_shadow", color_plan.subject_shadow),
                    ),
                    size=0.075,
                    angle_deg=90.0,
                ),
                value_fill(
                    "clothing",
                    primary(
                        (
                            ProgramPoint(0.22, 1),
                            ProgramPoint(0.32, 0.72),
                            ProgramPoint(0.68, 0.72),
                            ProgramPoint(0.78, 1),
                        )
                    ),
                    (
                        role_color("cloth_main", color_plan.subject_mid),
                        role_color("cloth_shadow", color_plan.subject_shadow),
                    ),
                    size=0.08,
                    profile="marker",
                    angle_deg=8.0,
                ),
                value_fill(
                    "face",
                    primary(_ellipse_points(0.5, 0.46, 0.205, 0.285)),
                    (
                        role_color("skin_base", color_plan.subject_base),
                        role_color("skin_shadow", color_plan.subject_shadow),
                    ),
                    size=0.065,
                    angle_deg=90.0,
                ),
            )
        )
    elif category == "landscape":
        prompt_lower = prompt.casefold()
        is_wave = any(keyword in prompt_lower for keyword in ("wave", "ocean", "sea", "波", "海", "北斎"))
        is_rose = any(keyword in prompt_lower for keyword in ("rose", "バラ", "薔薇"))
        is_wildflower = any(
            keyword in prompt_lower
            for keyword in ("wildflower", "wild flower", "garden", "meadow", "野花", "花畑", "庭園")
        )
        if is_wave:
            foundations[0] = value_fill(
                "sky",
                canvas,
                (
                    role_color("sky_zenith", color_plan.background_deep),
                    role_color("sky_horizon", color_plan.background),
                ),
                size=0.12,
                profile="airbrush",
                low_budget_color=color_plan.background_detail,
            )
            foundations.append(
                value_fill(
                    "sea",
                    (ProgramPoint(0, 0.48), ProgramPoint(1, 0.48), ProgramPoint(1, 1), ProgramPoint(0, 1)),
                    (
                        color_plan.background_detail,
                        color_plan.background_deep,
                    ),
                    size=0.09,
                    profile="watercolor",
                    low_budget_color=color_plan.background_deep,
                )
            )
        elif is_rose or is_wildflower:
            foundations[0] = value_fill(
                "background",
                canvas,
                (color_plan.background_deep, color_plan.background),
                size=0.12,
                profile="watercolor",
                low_budget_color=color_plan.background,
            )
            foundations.append(
                value_fill(
                    "flower-mass",
                    _ellipse_points(0.5, 0.45, 0.30, 0.28),
                    (
                        color_plan.highlight,
                        role_color("hair_main", color_plan.subject_mid),
                        role_color("hair_shadow", color_plan.subject_shadow),
                    ),
                    size=0.055,
                    profile="watercolor",
                    angle_deg=16.0,
                )
            )
        else:
            foundations[0] = value_fill(
                "sky",
                canvas,
                (
                    role_color("sky_zenith", color_plan.background_deep),
                    role_color("sky_horizon", color_plan.background),
                ),
                size=0.12,
                profile="airbrush",
                low_budget_color=color_plan.background_detail,
            )
            foundations.extend(
                (
                    value_fill(
                        "ground",
                        (ProgramPoint(0, 0.55), ProgramPoint(1, 0.50), ProgramPoint(1, 1), ProgramPoint(0, 1)),
                        (
                            role_color("hair_main", color_plan.subject_mid),
                            role_color("hair_shadow", color_plan.subject_shadow),
                        ),
                        size=0.09,
                        profile="watercolor",
                        low_budget_color=color_plan.subject_mid,
                    ),
                    value_fill(
                        "mountain",
                        (
                            ProgramPoint(0, 0.62),
                            ProgramPoint(0.28, 0.28),
                            ProgramPoint(0.48, 0.58),
                            ProgramPoint(0.72, 0.35),
                            ProgramPoint(1, 0.62),
                        ),
                        (color_plan.background_detail, role_color("hair_shadow", color_plan.subject_shadow)),
                        size=0.07,
                        profile="marker",
                        angle_deg=10.0,
                    ),
                ),
            )
    elif category == "creature":
        foundations.append(
            value_fill(
                "body",
                primary(_ellipse_points(0.5, 0.56, 0.28, 0.34)),
                (
                    role_color("hair_main", color_plan.subject_mid),
                    role_color("hair_shadow", color_plan.subject_shadow),
                ),
                size=0.07,
                angle_deg=90.0,
            )
        )
    elif category == "fx":
        # FX は既存作品へ重ねる用途が主なので、全面背景でキャンバスを覆わない。
        return ()

    for index, box in enumerate(composition.companions):
        foundations.append(
            value_fill(
                f"companion-{index + 1}-body",
                mapped(_ellipse_points(0.5, 0.58, 0.31, 0.32), box),
                (color_plan.subject_mid, color_plan.subject_shadow),
                size=0.065,
                angle_deg=90.0,
            )
        )
    return tuple(foundations)


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
    scene_spec = analyze_scene(prompt, palette=palette_name)
    category = _prompt_category(prompt)
    is_auto_palette = not palette_name or palette_name.strip().lower() == "auto"
    resolved_palette = infer_palette_from_prompt(prompt) if is_auto_palette else palette_name.strip().lower()
    base_colors = color_palette(resolved_palette)
    effective_colors = _prompt_palette(prompt, resolved_palette)
    composition = plan_scene_composition(scene_spec)
    color_plan = build_color_plan(resolved_palette, effective_colors)

    render_graph, title = _build_procedural_render_graph(
        scene_spec,
        prompt,
        seed,
        width,
        height,
        resolved_palette,
        composition,
    )
    foundations = _foundation_operations(
        category,
        prompt,
        effective_colors,
        color_plan,
        composition,
        scene_spec,
        count,
    )
    if count is None:
        scene_budget = None
    else:
        # compiler と同じ比率で面の完全性を先に予約し、残りを意味ノードへ配る。
        fill_count = len(foundations)
        reserved_for_fills = min(count, max(fill_count, round(count * FILL_STROKE_BUDGET_RATIO))) if fill_count else 0
        scene_budget = max(0, count - reserved_for_fills)
    composed = compose_render_graph(render_graph, scene_budget)
    strokes = list(composed.strokes)

    # 歴史的に固定色を持つ風景・動物・幾何・FXも、UIで選んだパレットへ確実に収める。
    strokes = recolor_strokes_to_palette(strokes, resolved_palette)
    strokes = _apply_color_roles(strokes, render_graph, color_plan)

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

    if strokes:
        path_operations = drawing_plan_to_stroke_program(
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
        ).operations
    else:
        path_operations = ()
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
    return StrokeProgram(
        prompt=prompt,
        seed=seed,
        operations=(*foundations, *path_operations),
        canvas_width=width,
        canvas_height=height,
        title=title,
        iteration=1,
        metadata={
            "generator": "procedural_v2",
            "prompt_category": category,
            "palette": palette_name,
            "resolved_palette": resolved_palette,
            "effective_palette": resolved_palette,
            "requested_count": count,
            "overlay": category == "fx",
            "scene_spec": scene_spec.as_dict(),
            "required_elements": list(scene_spec.required_elements),
            "rendered_elements": list(composed.rendered_elements),
            "missing_elements": list(composed.missing_elements),
            "semantic_fidelity": round(composed.semantic_fidelity, 4),
            "semantic_manifest": list(composed.manifest),
            "stroke_node_map": composed.stroke_node_map,
            "render_graph_version": 1,
            "semantic_stroke_budget": scene_budget,
            "composition_plan": composition.as_dict(),
            "color_plan": color_plan.as_dict(),
            "foundation_value_plan": [
                {
                    "id": operation.id,
                    "kind": operation.kind,
                    "colors": list(operation.colors)
                    if isinstance(operation, GradientFillOperation)
                    else [operation.brush.color],
                }
                for operation in foundations
            ],
        },
    )
