"""プロシージャル・イラスト生成エンジンのパッケージエントリーポイント。"""

from __future__ import annotations

from ..domain import DrawingPlan
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
]


def generate_procedural_plan(
    prompt: str,
    seed: int,
    count: int,
    width: float,
    height: float,
    palette_name: str = "anime",
) -> DrawingPlan:
    """自然言語プロンプトの意図を自動解析し、最適なプロシージャルイラスト計画を生成する。"""
    prompt_l = prompt.lower()

    # カテゴリ判定
    if any(k in prompt_l for k in ["fx", "集中線", "流線", "魔法", "magic", "rune", "speed", "hatch", "カケアミ"]):
        strokes = generate_manga_fx_strokes(prompt, seed, count, width, height)
        title = "Manga FX Artwork"
    elif any(k in prompt_l for k in ["mandala", "マンダラ", "幾何", "geometry", "city", "都市", "ビル", "cyber"]):
        strokes = generate_geometry_strokes(prompt, seed, count, width, height)
        title = "Geometric / City Artwork"
    elif any(
        k in prompt_l
        for k in [
            "land",
            "山",
            "mountain",
            "wave",
            "波",
            "海",
            "ocean",
            "tree",
            "木",
            "flower",
            "花",
            "rose",
            "バラ",
            "sakura",
            "桜",
            "風景",
        ]
    ):
        strokes = generate_landscape_strokes(prompt, seed, count, width, height, palette_name)
        title = "Landscape Artwork"
    elif any(k in prompt_l for k in ["cat", "猫", "dog", "犬", "bird", "鳥", "dragon", "ドラゴン", "動物", "animal"]):
        strokes = generate_creature_strokes(prompt, seed, count, width, height)
        title = "Creature Artwork"
    else:
        strokes = generate_character_strokes(prompt, seed, count, width, height, palette_name)
        title = "Character Portrait"

    # レイヤー順序の抽出
    layer_order = ["Draft", "Flats", "Shading", "Lineart", "Highlights", "FX"]
    present_layers: list[str] = []
    for lyr in layer_order:
        if any(s.layer_name == lyr for s in strokes):
            present_layers.append(lyr)

    return DrawingPlan(
        prompt=prompt,
        seed=seed,
        strokes=strokes,
        title=title,
        iteration=1,
        layers=present_layers,
    )
