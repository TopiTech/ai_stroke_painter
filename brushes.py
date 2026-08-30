"""描画エンジン間で共有する意味的ブラシプロファイル定義。"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class BrushProfileDefinition:
    key: str
    preset_hint: str
    candidates: tuple[str, ...]
    keywords: tuple[str, ...]


BRUSH_POLICY_VERSION = 1


@dataclass(frozen=True)
class BrushPolicy:
    """ユーザー指定の画材を、描画上の役割に適したブラシへ展開する。"""

    requested_profile: str

    def profile_for(
        self,
        layer_name: str,
        *,
        default_profile: str = "auto",
        operation_kind: str | None = None,
    ) -> str:
        role = brush_role_for_layer(layer_name, operation_kind=operation_kind)
        requested = canonical_brush_profile(self.requested_profile)
        fallback = canonical_brush_profile(default_profile)
        if fallback == "auto":
            fallback = _ROLE_DEFAULT_PROFILES.get(role, "gpen")
        if requested == "auto":
            if role == "draft":
                return "pencil"
            if role == "flats" and fallback in {"gpen", "marupen", "pencil", "screentone", "splatter"}:
                return "marker"
            if role == "shading" and fallback in {"gpen", "marupen", "marker", "screentone", "splatter"}:
                return "airbrush"
            if role == "lineart" and fallback in {"airbrush", "marker", "watercolor", "glaze", "splatter"}:
                return "gpen"
            if role == "highlights" and fallback not in {"gpen", "marupen", "brush"}:
                return "gpen"
            return fallback

        # 線用のペンで大面積を塗ったり、エアブラシで主線を引いたりしない。
        if role == "draft":
            return "pencil"
        if role == "flats":
            if requested == "brush":
                return "watercolor"
            if requested in {"gpen", "marupen", "pencil", "screentone", "splatter"}:
                return (
                    fallback
                    if fallback in {"marker", "airbrush", "watercolor", "brush", "glaze", "oil_paint", "pastel"}
                    else "marker"
                )
            if requested in {"brush", "watercolor", "glaze", "oil_paint", "pastel", "marker", "airbrush"}:
                return requested
            return "marker"
        if role == "shading":
            if requested == "brush":
                return "watercolor"
            if requested in {"brush", "watercolor", "glaze", "oil_paint", "pastel", "pencil", "airbrush"}:
                return requested
            return "airbrush"
        if role == "lineart":
            if requested in {"gpen", "marupen", "brush", "pencil", "pastel"}:
                return requested
            return "gpen"
        if role == "highlights":
            if requested in {"gpen", "marupen"}:
                return requested
            return "gpen"
        if role == "fx":
            return requested
        return fallback

    def as_dict(self) -> dict[str, object]:
        role_layers = (
            ("draft", "Draft"),
            ("flats", "Flats"),
            ("shading", "Shading"),
            ("lineart", "Lineart"),
            ("highlights", "Highlights"),
            ("fx", "FX"),
        )
        return {
            "version": BRUSH_POLICY_VERSION,
            "mode": "role_aware",
            "requested_profile": canonical_brush_profile(self.requested_profile),
            "roles": {role: self.profile_for(layer) for role, layer in role_layers},
            "compatible_roles": {
                role: sorted(
                    {
                        self.profile_for(layer, default_profile=default)
                        for default in (
                            "marker",
                            "airbrush",
                            "watercolor",
                            "gpen",
                            "marupen",
                            "pencil",
                            "brush",
                            "oil_paint",
                            "pastel",
                            "screentone",
                            "glaze",
                            "splatter",
                        )
                    }
                )
                for role, layer in role_layers
            },
        }


_ROLE_DEFAULT_PROFILES: dict[str, str] = {
    "draft": "pencil",
    "flats": "marker",
    "shading": "airbrush",
    "lineart": "gpen",
    "highlights": "gpen",
    "fx": "gpen",
}


def brush_role_for_layer(layer_name: str, *, operation_kind: str | None = None) -> str:
    normalized = str(layer_name).strip().casefold()
    if "draft" in normalized or "sketch" in normalized:
        return "draft"
    if "flat" in normalized or "base" in normalized or operation_kind in {"fill", "gradient_fill"}:
        return "flats"
    if "shad" in normalized or "shadow" in normalized or "hatch" in normalized:
        return "shading"
    if "highlight" in normalized or "light" in normalized:
        return "highlights"
    if normalized == "fx" or "effect" in normalized or operation_kind == "particles":
        return "fx"
    return "lineart"


def brush_policy_for_profile(profile: str | None) -> BrushPolicy:
    return BrushPolicy(canonical_brush_profile(profile))


def role_brush_profile(
    profile: str | None,
    layer_name: str,
    *,
    default_profile: str = "auto",
    operation_kind: str | None = None,
) -> str:
    return brush_policy_for_profile(profile).profile_for(
        layer_name,
        default_profile=default_profile,
        operation_kind=operation_kind,
    )


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
        "Dry Bristles",
        ("Dry Bristles", "Wet-1 Water"),
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
    "oil_paint": BrushProfileDefinition(
        "oil_paint",
        "Dry Bristles",
        ("Dry Bristles", "Wet-1 Water", "Impasto"),
        ("oil", "impasto", "thick paint", "oil_paint"),
    ),
    "pastel": BrushProfileDefinition(
        "pastel",
        "Pencil-4 Soft",
        ("Pencil-4 Soft", "Charcoal Pencil", "Chalk"),
        ("pastel", "chalk", "crayon", "chalk_pastel"),
    ),
    "screentone": BrushProfileDefinition(
        "screentone",
        "Airbrush Static",
        ("Airbrush Static", "Pattern Screen", "Halftone"),
        ("screentone", "halftone", "tone", "dot"),
    ),
    "glaze": BrushProfileDefinition(
        "glaze",
        "Wet Textured Soft",
        ("Wet Textured Soft", "Wet-1 Water", "Glaze"),
        ("glaze", "glazing", "wash_glaze"),
    ),
    "splatter": BrushProfileDefinition(
        "splatter",
        "Airbrush Static",
        ("Airbrush Static", "Splatter", "Spray"),
        ("splatter", "spatter", "fleck", "sparkles"),
    ),
}


_PROFILE_ALIASES: dict[str, str] = {
    "pen": "gpen",
    "ink": "gpen",
    "g-pen": "gpen",
    "fineliner": "gpen",
    "dip pen": "gpen",
    "fude": "brush",
    "calligraphy": "brush",
    "paint": "brush",
    "oil": "oil_paint",
    "oil paint": "oil_paint",
    "oil_paint": "oil_paint",
    "impasto": "oil_paint",
    "acrylic": "brush",
    "bristle": "brush",
    "bristles": "brush",
    "dry bristles": "brush",
    "dry bristle": "brush",
    "dry": "brush",
    "felt": "marker",
    "broad": "marker",
    "water": "watercolor",
    "wet": "watercolor",
    "wash": "watercolor",
    "glaze": "glaze",
    "charcoal": "pencil",
    "graphite": "pencil",
    "chalk": "pastel",
    "pastel": "pastel",
    "crayon": "pastel",
    "screentone": "screentone",
    "halftone": "screentone",
    "tone": "screentone",
    "splatter": "splatter",
    "spray": "airbrush",
    "soft": "airbrush",
    "gradient": "airbrush",
    "glow": "airbrush",
    "blur": "airbrush",
    "dry brush": "brush",
    "ink brush": "brush",
    "flat brush": "brush",
    "round brush": "brush",
    "filbert": "brush",
    "mop": "brush",
    "gouache": "brush",
    "tempera": "brush",
    "sumi": "brush",
    "liner": "gpen",
    "lineart": "gpen",
    "contour": "gpen",
    "sketch": "pencil",
    "shading": "pencil",
    "highlighter": "marker",
    "chisel": "marker",
    "blend": "airbrush",
    "smudge": "airbrush",
    "fog": "airbrush",
    "mist": "airbrush",
}


def canonical_brush_profile(profile: str | None) -> str:
    if not profile:
        return "auto"
    key = profile.strip().lower()
    if key in BRUSH_PROFILES:
        return key
    if key in _PROFILE_ALIASES:
        return _PROFILE_ALIASES[key]
    return "auto"


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
