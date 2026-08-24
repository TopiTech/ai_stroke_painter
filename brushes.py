"""描画エンジン間で共有する意味的ブラシプロファイル定義。"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class BrushProfileDefinition:
    key: str
    preset_hint: str
    candidates: tuple[str, ...]
    keywords: tuple[str, ...]


BRUSH_PROFILES: dict[str, BrushProfileDefinition] = {
    "auto": BrushProfileDefinition("auto", "Basic-5 Size", ("Basic-5 Size", "Basic-1"), ("basic",)),
    "gpen": BrushProfileDefinition(
        "gpen",
        "Ink-2 Fineliner",
        ("Ink-2 Fineliner", "Ink-3 Gpen", "G-Pen"),
        ("ink", "gpen", "g-pen", "pen"),
    ),
    "marupen": BrushProfileDefinition(
        "marupen",
        "Ink-1 Precision",
        ("Ink-1 Precision", "Ink-2 Fineliner", "Mapping Pen"),
        ("precision", "ink", "mapping", "pen"),
    ),
    "brush": BrushProfileDefinition(
        "brush",
        "Wet-1 Water",
        ("Wet-1 Water", "Dry Bristles"),
        ("bristle", "paint", "calligraphy"),
    ),
    "marker": BrushProfileDefinition(
        "marker",
        "Marker-1 Broad",
        ("Marker-1 Broad", "Marker Details", "Basic-5 Size"),
        ("marker", "broad", "felt"),
    ),
    "pencil": BrushProfileDefinition(
        "pencil",
        "Pencil-2",
        ("Pencil-2", "Pencil-4 Soft", "Charcoal Pencil"),
        ("pencil", "charcoal", "graphite", "chalk"),
    ),
    "watercolor": BrushProfileDefinition(
        "watercolor",
        "Wet Textured Soft",
        ("Wet Textured Soft", "Wet-1 Water", "Watercolor Texture"),
        ("watercolor", "water", "wet", "wash"),
    ),
    "airbrush": BrushProfileDefinition(
        "airbrush",
        "Airbrush Soft",
        ("Airbrush Soft", "Airbrush Static", "Airbrush Basic"),
        ("airbrush", "spray", "soft"),
    ),
    "eraser": BrushProfileDefinition(
        "eraser",
        "Eraser Small",
        ("Eraser Small", "Eraser Soft", "Eraser Circle", "Eraser"),
        ("eraser",),
    ),
}


_PROFILE_ALIASES = {
    "pen": "gpen",
    "ink": "gpen",
    "fude": "brush",
    "calligraphy": "brush",
    "water": "watercolor",
    "spray": "airbrush",
    "soft": "airbrush",
}


def canonical_brush_profile(profile: str | None) -> str:
    key = (profile or "auto").strip().lower()
    key = _PROFILE_ALIASES.get(key, key)
    return key if key in BRUSH_PROFILES else "auto"


def brush_definition(profile: str | None) -> BrushProfileDefinition:
    return BRUSH_PROFILES[canonical_brush_profile(profile)]


def brush_preset_for_profile(profile: str | None, fallback: str = "Basic-5 Size") -> str:
    definition = brush_definition(profile)
    return definition.preset_hint or fallback


def infer_brush_profile(preset_name: str, *, is_eraser: bool = False) -> str:
    if is_eraser:
        return "eraser"
    normalized = preset_name.strip().lower()
    # 候補名の完全一致を、部分一致より先に全プロファイル横断で評価する。
    # そうしないと "Airbrush Soft" が一般語 "brush" に誤分類される。
    for key, definition in BRUSH_PROFILES.items():
        if key == "auto":
            continue
        if any(candidate.lower() == normalized for candidate in definition.candidates):
            return key
    keyword_matches: list[tuple[int, str]] = []
    for key, definition in BRUSH_PROFILES.items():
        if key == "auto":
            continue
        if any(keyword in normalized for keyword in definition.keywords):
            longest = max(len(keyword) for keyword in definition.keywords if keyword in normalized)
            keyword_matches.append((longest, key))
    if keyword_matches:
        return max(keyword_matches)[1]
    return "auto"
