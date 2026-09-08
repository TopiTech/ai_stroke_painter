"""プロンプトを複数の描画要素へ分解する、レンダラー非依存のシーン契約。"""

from __future__ import annotations

from dataclasses import dataclass
import re
from typing import Any, Literal

from .prompt_analyzer import analyze_prompt

SceneDomain = Literal["character", "creature", "landscape", "geometry", "fx", "unknown"]


def _contains(text: str, keyword: str) -> bool:
    if any(ord(character) > 127 for character in keyword):
        return keyword in text
    # ``focus lines`` のような複数語ラベルでも、末尾名詞の複数形を許容する。
    suffix = r"(?:s|es)?" if keyword and keyword[-1].isalpha() else ""
    return re.search(rf"(?<![a-z0-9]){re.escape(keyword)}{suffix}(?![a-z0-9])", text) is not None


def _collect(text: str, vocabulary: dict[str, tuple[str, ...]]) -> tuple[str, ...]:
    return tuple(name for name, keywords in vocabulary.items() if any(_contains(text, keyword) for keyword in keywords))


@dataclass(frozen=True)
class SceneSpec:
    """生成器間で共有する、複数ラベル対応のシーン意味表現。"""

    prompt: str
    subjects: tuple[str, ...] = ()
    environments: tuple[str, ...] = ()
    motifs: tuple[str, ...] = ()
    effects: tuple[str, ...] = ()
    style: str = "anime"
    composition: str = "centered"
    palette: str = "auto"
    primary_domain: SceneDomain = "unknown"
    required_elements: tuple[str, ...] = ()
    supported: bool = False

    def __post_init__(self) -> None:
        if not isinstance(self.prompt, str) or not self.prompt.strip():
            raise ValueError("SceneSpec.prompt は空でない文字列である必要があります")
        if self.primary_domain not in {"character", "creature", "landscape", "geometry", "fx", "unknown"}:
            raise ValueError(f"未対応の primary_domain です: {self.primary_domain}")
        for field_name in ("subjects", "environments", "motifs", "effects", "required_elements"):
            values = getattr(self, field_name)
            if not isinstance(values, tuple) or any(not isinstance(value, str) or not value for value in values):
                raise ValueError(f"SceneSpec.{field_name} は空でない文字列のタプルである必要があります")
        if not isinstance(self.supported, bool):
            raise ValueError("SceneSpec.supported は真偽値である必要があります")

    def as_dict(self) -> dict[str, Any]:
        return {
            "prompt": self.prompt,
            "subjects": list(self.subjects),
            "environments": list(self.environments),
            "motifs": list(self.motifs),
            "effects": list(self.effects),
            "style": self.style,
            "composition": self.composition,
            "palette": self.palette,
            "primary_domain": self.primary_domain,
            "required_elements": list(self.required_elements),
            "supported": self.supported,
        }


_SUBJECT_VOCABULARY: dict[str, tuple[str, ...]] = {
    "character": (
        "girl",
        "boy",
        "woman",
        "man",
        "person",
        "portrait",
        "character",
        "hero",
        "heroine",
        "人物",
        "少女",
        "少年",
        "女性",
        "男性",
    ),
    "cat": ("cat", "kitten", "猫"),
    "dog": ("dog", "puppy", "犬"),
    "bird": ("bird", "eagle", "鳥", "鷲"),
    "dragon": ("dragon", "竜", "龍", "ドラゴン"),
    "wolf": ("wolf", "狼"),
}

_ENVIRONMENT_VOCABULARY: dict[str, tuple[str, ...]] = {
    "mountain": ("mountain", "ridge", "cliff", "山", "稜線", "崖"),
    "sea": ("sea", "ocean", "coast", "beach", "海", "海岸", "浜"),
    "forest": ("forest", "woods", "nature", "森", "森林", "自然"),
    "garden": ("garden", "meadow", "park", "庭", "花畑", "草原", "公園"),
    "city": ("city", "street", "urban", "building", "skyline", "都市", "街", "ビル"),
    "cathedral": ("cathedral", "basilica", "大聖堂", "聖堂"),
    "interior": ("room", "interior", "classroom", "部屋", "室内", "教室"),
}

_MOTIF_VOCABULARY: dict[str, tuple[str, ...]] = {
    "sakura": ("sakura", "cherry blossom", "桜"),
    "clouds": ("cloud", "cumulus", "雲", "積乱雲", "入道雲"),
    "wave": ("wave", "great wave", "大波", "波", "北斎", "hokusai"),
    "mountain": ("mountain", "ridge", "山", "稜線"),
    "tree": ("tree", "pine", "木", "樹", "松"),
    "wildflowers": ("wildflower", "wild flower", "meadow flower", "野花", "花畑"),
    "rose": ("rose", "バラ", "薔薇"),
    "mandala": ("mandala", "kaleidoscope", "マンダラ", "万華鏡"),
    "magic_circle": ("magic circle", "magical circle", "rune circle", "魔法陣", "ルーン"),
    "cyber_city": ("cyberpunk city", "neon city", "cyber city", "サイバー都市", "ネオン街"),
}

_EFFECT_VOCABULARY: dict[str, tuple[str, ...]] = {
    "focus_lines": ("focus line", "radial line", "focus", "radial", "集中線", "放射線"),
    "speed_lines": ("speed line", "motion line", "流線", "スピード線"),
    "hatching": ("hatch", "crosshatch", "カケアミ", "ハッチング"),
    "glow": ("glow", "aura", "radiant", "発光", "オーラ", "光芒"),
    "magic_aura": ("magic", "magical", "spell", "魔法", "呪文"),
    "petals": ("falling petals", "petal drift", "花びら", "桜吹雪"),
}


def analyze_scene(prompt: str, *, palette: str = "auto") -> SceneSpec:
    """単一カテゴリへ潰さず、主役・環境・モチーフ・効果を独立抽出する。"""
    if not isinstance(prompt, str) or not prompt.strip():
        raise ValueError("prompt は空でない文字列である必要があります")
    normalized = prompt.strip()
    lowered = normalized.casefold()
    semantic = analyze_prompt(normalized)

    subjects = _collect(lowered, _SUBJECT_VOCABULARY)
    environments = _collect(lowered, _ENVIRONMENT_VOCABULARY)
    motifs = _collect(lowered, _MOTIF_VOCABULARY)
    effects = _collect(lowered, _EFFECT_VOCABULARY)
    # ``radial speed lines`` は1種類の集中線を表す複合語。明示的に両方を
    # 並記した場合以外は、同じ語句から focus/speed を二重計上しない。
    if (
        "focus_lines" in effects
        and "speed_lines" in effects
        and any(phrase in lowered for phrase in ("radial speed line", "focus radial speed line"))
        and not any(phrase in lowered for phrase in ("focus lines and speed lines", "集中線とスピード線"))
    ):
        effects = tuple(effect for effect in effects if effect != "speed_lines")

    creature_subjects = {"cat", "dog", "bird", "dragon", "wolf"}
    if "character" in subjects:
        primary_domain: SceneDomain = "character"
    elif any(subject in creature_subjects for subject in subjects):
        primary_domain = "creature"
    elif any(motif in {"mandala", "cyber_city"} for motif in motifs) or any(
        environment in {"city", "cathedral"} for environment in environments
    ):
        primary_domain = "geometry"
    elif environments or any(
        motif in {"sakura", "clouds", "wave", "mountain", "tree", "wildflowers", "rose"} for motif in motifs
    ):
        primary_domain = "landscape"
    elif effects or "magic_circle" in motifs:
        primary_domain = "fx"
    else:
        primary_domain = "unknown"

    if any(_contains(lowered, word) for word in ("portrait", "close-up", "close up", "顔アップ", "バストアップ")):
        composition = "portrait"
    elif any(_contains(lowered, word) for word in ("panorama", "wide", "widescreen", "パノラマ", "横長")):
        composition = "wide"
    elif semantic.character.view_angle != "auto":
        composition = semantic.character.view_angle
    else:
        composition = "centered"

    # ユーザーが明示した具体物だけを必須要素とする。環境は主役と独立して保持する。
    required_elements = tuple(dict.fromkeys((*subjects, *environments, *motifs, *effects)))
    return SceneSpec(
        prompt=normalized,
        subjects=subjects,
        environments=environments,
        motifs=motifs,
        effects=effects,
        style=semantic.art_style,
        composition=composition,
        palette=palette.strip().lower() if isinstance(palette, str) and palette.strip() else "auto",
        primary_domain=primary_domain,
        required_elements=required_elements,
        supported=primary_domain != "unknown",
    )
