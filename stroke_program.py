"""高水準の描画命令を検証し、既存 DrawingPlan へ決定論的にコンパイルする。"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field, replace
import hashlib
import math
import random
import re
from typing import Any, Literal, TypeAlias
import uuid

from .brushes import brush_preset_for_profile, canonical_brush_profile, infer_brush_profile
from .domain import MAX_PLAN_STROKES, MAX_STROKE_POINTS, DrawingPlan, PlanValidationError, Stroke, StrokePoint

PROGRAM_SCHEMA_VERSION = 2
MAX_PROGRAM_OPERATIONS = MAX_PLAN_STROKES
MAX_OPERATION_POINTS = 1_000
_COLOR_RE = re.compile(r"^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{4}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$")


_NAMED_COLORS: dict[str, str] = {
    "black": "#000000",
    "white": "#ffffff",
    "red": "#ff0000",
    "green": "#00ff00",
    "blue": "#0000ff",
    "yellow": "#ffff00",
    "cyan": "#00ffff",
    "magenta": "#ff00ff",
    "gray": "#808080",
    "grey": "#808080",
    "brown": "#8b4513",
    "orange": "#ffa500",
    "pink": "#ffc0cb",
    "purple": "#800080",
    "violet": "#ee82ee",
    "beige": "#f5f5dc",
    "sky": "#87ceeb",
    "navy": "#000080",
    "gold": "#ffd700",
    "silver": "#c0c0c0",
    "teal": "#008080",
    "olive": "#808000",
    "maroon": "#800000",
    "lime": "#00ff00",
    "aqua": "#00ffff",
    "fuchsia": "#ff00ff",
    "crimson": "#dc143c",
    "coral": "#ff7f50",
    "indigo": "#4b0082",
    "khaki": "#f0e68c",
    "lavender": "#e6e6fa",
    "peach": "#ffdab9",
    "plum": "#dda0dd",
    "salmon": "#fa8072",
    "tan": "#d2b48c",
    "turquoise": "#40e0d0",
    "ivory": "#fffff0",
    "snow": "#fffafa",
    "charcoal": "#36454f",
    "cream": "#fffdd0",
    "transparent": "#00000000",
    "none": "#00000000",
}


def _hsl_to_rgb(h_deg: float, s_pct: float, l_pct: float) -> tuple[int, int, int]:
    """HSL (h: 0-360, s: 0-100%, l: 0-100%) を RGB (0-255) に変換する。"""
    import colorsys

    h = (h_deg % 360.0) / 360.0
    s = max(0.0, min(100.0, s_pct)) / 100.0
    lum = max(0.0, min(100.0, l_pct)) / 100.0
    r, g, b = colorsys.hls_to_rgb(h, lum, s)
    return round(r * 255), round(g * 255), round(b * 255)


def normalize_hex_color(color_val: Any, fallback: str = "#232323") -> str:
    """様々な色表現（#RGB, #RRGGBB, #RRGGBBAA, #なしHEX, 名前付き色, rgb(), rgba(), hsl(), hsla() 等）を安全に標準16進カラーコードに変換する。"""
    if not isinstance(color_val, str):
        return fallback
    c = color_val.strip()
    if not c:
        return fallback
    if _COLOR_RE.match(c):
        return c
    # '#' 抜けの 3, 4, 6, 8 桁 hex
    if re.match(r"^[0-9a-fA-F]{3,8}$", c):
        cand = f"#{c}"
        if _COLOR_RE.match(cand):
            return cand
    # 名前付きカラー
    low = c.lower()
    if low in _NAMED_COLORS:
        return _NAMED_COLORS[low]
    # rgb(r, g, b) または rgba(r, g, b, a)
    rgb_m = re.match(
        r"rgba?\s*\(\s*(\d+(?:\.\d+)?%?)\s*,\s*(\d+(?:\.\d+)?%?)\s*,\s*(\d+(?:\.\d+)?%?)(?:\s*,\s*(\d+(?:\.\d+)?%?))?\s*\)",
        c,
        re.I,
    )
    if rgb_m:

        def _parse_rgb_part(v: str) -> int:
            if v.endswith("%"):
                return max(0, min(255, round(float(v[:-1]) * 2.55)))
            f_val = float(v)
            if f_val <= 1.0 and "." in v:
                return max(0, min(255, round(f_val * 255)))
            return max(0, min(255, round(f_val)))

        r = _parse_rgb_part(rgb_m.group(1))
        g = _parse_rgb_part(rgb_m.group(2))
        b = _parse_rgb_part(rgb_m.group(3))
        a_str = rgb_m.group(4)
        if a_str is not None:
            if a_str.endswith("%"):
                a = max(0, min(255, round(float(a_str[:-1]) * 2.55)))
            else:
                f_a = float(a_str)
                a = max(0, min(255, round(f_a * 255 if f_a <= 1.0 else f_a)))
            return f"#{r:02x}{g:02x}{b:02x}{a:02x}"
        return f"#{r:02x}{g:02x}{b:02x}"

    # hsl(h, s%, l%) または hsla(h, s%, l%, a)
    hsl_m = re.match(
        r"hsla?\s*\(\s*(\d+(?:\.\d+)?(?:deg)?)\s*,\s*(\d+(?:\.\d+)?)%?\s*,\s*(\d+(?:\.\d+)?)%?(?:\s*,\s*(\d+(?:\.\d+)?%?))?\s*\)",
        c,
        re.I,
    )
    if hsl_m:
        h_str = hsl_m.group(1).lower().replace("deg", "")
        h = float(h_str)
        s = float(hsl_m.group(2))
        lum = float(hsl_m.group(3))
        r, g, b = _hsl_to_rgb(h, s, lum)
        a_str = hsl_m.group(4)
        if a_str is not None:
            if a_str.endswith("%"):
                a = max(0, min(255, round(float(a_str[:-1]) * 2.55)))
            else:
                f_a = float(a_str)
                a = max(0, min(255, round(f_a * 255 if f_a <= 1.0 else f_a)))
            return f"#{r:02x}{g:02x}{b:02x}{a:02x}"
        return f"#{r:02x}{g:02x}{b:02x}"

    return fallback


def _catmull_rom_spline(
    control_points: list[tuple[float, float]], samples_per_segment: int
) -> list[tuple[float, float]]:
    # 遅延importで StrokeProgram -> procedural package の循環初期化を避ける。
    from .procedural.base import catmull_rom_spline

    return catmull_rom_spline(control_points, samples_per_segment)


def _sample_strokes_by_priority(strokes: list[Stroke], count: int) -> list[Stroke]:
    from .procedural.base import sample_strokes_by_priority

    return sample_strokes_by_priority(strokes, count)


def _finite(value: Any, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise PlanValidationError(f"{name} は有限数値である必要があります")
    return float(value)


def _coordinate_canvas_dimension(value: Any) -> float:
    if isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value) and value > 1.0:
        return float(value)
    return 1000.0


def _unit(value: Any, name: str) -> float:
    result = _finite(value, name)
    # 0.0〜1.0 に自動クランプして微小なはみ出しや丸め誤差を許容
    return max(0.0, min(1.0, result))


def _reject_unknown_keys(value: Mapping[Any, Any], allowed: set[str], name: str) -> None:
    unknown = sorted(str(key) for key in value if key not in allowed)
    if unknown:
        raise PlanValidationError(f"{name} に未対応のフィールドがあります: {', '.join(unknown)}")


@dataclass(frozen=True)
class ProgramPoint:
    """キャンバス比率で表した制御点。"""

    x: float
    y: float
    pressure: float = 0.8

    def __post_init__(self) -> None:
        object.__setattr__(self, "x", _unit(self.x, "point.x"))
        object.__setattr__(self, "y", _unit(self.y, "point.y"))
        object.__setattr__(self, "pressure", _unit(self.pressure, "point.pressure"))

    @classmethod
    def from_value(cls, value: Any, canvas_w: float = 1000.0, canvas_h: float = 1000.0) -> ProgramPoint:
        if isinstance(value, Sequence) and not isinstance(value, (str, bytes)):
            if len(value) < 2:
                raise PlanValidationError("program point 配列には x, y が必要です")
            raw_x = float(value[0])
            raw_y = float(value[1])
            # ピクセル座標（> 1.0）が渡された場合の自動比率正規化
            if raw_x > 1.0 and canvas_w > 1.0:
                raw_x = raw_x / canvas_w
            if raw_y > 1.0 and canvas_h > 1.0:
                raw_y = raw_y / canvas_h
            pressure = float(value[2]) if len(value) >= 3 else 0.8
            return cls(raw_x, raw_y, pressure)
        if isinstance(value, Mapping):
            try:
                raw_x = float(value["x"])
                raw_y = float(value["y"])
                if raw_x > 1.0 and canvas_w > 1.0:
                    raw_x = raw_x / canvas_w
                if raw_y > 1.0 and canvas_h > 1.0:
                    raw_y = raw_y / canvas_h
                pressure = float(value.get("pressure", 0.8))
                return cls(raw_x, raw_y, pressure)
            except (KeyError, TypeError, ValueError) as exc:
                raise PlanValidationError(f"program point の値が不正です: {exc}") from exc
        raise PlanValidationError("program point は配列またはオブジェクトである必要があります")

    def as_list(self) -> list[float]:
        return [self.x, self.y, self.pressure]


@dataclass(frozen=True)
class ProgramBrush:
    profile: str = "auto"
    preset_hint: str | None = None
    color: str = "#232323"
    size: float = 0.006
    size_mode: Literal["ratio", "px"] = "ratio"
    opacity: float = 1.0
    is_eraser: bool = False

    def __post_init__(self) -> None:
        if not isinstance(self.profile, str) or not self.profile.strip():
            raise PlanValidationError("program brush profile は文字列である必要があります")
        canonical_profile = canonical_brush_profile(self.profile)
        if canonical_profile == "auto" and self.profile.strip().lower() != "auto":
            raise PlanValidationError(f"未対応の program brush profile: {self.profile}")
        object.__setattr__(self, "profile", canonical_profile)
        if self.preset_hint is not None and (not isinstance(self.preset_hint, str) or not self.preset_hint.strip()):
            raise PlanValidationError("program brush preset_hint は空でない文字列または null である必要があります")
        if not isinstance(self.color, str) or not _COLOR_RE.match(self.color):
            raise PlanValidationError("program brush color は16進カラーである必要があります")
        size = _finite(self.size, "program brush size")
        if size <= 0:
            raise PlanValidationError("program brush size は正数である必要があります")
        object.__setattr__(self, "size", size)
        if self.size_mode not in {"ratio", "px"}:
            raise PlanValidationError("program brush size_mode は ratio または px である必要があります")
        object.__setattr__(self, "opacity", _unit(self.opacity, "program brush opacity"))
        if not isinstance(self.is_eraser, bool):
            raise PlanValidationError("program brush is_eraser は真偽値である必要があります")

    @classmethod
    def from_dict(cls, value: Any) -> ProgramBrush:
        if value is None:
            return cls()
        if not isinstance(value, Mapping):
            raise PlanValidationError("brush はオブジェクトである必要があります")
        _reject_unknown_keys(
            value,
            {"profile", "preset_hint", "color", "size", "size_ratio", "size_mode", "opacity", "is_eraser"},
            "brush",
        )
        raw_is_eraser = value.get("is_eraser")
        profile_str = str(value.get("profile", "auto")).strip().lower()
        preset_str = str(value.get("preset_hint", "") or "").strip().lower()
        if raw_is_eraser is None:
            is_eraser = profile_str == "eraser" or "eraser" in preset_str
        elif isinstance(raw_is_eraser, bool):
            is_eraser = raw_is_eraser
        else:
            raise PlanValidationError("brush is_eraser は真偽値である必要があります")
        return cls(
            profile=value.get("profile", "auto"),
            preset_hint=value.get("preset_hint"),
            color=normalize_hex_color(value.get("color", "#232323"), fallback="#232323"),
            size=value.get("size", value.get("size_ratio", 0.006)),
            size_mode=value.get("size_mode", "ratio"),
            opacity=value.get("opacity", 1.0),
            is_eraser=is_eraser,
        )

    def size_px(self, width: float, height: float) -> float:
        return self.size if self.size_mode == "px" else max(0.5, self.size * min(width, height))

    def as_dict(self) -> dict[str, Any]:
        result: dict[str, Any] = {
            "profile": self.profile,
            "color": self.color,
            "size": self.size,
            "size_mode": self.size_mode,
            "opacity": self.opacity,
            "is_eraser": self.is_eraser,
        }
        if self.preset_hint is not None:
            result["preset_hint"] = self.preset_hint
        return result


@dataclass(frozen=True)
class PathOperation:
    id: str
    points: Sequence[ProgramPoint]
    brush: ProgramBrush = field(default_factory=ProgramBrush)
    layer: str = "Lineart"
    closed: bool = False
    smooth: bool = True
    role: str = "auto"
    kind: Literal["path"] = "path"

    def __post_init__(self) -> None:
        _validate_operation_common(self.id, self.layer, self.brush)
        points = tuple(self.points)
        if not 2 <= len(points) <= MAX_OPERATION_POINTS:
            raise PlanValidationError(f"path points は 2 から {MAX_OPERATION_POINTS} 点必要です")
        if any(not isinstance(point, ProgramPoint) for point in points):
            raise PlanValidationError("path points は ProgramPoint である必要があります")
        object.__setattr__(self, "points", points)
        if not isinstance(self.closed, bool) or not isinstance(self.smooth, bool):
            raise PlanValidationError("path closed/smooth は真偽値である必要があります")
        if not isinstance(self.role, str):
            raise PlanValidationError("path role は文字列である必要があります")
        valid_roles = {"auto", "outline", "detail", "accent", "crevice", "hair", "eye", "hatch"}
        r_low = self.role.strip().lower()
        object.__setattr__(self, "role", r_low if r_low in valid_roles else "auto")


@dataclass(frozen=True)
class FillOperation:
    id: str
    polygon: Sequence[ProgramPoint]
    brush: ProgramBrush = field(default_factory=lambda: ProgramBrush(profile="marker", size=0.035))
    layer: str = "Flats"
    spacing: float = 0.72
    style: str = "wash"
    angle_deg: float = 0.0
    kind: Literal["fill"] = "fill"

    def __post_init__(self) -> None:
        _validate_operation_common(self.id, self.layer, self.brush)
        polygon = tuple(self.polygon)
        if not 3 <= len(polygon) <= MAX_OPERATION_POINTS:
            raise PlanValidationError(f"fill polygon は 3 から {MAX_OPERATION_POINTS} 点必要です")
        if any(not isinstance(point, ProgramPoint) for point in polygon):
            raise PlanValidationError("fill polygon は ProgramPoint である必要があります")
        object.__setattr__(self, "polygon", polygon)
        spacing = _finite(self.spacing, "fill spacing")
        if not 0.2 <= spacing <= 1.0:
            raise PlanValidationError("fill spacing は 0.2 から 1.0 の範囲である必要があります")
        object.__setattr__(self, "spacing", spacing)
        if not isinstance(self.style, str) or self.style.strip().lower() not in {
            "wash",
            "scanline",
            "feathered",
            "contour",
            "radial",
            "directional",
        }:
            raise PlanValidationError(
                "fill style は wash, scanline, feathered, contour, radial, directional のいずれかである必要があります"
            )
        object.__setattr__(self, "style", self.style.strip().lower())
        object.__setattr__(self, "angle_deg", _finite(self.angle_deg, "fill angle_deg") % 360.0)


@dataclass(frozen=True)
class GradientFillOperation:
    """多色・線形/放射グラデーションで面を滑らかに満たす高品位フィルオペレーション。"""

    id: str
    polygon: Sequence[ProgramPoint]
    colors: Sequence[str] = ()
    brush: ProgramBrush = field(default_factory=lambda: ProgramBrush(profile="watercolor", size=0.04))
    layer: str = "Flats"
    spacing: float = 0.65
    style: str = "linear"
    angle_deg: float = 0.0
    kind: Literal["gradient_fill"] = "gradient_fill"

    def __post_init__(self) -> None:
        _validate_operation_common(self.id, self.layer, self.brush)
        polygon = tuple(self.polygon)
        if not 3 <= len(polygon) <= MAX_OPERATION_POINTS:
            raise PlanValidationError(f"gradient_fill polygon は 3 から {MAX_OPERATION_POINTS} 点必要です")
        if any(not isinstance(point, ProgramPoint) for point in polygon):
            raise PlanValidationError("gradient_fill polygon は ProgramPoint である必要があります")
        object.__setattr__(self, "polygon", polygon)
        object.__setattr__(
            self,
            "colors",
            tuple(normalize_hex_color(c) for c in self.colors) if self.colors else (self.brush.color, "#ffffff"),
        )
        spacing = _finite(self.spacing, "gradient_fill spacing")
        if not 0.1 <= spacing <= 1.5:
            raise PlanValidationError("gradient_fill spacing は 0.1 から 1.5 の範囲である必要があります")
        object.__setattr__(self, "spacing", spacing)
        st_low = str(self.style).strip().lower()
        if st_low not in {"linear", "radial", "contour", "directional", "wash"}:
            st_low = "linear"
        object.__setattr__(self, "style", st_low)
        object.__setattr__(self, "angle_deg", _finite(self.angle_deg, "gradient_fill angle_deg") % 360.0)


@dataclass(frozen=True)
class RibbonOperation:
    """髪の毛束・リボン・布のドレープ等、幅が連続変化する立体ストロークオペレーション。"""

    id: str
    spine: Sequence[ProgramPoint]
    brush: ProgramBrush = field(default_factory=lambda: ProgramBrush(profile="brush", size=0.015))
    layer: str = "Lineart"
    width_start: float = 0.008
    width_mid: float = 0.025
    width_end: float = 0.003
    taper_profile: str = "taper_both"
    smooth: bool = True
    kind: Literal["ribbon"] = "ribbon"

    def __post_init__(self) -> None:
        _validate_operation_common(self.id, self.layer, self.brush)
        spine = tuple(self.spine)
        if not 2 <= len(spine) <= MAX_OPERATION_POINTS:
            raise PlanValidationError(f"ribbon spine は 2 から {MAX_OPERATION_POINTS} 点必要です")
        if any(not isinstance(point, ProgramPoint) for point in spine):
            raise PlanValidationError("ribbon spine は ProgramPoint である必要があります")
        object.__setattr__(self, "spine", spine)
        object.__setattr__(self, "width_start", max(0.0005, min(0.5, _finite(self.width_start, "ribbon width_start"))))
        object.__setattr__(self, "width_mid", max(0.0005, min(0.5, _finite(self.width_mid, "ribbon width_mid"))))
        object.__setattr__(self, "width_end", max(0.0005, min(0.5, _finite(self.width_end, "ribbon width_end"))))
        tp_low = str(self.taper_profile).strip().lower()
        if tp_low not in {"taper_both", "taper_start", "taper_end", "uniform"}:
            tp_low = "taper_both"
        object.__setattr__(self, "taper_profile", tp_low)
        if not isinstance(self.smooth, bool):
            raise PlanValidationError("ribbon smooth は真偽値である必要があります")


@dataclass(frozen=True)
class HatchOperation:
    id: str
    polygon: Sequence[ProgramPoint]
    brush: ProgramBrush = field(default_factory=lambda: ProgramBrush(profile="pencil", size=0.0025))
    layer: str = "Shading"
    angle_deg: float = 30.0
    spacing: float = 0.012
    cross: bool = False
    kind: Literal["hatch"] = "hatch"

    def __post_init__(self) -> None:
        _validate_operation_common(self.id, self.layer, self.brush)
        polygon = tuple(self.polygon)
        if not 3 <= len(polygon) <= MAX_OPERATION_POINTS:
            raise PlanValidationError(f"hatch polygon は 3 から {MAX_OPERATION_POINTS} 点必要です")
        if any(not isinstance(point, ProgramPoint) for point in polygon):
            raise PlanValidationError("hatch polygon は ProgramPoint である必要があります")
        object.__setattr__(self, "polygon", polygon)
        object.__setattr__(self, "angle_deg", _finite(self.angle_deg, "hatch angle_deg") % 180.0)
        spacing = _finite(self.spacing, "hatch spacing")
        if not 0.001 <= spacing <= 0.5:
            raise PlanValidationError("hatch spacing は 0.001 から 0.5 の範囲である必要があります")
        object.__setattr__(self, "spacing", spacing)
        if not isinstance(self.cross, bool):
            raise PlanValidationError("hatch cross は真偽値である必要があります")


@dataclass(frozen=True)
class ParticleOperation:
    id: str
    bounds: tuple[float, float, float, float]
    count: int = 20
    brush: ProgramBrush = field(default_factory=lambda: ProgramBrush(size=0.003))
    layer: str = "FX"
    length: float = 0.015
    angle_deg: float = 90.0
    angle_jitter: float = 35.0
    shape: str = "petal"
    kind: Literal["particles"] = "particles"

    def __post_init__(self) -> None:
        _validate_operation_common(self.id, self.layer, self.brush)
        if len(self.bounds) != 4:
            raise PlanValidationError("particle bounds は x0,y0,x1,y1 の4要素である必要があります")
        x0, y0, x1, y1 = (_unit(value, "particle bounds") for value in self.bounds)
        if x1 <= x0 or y1 <= y0:
            raise PlanValidationError("particle bounds の終点は始点より大きい必要があります")
        object.__setattr__(self, "bounds", (x0, y0, x1, y1))
        if isinstance(self.count, bool) or not isinstance(self.count, int) or not 1 <= self.count <= 500:
            raise PlanValidationError("particle count は 1 から 500 の整数である必要があります")
        length = _finite(self.length, "particle length")
        if not 0.0005 <= length <= 0.5:
            raise PlanValidationError("particle length は 0.0005 から 0.5 の範囲である必要があります")
        object.__setattr__(self, "length", length)
        object.__setattr__(self, "angle_deg", _finite(self.angle_deg, "particle angle_deg"))
        jitter = _finite(self.angle_jitter, "particle angle_jitter")
        if not 0.0 <= jitter <= 180.0:
            raise PlanValidationError("particle angle_jitter は 0 から 180 の範囲である必要があります")
        object.__setattr__(self, "angle_jitter", jitter)
        if not isinstance(self.shape, str) or self.shape.strip().lower() not in {
            "petal",
            "line",
            "sparkle",
            "drift",
            "bokeh",
            "splatter",
            "star",
        }:
            raise PlanValidationError(
                "particle shape は petal, line, sparkle, drift, bokeh, splatter, star のいずれかである必要があります"
            )
        object.__setattr__(self, "shape", self.shape.strip().lower())


@dataclass(frozen=True)
class MacroOperation:
    """花房・樹木分岐・山並み・水彩ウォッシュ等のプロシージャル高水準アート・プリミティブ。"""

    id: str
    name: str
    brush: ProgramBrush = field(default_factory=lambda: ProgramBrush(profile="brush"))
    layer: str = "Flats"
    center: tuple[float, float] = (0.5, 0.5)
    radius: float = 0.2
    bounds: tuple[float, float, float, float] = (0.0, 0.0, 1.0, 1.0)
    polygon: Sequence[ProgramPoint] = ()
    colors: Sequence[str] = ()
    params: Mapping[str, Any] = field(default_factory=dict)
    kind: Literal["macro"] = "macro"

    def __post_init__(self) -> None:
        _validate_operation_common(self.id, self.layer, self.brush)
        if not isinstance(self.name, str) or not self.name.strip():
            raise PlanValidationError("macro name は空でない文字列である必要があります")
        object.__setattr__(self, "name", self.name.strip().lower())
        if len(self.center) != 2:
            raise PlanValidationError("macro center は x,y の2要素である必要があります")
        cx, cy = (_unit(v, "macro center") for v in self.center)
        object.__setattr__(self, "center", (cx, cy))
        radius = _finite(self.radius, "macro radius")
        if not 0.001 <= radius <= 2.0:
            raise PlanValidationError("macro radius は 0.001 から 2.0 の範囲である必要があります")
        object.__setattr__(self, "radius", radius)
        if len(self.bounds) != 4:
            raise PlanValidationError("macro bounds は x0,y0,x1,y1 の4要素である必要があります")
        x0, y0, x1, y1 = (_unit(v, "macro bounds") for v in self.bounds)
        if x1 <= x0 or y1 <= y0:
            raise PlanValidationError("macro bounds の終点は始点より大きい必要があります")
        object.__setattr__(self, "bounds", (x0, y0, x1, y1))
        object.__setattr__(self, "polygon", tuple(self.polygon))
        object.__setattr__(self, "colors", tuple(normalize_hex_color(c) for c in self.colors))
        if not isinstance(self.params, Mapping):
            raise PlanValidationError("macro params はオブジェクトである必要があります")
        object.__setattr__(self, "params", dict(self.params))


ProgramOperation: TypeAlias = (
    PathOperation
    | FillOperation
    | GradientFillOperation
    | RibbonOperation
    | HatchOperation
    | ParticleOperation
    | MacroOperation
)


def _validate_operation_common(operation_id: str, layer: str, brush: ProgramBrush) -> None:
    if not isinstance(operation_id, str) or not operation_id.strip():
        raise PlanValidationError("operation id は空でない文字列である必要があります")
    if not isinstance(layer, str) or not layer.strip():
        raise PlanValidationError("operation layer は空でない文字列である必要があります")
    if not isinstance(brush, ProgramBrush):
        raise PlanValidationError("operation brush は ProgramBrush である必要があります")


def _points_from(
    value: Any,
    name: str,
    *,
    canvas_w: float = 1000.0,
    canvas_h: float = 1000.0,
) -> tuple[ProgramPoint, ...]:
    if not isinstance(value, Sequence) or isinstance(value, (str, bytes)):
        raise PlanValidationError(f"{name} は点配列である必要があります")
    if len(value) > MAX_OPERATION_POINTS:
        raise PlanValidationError(f"{name} は {MAX_OPERATION_POINTS} 点以下である必要があります")
    return tuple(ProgramPoint.from_value(point, canvas_w=canvas_w, canvas_h=canvas_h) for point in value)


def operation_from_dict(
    value: Any,
    *,
    canvas_w: float = 1000.0,
    canvas_h: float = 1000.0,
) -> ProgramOperation:
    if not isinstance(value, Mapping):
        raise PlanValidationError("operation はオブジェクトである必要があります")
    kind = str(value.get("kind", "path")).strip().lower()
    operation_id = value.get("id", "")
    if kind == "path":
        _reject_unknown_keys(
            value,
            {"kind", "id", "points", "brush", "layer", "layer_name", "closed", "smooth", "role"},
            "path operation",
        )
        return PathOperation(
            id=operation_id,
            points=_points_from(value.get("points", ()), "path points", canvas_w=canvas_w, canvas_h=canvas_h),
            brush=ProgramBrush.from_dict(value.get("brush")),
            layer=value.get("layer", value.get("layer_name", "Lineart")),
            closed=value.get("closed", False),
            smooth=value.get("smooth", True),
            role=value.get("role", "auto"),
        )
    if kind == "fill":
        _reject_unknown_keys(
            value,
            {
                "kind",
                "id",
                "polygon",
                "points",
                "brush",
                "layer",
                "layer_name",
                "spacing",
                "style",
                "angle_deg",
                "angle",
            },
            "fill operation",
        )
        return FillOperation(
            id=operation_id,
            polygon=_points_from(
                value.get("polygon", value.get("points", ())),
                "fill polygon",
                canvas_w=canvas_w,
                canvas_h=canvas_h,
            ),
            brush=(
                ProgramBrush.from_dict(value["brush"])
                if "brush" in value
                else ProgramBrush(profile="marker", size=0.035)
            ),
            layer=value.get("layer", value.get("layer_name", "Flats")),
            spacing=value.get("spacing", 0.72),
            style=value.get("style", "wash"),
            angle_deg=value.get("angle_deg", value.get("angle", 0.0)),
        )
    if kind == "gradient_fill":
        _reject_unknown_keys(
            value,
            {
                "kind",
                "id",
                "polygon",
                "points",
                "colors",
                "brush",
                "layer",
                "layer_name",
                "spacing",
                "style",
                "angle_deg",
                "angle",
            },
            "gradient_fill operation",
        )
        raw_colors = value.get("colors", ())
        grad_colors_tuple = (
            tuple(normalize_hex_color(c) for c in raw_colors if isinstance(c, (str, int)))
            if isinstance(raw_colors, Sequence) and not isinstance(raw_colors, (str, bytes))
            else ()
        )
        return GradientFillOperation(
            id=operation_id,
            polygon=_points_from(
                value.get("polygon", value.get("points", ())),
                "gradient_fill polygon",
                canvas_w=canvas_w,
                canvas_h=canvas_h,
            ),
            colors=grad_colors_tuple,
            brush=(
                ProgramBrush.from_dict(value["brush"])
                if "brush" in value
                else ProgramBrush(profile="watercolor", size=0.04)
            ),
            layer=value.get("layer", value.get("layer_name", "Flats")),
            spacing=value.get("spacing", 0.65),
            style=value.get("style", "linear"),
            angle_deg=value.get("angle_deg", value.get("angle", 0.0)),
        )
    if kind == "ribbon":
        _reject_unknown_keys(
            value,
            {
                "kind",
                "id",
                "spine",
                "points",
                "brush",
                "layer",
                "layer_name",
                "width_start",
                "width_mid",
                "width_end",
                "taper_profile",
                "smooth",
            },
            "ribbon operation",
        )
        return RibbonOperation(
            id=operation_id,
            spine=_points_from(
                value.get("spine", value.get("points", ())),
                "ribbon spine",
                canvas_w=canvas_w,
                canvas_h=canvas_h,
            ),
            brush=(
                ProgramBrush.from_dict(value["brush"])
                if "brush" in value
                else ProgramBrush(profile="brush", size=0.015)
            ),
            layer=value.get("layer", value.get("layer_name", "Lineart")),
            width_start=value.get("width_start", 0.008),
            width_mid=value.get("width_mid", 0.025),
            width_end=value.get("width_end", 0.003),
            taper_profile=value.get("taper_profile", "taper_both"),
            smooth=value.get("smooth", True),
        )
    if kind == "hatch":
        _reject_unknown_keys(
            value,
            {
                "kind",
                "id",
                "polygon",
                "points",
                "brush",
                "layer",
                "layer_name",
                "angle_deg",
                "spacing",
                "cross",
            },
            "hatch operation",
        )
        return HatchOperation(
            id=operation_id,
            polygon=_points_from(
                value.get("polygon", value.get("points", ())),
                "hatch polygon",
                canvas_w=canvas_w,
                canvas_h=canvas_h,
            ),
            brush=(
                ProgramBrush.from_dict(value["brush"])
                if "brush" in value
                else ProgramBrush(profile="pencil", size=0.0025)
            ),
            layer=value.get("layer", value.get("layer_name", "Shading")),
            angle_deg=value.get("angle_deg", 30.0),
            spacing=value.get("spacing", 0.012),
            cross=value.get("cross", False),
        )
    if kind == "particles":
        _reject_unknown_keys(
            value,
            {
                "kind",
                "id",
                "bounds",
                "count",
                "brush",
                "layer",
                "layer_name",
                "length",
                "angle_deg",
                "angle_jitter",
                "shape",
            },
            "particles operation",
        )
        raw_bounds = value.get("bounds", (0.0, 0.0, 1.0, 1.0))
        if not isinstance(raw_bounds, Sequence) or isinstance(raw_bounds, (str, bytes)):
            raise PlanValidationError("particle bounds は配列である必要があります")
        return ParticleOperation(
            id=operation_id,
            bounds=tuple(raw_bounds),
            count=value.get("count", 20),
            brush=(ProgramBrush.from_dict(value["brush"]) if "brush" in value else ProgramBrush(size=0.003)),
            layer=value.get("layer", value.get("layer_name", "FX")),
            length=value.get("length", 0.015),
            angle_deg=value.get("angle_deg", 90.0),
            angle_jitter=value.get("angle_jitter", 35.0),
            shape=value.get("shape", "petal"),
        )
    if kind in (
        "macro",
        "flower_cluster",
        "sakura_canopy",
        "branch_tree",
        "mountain_range",
        "watercolor_wash",
        "rose_bloom",
        "wildflower",
        "cloud_cluster",
        "cumulus_clouds",
        "cloud",
        "character_face",
        "anime_face",
        "portrait",
        "magic_circle",
        "cyber_city",
    ):
        macro_name = str(value.get("name", kind if kind != "macro" else "flower_cluster")).strip().lower()
        raw_center = value.get("center", (0.5, 0.5))
        if isinstance(raw_center, Sequence) and not isinstance(raw_center, (str, bytes)) and len(raw_center) >= 2:
            try:
                cx_val = float(raw_center[0])
                cy_val = float(raw_center[1])
                if cx_val > 1.0 and canvas_w > 1.0:
                    cx_val /= canvas_w
                if cy_val > 1.0 and canvas_h > 1.0:
                    cy_val /= canvas_h
                center_tuple = (max(0.0, min(1.0, cx_val)), max(0.0, min(1.0, cy_val)))
            except (ValueError, TypeError):
                center_tuple = (0.5, 0.5)
        else:
            center_tuple = (0.5, 0.5)

        raw_radius = value.get("radius", value.get("r", 0.2))
        try:
            r_num = float(raw_radius)
            if r_num > 1.0 and min(canvas_w, canvas_h) > 1.0:
                r_num /= min(canvas_w, canvas_h)
            radius_val = max(0.001, min(2.0, r_num))
        except (ValueError, TypeError):
            radius_val = 0.2

        raw_bounds = value.get("bounds", (0.0, 0.0, 1.0, 1.0))
        if isinstance(raw_bounds, Sequence) and not isinstance(raw_bounds, (str, bytes)) and len(raw_bounds) >= 4:
            try:
                b_x0, b_y0, b_x1, b_y1 = (
                    float(raw_bounds[0]),
                    float(raw_bounds[1]),
                    float(raw_bounds[2]),
                    float(raw_bounds[3]),
                )
                if b_x0 > 1.0 and canvas_w > 1.0:
                    b_x0 /= canvas_w
                if b_y0 > 1.0 and canvas_h > 1.0:
                    b_y0 /= canvas_h
                if b_x1 > 1.0 and canvas_w > 1.0:
                    b_x1 /= canvas_w
                if b_y1 > 1.0 and canvas_h > 1.0:
                    b_y1 /= canvas_h
                b_x0, b_y0, b_x1, b_y1 = (
                    max(0.0, min(1.0, b_x0)),
                    max(0.0, min(1.0, b_y0)),
                    max(0.0, min(1.0, b_x1)),
                    max(0.0, min(1.0, b_y1)),
                )
                if b_x1 <= b_x0:
                    b_x1 = min(1.0, b_x0 + 0.1)
                if b_y1 <= b_y0:
                    b_y1 = min(1.0, b_y0 + 0.1)
                bounds_tuple = (b_x0, b_y0, b_x1, b_y1)
            except (ValueError, TypeError):
                bounds_tuple = (0.0, 0.0, 1.0, 1.0)
        else:
            bounds_tuple = (0.0, 0.0, 1.0, 1.0)

        raw_colors = value.get("colors", ())
        colors_tuple: tuple[str, ...] = (
            tuple(normalize_hex_color(c) for c in raw_colors if isinstance(c, (str, int)))
            if isinstance(raw_colors, Sequence) and not isinstance(raw_colors, (str, bytes))
            else ()
        )

        return MacroOperation(
            id=operation_id,
            name=macro_name,
            brush=(
                ProgramBrush.from_dict(value["brush"])
                if "brush" in value
                else ProgramBrush(profile="watercolor" if "wash" in macro_name else "brush")
            ),
            layer=value.get("layer", value.get("layer_name", "Flats")),
            center=center_tuple,
            radius=radius_val,
            bounds=bounds_tuple,
            polygon=(
                _points_from(value.get("polygon", ()), "macro polygon", canvas_w=canvas_w, canvas_h=canvas_h)
                if "polygon" in value
                else ()
            ),
            colors=colors_tuple,
            params=dict(value.get("params", {})) if isinstance(value.get("params"), Mapping) else {},
        )
    raise PlanValidationError(f"未対応の operation kind: {kind}")


@dataclass(frozen=True)
class StrokeProgram:
    prompt: str
    seed: int
    operations: Sequence[ProgramOperation]
    canvas_width: float
    canvas_height: float
    title: str = ""
    iteration: int = 1
    metadata: Mapping[str, Any] = field(default_factory=dict)
    goal_reached: bool = False
    completion_score: float = 0.0

    def __post_init__(self) -> None:
        if not isinstance(self.prompt, str):
            raise PlanValidationError("program prompt は文字列である必要があります")
        if isinstance(self.seed, bool) or not isinstance(self.seed, int) or self.seed < 0:
            raise PlanValidationError("program seed は0以上の整数である必要があります")
        operations = tuple(self.operations)
        if not 1 <= len(operations) <= MAX_PROGRAM_OPERATIONS:
            raise PlanValidationError(f"operations は1から{MAX_PROGRAM_OPERATIONS}件必要です")
        if any(
            not isinstance(
                operation,
                (
                    PathOperation,
                    FillOperation,
                    GradientFillOperation,
                    RibbonOperation,
                    HatchOperation,
                    ParticleOperation,
                    MacroOperation,
                ),
            )
            for operation in operations
        ):
            raise PlanValidationError("operations に未対応の値があります")
        if len({operation.id for operation in operations}) != len(operations):
            raise PlanValidationError("operation id は一意である必要があります")
        object.__setattr__(self, "operations", operations)
        for name in ("canvas_width", "canvas_height"):
            dimension = _finite(getattr(self, name), name)
            if dimension < 2:
                raise PlanValidationError(f"{name} は2以上である必要があります")
            object.__setattr__(self, name, dimension)
        if isinstance(self.iteration, bool) or not isinstance(self.iteration, int) or self.iteration < 1:
            raise PlanValidationError("program iteration は1以上の整数である必要があります")
        if not isinstance(self.metadata, Mapping):
            raise PlanValidationError("program metadata はオブジェクトである必要があります")
        object.__setattr__(self, "metadata", dict(self.metadata))
        if not isinstance(self.goal_reached, bool):
            raise PlanValidationError("program goal_reached は真偽値である必要があります")
        object.__setattr__(self, "completion_score", _unit(self.completion_score, "program completion_score"))

    @classmethod
    def from_dict(cls, value: Any) -> StrokeProgram:
        if not isinstance(value, Mapping):
            raise PlanValidationError("StrokeProgram はオブジェクトである必要があります")
        version = value.get("schema_version", PROGRAM_SCHEMA_VERSION)
        if version != PROGRAM_SCHEMA_VERSION:
            raise PlanValidationError(f"未対応の StrokeProgram schema_version: {version}")
        raw_operations = value.get("operations")
        if not isinstance(raw_operations, Sequence) or isinstance(raw_operations, (str, bytes)):
            raise PlanValidationError("operations は配列である必要があります")
        if len(raw_operations) > MAX_PROGRAM_OPERATIONS:
            raise PlanValidationError(f"operations は{MAX_PROGRAM_OPERATIONS}件以下である必要があります")
        canvas = value.get("canvas", {})
        canvas_mapping = canvas if isinstance(canvas, Mapping) else {}
        raw_canvas_width = value.get("canvas_width", canvas_mapping.get("width", 1000.0))
        raw_canvas_height = value.get("canvas_height", canvas_mapping.get("height", 1000.0))
        coordinate_canvas_width = _coordinate_canvas_dimension(raw_canvas_width)
        coordinate_canvas_height = _coordinate_canvas_dimension(raw_canvas_height)
        return cls(
            prompt=value.get("prompt", ""),
            seed=value.get("seed", 0),
            operations=tuple(
                operation_from_dict(
                    operation,
                    canvas_w=coordinate_canvas_width,
                    canvas_h=coordinate_canvas_height,
                )
                for operation in raw_operations
            ),
            canvas_width=raw_canvas_width,
            canvas_height=raw_canvas_height,
            title=value.get("title", ""),
            iteration=value.get("iteration", 1),
            metadata=value.get("metadata", {}),
            goal_reached=value.get("goal_reached", False),
            completion_score=value.get("completion_score", 0.0),
        )

    def as_dict(self) -> dict[str, Any]:
        operations: list[dict[str, Any]] = []
        for operation in self.operations:
            item: dict[str, Any] = {
                "kind": operation.kind,
                "id": operation.id,
                "layer": operation.layer,
                "brush": operation.brush.as_dict(),
            }
            if isinstance(operation, PathOperation):
                item.update(
                    points=[point.as_list() for point in operation.points],
                    closed=operation.closed,
                    smooth=operation.smooth,
                    role=operation.role,
                )
            elif isinstance(operation, (FillOperation, HatchOperation, GradientFillOperation)):
                item["polygon"] = [point.as_list() for point in operation.polygon]
                item["spacing"] = operation.spacing
                if isinstance(operation, FillOperation):
                    item["style"] = operation.style
                    if abs(operation.angle_deg) > 1e-3:
                        item["angle_deg"] = operation.angle_deg
                elif isinstance(operation, GradientFillOperation):
                    item["style"] = operation.style
                    item["colors"] = list(operation.colors)
                    if abs(operation.angle_deg) > 1e-3:
                        item["angle_deg"] = operation.angle_deg
                elif isinstance(operation, HatchOperation):
                    item["angle_deg"] = operation.angle_deg
                    item["cross"] = operation.cross
            elif isinstance(operation, RibbonOperation):
                item.update(
                    spine=[point.as_list() for point in operation.spine],
                    width_start=operation.width_start,
                    width_mid=operation.width_mid,
                    width_end=operation.width_end,
                    taper_profile=operation.taper_profile,
                    smooth=operation.smooth,
                )
            elif isinstance(operation, ParticleOperation):
                item.update(
                    bounds=list(operation.bounds),
                    count=operation.count,
                    length=operation.length,
                    angle_deg=operation.angle_deg,
                    angle_jitter=operation.angle_jitter,
                    shape=operation.shape,
                )
            elif isinstance(operation, MacroOperation):
                item.update(
                    name=operation.name,
                    center=list(operation.center),
                    radius=operation.radius,
                    bounds=list(operation.bounds),
                    polygon=[point.as_list() for point in operation.polygon],
                    colors=list(operation.colors),
                    params=dict(operation.params),
                )
            operations.append(item)
        return {
            "schema_version": PROGRAM_SCHEMA_VERSION,
            "prompt": self.prompt,
            "seed": self.seed,
            "title": self.title,
            "iteration": self.iteration,
            "canvas": {"width": self.canvas_width, "height": self.canvas_height},
            "operations": operations,
            "metadata": dict(self.metadata),
            "goal_reached": self.goal_reached,
            "completion_score": self.completion_score,
        }

    def with_canvas_size(self, width: float, height: float) -> StrokeProgram:
        """キャンバス寸法を更新した新しい StrokeProgram を生成する。"""
        new_w = _finite(width, "canvas_width")
        new_h = _finite(height, "canvas_height")
        if new_w < 2 or new_h < 2:
            raise PlanValidationError("canvas_width と canvas_height は 2 以上である必要があります")
        return replace(self, canvas_width=new_w, canvas_height=new_h)


def _operation_uuid(program: StrokeProgram, operation_id: str, index: int) -> str:
    return str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/program/{program.seed}/{operation_id}/{index}"))


def _make_stroke(
    program: StrokeProgram,
    operation: ProgramOperation,
    index: int,
    points: Sequence[tuple[float, float, float]],
) -> Stroke:
    pts = list(points)
    if len(pts) > MAX_STROKE_POINTS:
        step = (len(pts) - 1) / (MAX_STROKE_POINTS - 1)
        sampled = [pts[int(round(i * step))] for i in range(MAX_STROKE_POINTS - 1)]
        sampled.append(pts[-1])
        pts = sampled
    bounded: list[StrokePoint] = []
    for point_index, (x, y, pressure) in enumerate(pts):
        bounded.append(
            StrokePoint(
                x=max(0.0, min(program.canvas_width - 0.5, x)),
                y=max(0.0, min(program.canvas_height - 0.5, y)),
                pressure=max(0.0, min(1.0, pressure)),
                time_ms=point_index * 10,
            )
        )
    stroke_id = (
        operation.id
        if isinstance(operation, PathOperation) and index == 0
        else _operation_uuid(program, operation.id, index)
    )
    is_eraser = bool(
        operation.brush.is_eraser
        or operation.brush.profile == "eraser"
        or "eraser" in (operation.brush.preset_hint or "").lower()
        or operation.layer.lower() == "eraser"
    )
    preset = operation.brush.preset_hint or brush_preset_for_profile("eraser" if is_eraser else operation.brush.profile)
    return Stroke(
        id=stroke_id,
        points=bounded,
        brush_preset=preset,
        color=operation.brush.color,
        size_px=operation.brush.size_px(program.canvas_width, program.canvas_height),
        layer_name=operation.layer,
        opacity=operation.brush.opacity,
        is_eraser=is_eraser,
    )


def _parse_hex_rgb(color_str: str) -> tuple[int, int, int]:
    """HEXカラー文字列 (3, 4, 6, 8 桁) を安全に RGB タプルへ変換する。"""
    c = color_str.strip().lstrip("#")
    if len(c) in (3, 4):
        try:
            return int(c[0] * 2, 16), int(c[1] * 2, 16), int(c[2] * 2, 16)
        except ValueError:
            return (35, 35, 35)
    if len(c) in (6, 8):
        try:
            return int(c[0:2], 16), int(c[2:4], 16), int(c[4:6], 16)
        except ValueError:
            return (35, 35, 35)
    return (35, 35, 35)


def _interpolate_color_hex(c1: str, c2: str, t: float) -> str:
    """2つのHEXカラーをRGB空間で安全に補間する（3/4/6/8桁対応）。"""
    t = max(0.0, min(1.0, float(t)))
    r1, g1, b1 = _parse_hex_rgb(c1)
    r2, g2, b2 = _parse_hex_rgb(c2)
    r = round(r1 + (r2 - r1) * t)
    g = round(g1 + (g2 - g1) * t)
    b = round(b1 + (b2 - b1) * t)
    return f"#{max(0, min(255, r)):02x}{max(0, min(255, g)):02x}{max(0, min(255, b)):02x}"


def _multi_color_interpolate(colors: Sequence[str], t: float) -> str:
    """複数のカラーストップから位置 t (0.0-1.0) の補間色を計算する。"""
    if not colors:
        return "#232323"
    if len(colors) == 1:
        return colors[0]
    t = max(0.0, min(1.0, float(t)))
    scaled = t * (len(colors) - 1)
    idx = min(len(colors) - 2, int(scaled))
    fraction = scaled - idx
    return _interpolate_color_hex(colors[idx], colors[idx + 1], fraction)


def _compile_path(program: StrokeProgram, operation: PathOperation) -> list[Stroke]:
    controls = [
        (point.x * program.canvas_width, point.y * program.canvas_height, point.pressure) for point in operation.points
    ]
    if operation.closed and controls[0] != controls[-1]:
        controls.append(controls[0])
    if operation.smooth and len(controls) >= 3:
        positions = _catmull_rom_spline([(x, y) for x, y, _pressure in controls], samples_per_segment=6)
        dense: list[tuple[float, float, float]] = []
        max_u = len(controls) - 1
        for index, (x, y) in enumerate(positions):
            u = (index / max(1, len(positions) - 1)) * max_u
            left = min(max_u - 1, int(u))
            fraction = u - left
            pressure = controls[left][2] + (controls[left + 1][2] - controls[left][2]) * fraction
            dense.append((x, y, pressure))
    else:
        dense = controls
    if len(dense) > MAX_OPERATION_POINTS:
        step = (len(dense) - 1) / float(MAX_OPERATION_POINTS - 1)
        dense = [dense[int(round(i * step))] for i in range(MAX_OPERATION_POINTS - 1)] + [dense[-1]]

    # 階層的線画ダイナミクス（Role連動テーパリング & 曲率インク溜まり & 端点抜き）
    n_pts = len(dense)
    role = getattr(operation, "role", "auto")
    if not operation.closed and n_pts >= 3:
        modulated_dense: list[tuple[float, float, float]] = []
        for i, (px, py, p_press) in enumerate(dense):
            t_norm = i / max(1, n_pts - 1)
            taper_factor = 1.0
            if role == "hair":
                # 髪の毛: 根元が太く、毛先に向かって鋭くシュッと抜ける
                taper_factor = 0.85 * (1.0 - t_norm * 0.75) if t_norm > 0.1 else 0.5 + 0.5 * (t_norm / 0.1)
            elif role == "detail":
                # 瞳・小鼻・二重等の極細ディテール: 両端を繊細に抜く
                if t_norm < 0.20:
                    taper_factor = 0.30 + 0.70 * (t_norm / 0.20)
                elif t_norm > 0.80:
                    taper_factor = 0.30 + 0.70 * ((1.0 - t_norm) / 0.20)
            elif role == "outline":
                # 主線・外輪郭: しっかりした芯のある線 + 適度な入り抜き
                if t_norm < 0.10:
                    taper_factor = 0.55 + 0.45 * (t_norm / 0.10)
                elif t_norm > 0.90:
                    taper_factor = 0.55 + 0.45 * ((1.0 - t_norm) / 0.10)
            else:
                # 標準 (auto / accent / crevice)
                if t_norm < 0.15:
                    taper_factor = 0.40 + 0.60 * (t_norm / 0.15)
                elif t_norm > 0.85:
                    taper_factor = 0.40 + 0.60 * ((1.0 - t_norm) / 0.15)

            curvature_factor = 1.0
            span = max(1, min(6, n_pts // 5))
            if span <= i <= n_pts - 1 - span:
                p_prev = dense[i - span]
                p_next = dense[i + span]
                v1_x, v1_y = px - p_prev[0], py - p_prev[1]
                v2_x, v2_y = p_next[0] - px, p_next[1] - py
                len1 = math.hypot(v1_x, v1_y)
                len2 = math.hypot(v2_x, v2_y)
                if len1 > 1e-4 and len2 > 1e-4:
                    dot = (v1_x * v2_x + v1_y * v2_y) / (len1 * len2)
                    dot = max(-1.0, min(1.0, dot))
                    if dot < 0.85:
                        accel = 0.45 if role == "outline" else 0.35
                        curvature_factor = 1.0 + accel * (0.85 - dot)

            final_press = max(0.05, min(1.0, p_press * taper_factor * curvature_factor))
            modulated_dense.append((px, py, final_press))
        dense = modulated_dense

    return [_make_stroke(program, operation, 0, dense)]


def _compile_ribbon(program: StrokeProgram, operation: RibbonOperation, limit: int) -> list[Stroke]:
    """髪の毛束・リボン・布のドレープ等を立体ストローク群へコンパイルする。"""
    w = program.canvas_width
    h = program.canvas_height
    scale = min(w, h)
    spine_pts = [(p.x * w, p.y * h, p.pressure) for p in operation.spine]
    if len(spine_pts) < 2:
        return []

    if operation.smooth and len(spine_pts) >= 3:
        coords = [(x, y) for x, y, _p in spine_pts]
        smooth_coords = _catmull_rom_spline(coords, samples_per_segment=8)
        dense: list[tuple[float, float, float]] = []
        max_u = len(spine_pts) - 1
        for idx, (sx, sy) in enumerate(smooth_coords):
            u = (idx / max(1, len(smooth_coords) - 1)) * max_u
            left = min(max_u - 1, int(u))
            frac = u - left
            pr = spine_pts[left][2] + (spine_pts[left + 1][2] - spine_pts[left][2]) * frac
            dense.append((sx, sy, pr))
    else:
        dense = spine_pts

    n = len(dense)
    if n < 2:
        return []

    w_start = operation.width_start * scale
    w_mid = operation.width_mid * scale
    w_end = operation.width_end * scale
    taper_prof = operation.taper_profile

    # 各点での法線ベクトルと幅の計算
    left_edge: list[tuple[float, float, float]] = []
    right_edge: list[tuple[float, float, float]] = []
    center_ridge: list[tuple[float, float, float]] = []

    for i in range(n):
        cx, cy, cp = dense[i]
        t = i / max(1, n - 1)
        if taper_prof == "taper_both":
            cur_w = w_start * (1.0 - t) * 0.5 + w_mid * (math.sin(t * math.pi)) + w_end * t * 0.5
        elif taper_prof == "taper_start":
            cur_w = w_start * (1.0 - t) + w_end * t
        elif taper_prof == "taper_end":
            cur_w = w_mid * (1.0 - t * 0.8) + w_end * t * 0.2
        else:
            cur_w = w_mid

        # 接線と法線
        if i == 0:
            tx = dense[1][0] - cx
            ty = dense[1][1] - cy
        elif i == n - 1:
            tx = cx - dense[n - 2][0]
            ty = cy - dense[n - 2][1]
        else:
            tx = dense[i + 1][0] - dense[i - 1][0]
            ty = dense[i + 1][1] - dense[i - 1][1]

        t_len = math.hypot(tx, ty)
        if t_len > 1e-5:
            nx = -ty / t_len
            ny = tx / t_len
        else:
            nx, ny = 0.0, 1.0

        half_w = cur_w * 0.5
        p_val = max(0.1, min(1.0, cp * (0.4 + 0.6 * math.sin(t * math.pi))))
        left_edge.append((cx + nx * half_w, cy + ny * half_w, p_val))
        right_edge.append((cx - nx * half_w, cy - ny * half_w, p_val))
        center_ridge.append((cx, cy, max(0.2, cp)))

    strokes: list[Stroke] = []
    # 1. リボンのメインボリューム（中心の塗りストローク）
    brush_sz = max(w_mid * 0.75, operation.brush.size_px(w, h))
    ribbon_body = replace(
        operation.brush,
        size=brush_sz if operation.brush.size_mode == "px" else brush_sz / scale,
        opacity=min(1.0, operation.brush.opacity * 0.85),
    )
    strokes.append(
        _make_stroke(
            program,
            replace(operation, brush=ribbon_body),
            len(strokes),
            center_ridge,
        )
    )

    # 2. リボンの左右の稜線・輪郭ストローク (Lineart)
    if len(strokes) < limit:
        edge_brush = replace(
            operation.brush,
            size=max(1.5, scale * 0.003),
            size_mode="px",
            opacity=0.9,
            profile="gpen",
        )
        strokes.append(
            _make_stroke(
                program,
                replace(operation, brush=edge_brush, layer="Lineart"),
                len(strokes),
                left_edge,
            )
        )
    if len(strokes) < limit:
        edge_brush_r = replace(
            operation.brush,
            size=max(1.5, scale * 0.003),
            size_mode="px",
            opacity=0.9,
            profile="gpen",
        )
        strokes.append(
            _make_stroke(
                program,
                replace(operation, brush=edge_brush_r, layer="Lineart"),
                len(strokes),
                right_edge,
            )
        )

    return strokes


def _scanline_segments(polygon: Sequence[tuple[float, float]], y: float) -> list[tuple[float, float]]:
    intersections: list[float] = []
    for first, second in zip(polygon, (*polygon[1:], polygon[0]), strict=False):
        x1, y1 = first
        x2, y2 = second
        if (y1 <= y < y2) or (y2 <= y < y1):
            fraction = (y - y1) / (y2 - y1)
            intersections.append(x1 + (x2 - x1) * fraction)
    intersections.sort()
    return [
        (intersections[index], intersections[index + 1])
        for index in range(0, len(intersections) - 1, 2)
        if intersections[index + 1] - intersections[index] > 0.25
    ]


def _compile_fill_contour(
    program: StrokeProgram,
    operation: FillOperation,
    polygon: Sequence[tuple[float, float]],
    limit: int,
) -> list[Stroke]:
    """面の曲率に沿った輪郭追従（コンター）多重オフセット塗り。"""
    cx = sum(p[0] for p in polygon) / len(polygon)
    cy = sum(p[1] for p in polygon) / len(polygon)
    max_radius = max(math.hypot(p[0] - cx, p[1] - cy) for p in polygon)
    if max_radius < 1.0:
        return []

    brush_px = operation.brush.size_px(program.canvas_width, program.canvas_height)
    step_r = max(0.8, brush_px * 0.65)
    steps = max(1, min(limit, int(max_radius / step_r)))

    strokes: list[Stroke] = []
    poly_pts = list(polygon)
    if poly_pts[0] != poly_pts[-1]:
        poly_pts.append(poly_pts[0])

    for step_i in range(steps):
        if len(strokes) >= limit:
            break
        scale = max(0.02, 1.0 - (step_i / steps))
        scaled_ring = [(cx + (px - cx) * scale, cy + (py - cy) * scale) for px, py in poly_pts]
        if len(scaled_ring) >= 3:
            spline_pts = _catmull_rom_spline(scaled_ring, samples_per_segment=6)
            pts_with_p = [(sx, sy, 0.85) for sx, sy in spline_pts]
        else:
            pts_with_p = [(sx, sy, 0.85) for sx, sy in scaled_ring]
        strokes.append(_make_stroke(program, operation, len(strokes), pts_with_p))

    return strokes


def _compile_fill_radial(
    program: StrokeProgram,
    operation: FillOperation,
    polygon: Sequence[tuple[float, float]],
    limit: int,
) -> list[Stroke]:
    """中心から外周へ向かう放射状グラデーション塗り。"""
    cx = sum(p[0] for p in polygon) / len(polygon)
    cy = sum(p[1] for p in polygon) / len(polygon)
    max_radius = max(math.hypot(p[0] - cx, p[1] - cy) for p in polygon)
    if max_radius < 1.0:
        return []

    brush_px = operation.brush.size_px(program.canvas_width, program.canvas_height)
    ray_count = max(8, min(limit, int(2.0 * math.pi * max_radius / max(1.0, brush_px * 0.8))))

    strokes: list[Stroke] = []
    for ray_i in range(ray_count):
        if len(strokes) >= limit:
            break
        ang = (ray_i / ray_count) * 2.0 * math.pi
        dir_x = math.cos(ang)
        dir_y = math.sin(ang)

        ray_radius = _ray_polygon_distance((cx, cy), (dir_x, dir_y), polygon)
        if ray_radius is None:
            continue
        outer_x = cx + dir_x * ray_radius
        outer_y = cy + dir_y * ray_radius
        mid_x = cx + dir_x * ray_radius * 0.5
        mid_y = cy + dir_y * ray_radius * 0.5
        pts = [
            (cx, cy, 0.95),
            (mid_x, mid_y, 0.80),
            (outer_x, outer_y, 0.20),
        ]
        strokes.append(_make_stroke(program, operation, len(strokes), pts))

    return strokes


def _ray_polygon_distance(
    origin: tuple[float, float], direction: tuple[float, float], polygon: Sequence[tuple[float, float]]
) -> float | None:
    """Ray と polygon 辺の最も近い前方交点までの距離を返す。"""
    ox, oy = origin
    dx, dy = direction
    distances: list[float] = []
    for first, second in zip(polygon, (*polygon[1:], polygon[0]), strict=False):
        edge_x = second[0] - first[0]
        edge_y = second[1] - first[1]
        denominator = dx * edge_y - dy * edge_x
        if abs(denominator) < 1e-9:
            continue
        rel_x = first[0] - ox
        rel_y = first[1] - oy
        ray_t = (rel_x * edge_y - rel_y * edge_x) / denominator
        edge_t = (rel_x * dy - rel_y * dx) / denominator
        if ray_t >= 0.0 and 0.0 <= edge_t <= 1.0:
            distances.append(ray_t)
    return min(distances) if distances else None


def _compile_fill_directional(
    program: StrokeProgram,
    operation: FillOperation,
    polygon: Sequence[tuple[float, float]],
    angle_deg: float,
    limit: int,
) -> list[Stroke]:
    """指定角度に沿ったスキャンライン塗り。"""
    center = (
        sum(p[0] for p in polygon) / len(polygon),
        sum(p[1] for p in polygon) / len(polygon),
    )
    angle = math.radians(angle_deg)
    rotated = [_rotate(p, center, -angle) for p in polygon]
    min_y = min(p[1] for p in rotated)
    max_y = max(p[1] for p in rotated)
    spacing_scale = 0.55 if operation.style in {"wash", "feathered"} else operation.spacing
    spacing = max(0.5, operation.brush.size_px(program.canvas_width, program.canvas_height) * spacing_scale)
    strokes: list[Stroke] = []
    row = 0
    y = min_y + spacing * 0.5
    while y < max_y and len(strokes) < limit:
        segments = _scanline_segments(rotated, y)
        if row % 2:
            segments.reverse()
        for start_x, end_x in segments:
            if len(strokes) >= limit:
                break
            first_x, second_x = (end_x, start_x) if row % 2 else (start_x, end_x)
            first_rot = _rotate((first_x, y), center, angle)
            second_rot = _rotate((second_x, y), center, angle)
            seg_len = math.hypot(second_rot[0] - first_rot[0], second_rot[1] - first_rot[1])
            if operation.style in {"wash", "feathered", "directional"} and seg_len > 4.0:
                mid_x = (first_rot[0] + second_rot[0]) * 0.5
                mid_y = (first_rot[1] + second_rot[1]) * 0.5
                # 微細な有機的たわみ（水彩ストロークの手描き感）
                dx = second_rot[0] - first_rot[0]
                dy = second_rot[1] - first_rot[1]
                perp_x = -dy / max(1e-5, seg_len) * min(3.0, seg_len * 0.04)
                perp_y = dx / max(1e-5, seg_len) * min(3.0, seg_len * 0.04)
                jitter_sign = 1.0 if row % 2 == 0 else -1.0
                p1_x = first_rot[0] + dx * 0.25 + perp_x * jitter_sign
                p1_y = first_rot[1] + dy * 0.25 + perp_y * jitter_sign
                p2_x = first_rot[0] + dx * 0.75 - perp_x * jitter_sign
                p2_y = first_rot[1] + dy * 0.75 - perp_y * jitter_sign
                pts = [
                    (first_rot[0], first_rot[1], 0.45),
                    (p1_x, p1_y, 0.90),
                    (mid_x, mid_y, 0.95),
                    (p2_x, p2_y, 0.90),
                    (second_rot[0], second_rot[1], 0.45),
                ]
            else:
                pts = [(first_rot[0], first_rot[1], 1.0), (second_rot[0], second_rot[1], 1.0)]
            strokes.append(_make_stroke(program, operation, len(strokes), pts))
        row += 1
        y += spacing

    # フォールバック: polygon が薄く走査線間隔内に収まらなかった場合、中心断面をサンプリング
    if not strokes and limit > 0:
        mid_y = (min_y + max_y) * 0.5
        segments = _scanline_segments(rotated, mid_y)
        if not segments:
            min_x = min(p[0] for p in rotated)
            max_x = max(p[0] for p in rotated)
            if max_x - min_x >= 0.25:
                segments = [(min_x, max_x)]
            else:
                segments = [(min_x, max_x + 0.5)]
        for start_x, end_x in segments:
            if len(strokes) >= limit:
                break
            first_rot = _rotate((start_x, mid_y), center, angle)
            second_rot = _rotate((end_x, mid_y), center, angle)
            strokes.append(
                _make_stroke(
                    program,
                    operation,
                    len(strokes),
                    [(first_rot[0], first_rot[1], 1.0), (second_rot[0], second_rot[1], 1.0)],
                )
            )

    # 水彩ウォッシュ時のウェットエッジ（絵の具溜まり輪郭線）
    if operation.style in {"wash", "feathered"} and len(strokes) < limit and len(polygon) >= 3:
        closed_poly = list(polygon)
        if closed_poly[0] != closed_poly[-1]:
            closed_poly.append(closed_poly[0])
        spline_edge = _catmull_rom_spline(closed_poly, samples_per_segment=4)
        edge_pts = [(ex, ey, 0.65) for ex, ey in spline_edge]
        strokes.append(_make_stroke(program, operation, len(strokes), edge_pts))

    return strokes


def _compile_fill(program: StrokeProgram, operation: FillOperation, limit: int) -> list[Stroke]:
    polygon = [(point.x * program.canvas_width, point.y * program.canvas_height) for point in operation.polygon]
    if operation.style == "contour":
        res = _compile_fill_contour(program, operation, polygon, limit)
        if res:
            return res
    if operation.style == "radial":
        res = _compile_fill_radial(program, operation, polygon, limit)
        if res:
            return res
    if operation.style == "directional" or abs(operation.angle_deg) > 1e-3:
        res = _compile_fill_directional(program, operation, polygon, operation.angle_deg, limit)
        if res:
            return res

    min_y = min(point[1] for point in polygon)
    max_y = max(point[1] for point in polygon)
    spacing_scale = 0.55 if operation.style in {"wash", "feathered"} else operation.spacing
    spacing = max(0.5, operation.brush.size_px(program.canvas_width, program.canvas_height) * spacing_scale)
    strokes: list[Stroke] = []
    row = 0
    y = min_y + spacing * 0.5
    while y < max_y and len(strokes) < limit:
        segments = _scanline_segments(polygon, y)
        if row % 2:
            segments.reverse()
        for start_x, end_x in segments:
            if len(strokes) >= limit:
                break
            first_x, second_x = (end_x, start_x) if row % 2 else (start_x, end_x)
            seg_len = abs(second_x - first_x)
            if operation.style in {"wash", "feathered"} and seg_len > 4.0:
                mid_x = (first_x + second_x) * 0.5
                dy_jitter = min(2.5, seg_len * 0.03) * (1.0 if row % 2 == 0 else -1.0)
                p1_x = first_x + (second_x - first_x) * 0.25
                p1_y = y + dy_jitter
                p2_x = first_x + (second_x - first_x) * 0.75
                p2_y = y - dy_jitter
                pts = [
                    (first_x, y, 0.45),
                    (p1_x, p1_y, 0.90),
                    (mid_x, y, 0.95),
                    (p2_x, p2_y, 0.90),
                    (second_x, y, 0.45),
                ]
            else:
                pts = [(first_x, y, 1.0), (second_x, y, 1.0)]
            strokes.append(_make_stroke(program, operation, len(strokes), pts))
        row += 1
        y += spacing

    # フォールバック: polygon が薄く走査線間隔内に収まらなかった場合、中心断面をサンプリング
    if not strokes and limit > 0:
        mid_y = (min_y + max_y) * 0.5
        segments = _scanline_segments(polygon, mid_y)
        if not segments:
            min_x = min(p[0] for p in polygon)
            max_x = max(p[0] for p in polygon)
            if max_x - min_x >= 0.25:
                segments = [(min_x, max_x)]
            else:
                segments = [(min_x, max_x + 0.5)]
        for start_x, end_x in segments:
            if len(strokes) >= limit:
                break
            strokes.append(
                _make_stroke(
                    program,
                    operation,
                    len(strokes),
                    [(start_x, mid_y, 1.0), (end_x, mid_y, 1.0)],
                )
            )

    # 水彩ウォッシュ時のウェットエッジ（絵の具溜まり輪郭線）
    if operation.style in {"wash", "feathered"} and len(strokes) < limit and len(polygon) >= 3:
        closed_poly = list(polygon)
        if closed_poly[0] != closed_poly[-1]:
            closed_poly.append(closed_poly[0])
        spline_edge = _catmull_rom_spline(closed_poly, samples_per_segment=4)
        edge_pts = [(ex, ey, 0.65) for ex, ey in spline_edge]
        strokes.append(_make_stroke(program, operation, len(strokes), edge_pts))

    return strokes


def _rotate(point: tuple[float, float], center: tuple[float, float], angle: float) -> tuple[float, float]:
    cosine = math.cos(angle)
    sine = math.sin(angle)
    dx = point[0] - center[0]
    dy = point[1] - center[1]
    return center[0] + dx * cosine - dy * sine, center[1] + dx * sine + dy * cosine


def _compile_hatch_angle(
    program: StrokeProgram,
    operation: HatchOperation,
    angle_deg: float,
    start_index: int,
    limit: int,
) -> list[Stroke]:
    polygon = [(point.x * program.canvas_width, point.y * program.canvas_height) for point in operation.polygon]
    center = (
        sum(point[0] for point in polygon) / len(polygon),
        sum(point[1] for point in polygon) / len(polygon),
    )
    angle = math.radians(angle_deg)
    rotated = [_rotate(point, center, -angle) for point in polygon]
    min_y = min(point[1] for point in rotated)
    max_y = max(point[1] for point in rotated)
    raw_spacing = operation.spacing * min(program.canvas_width, program.canvas_height)
    brush_px = operation.brush.size_px(program.canvas_width, program.canvas_height)
    # 高解像度キャンバスでハッチングが粗すぎるゼブラ縞になるのを抑制
    spacing = max(1.0, min(raw_spacing, max(1.5, brush_px * 2.8)))
    strokes: list[Stroke] = []
    y = min_y + spacing * 0.5
    while y < max_y and start_index + len(strokes) < limit:
        for start_x, end_x in _scanline_segments(rotated, y):
            if start_index + len(strokes) >= limit:
                break
            first = _rotate((start_x, y), center, angle)
            second = _rotate((end_x, y), center, angle)
            line_len = math.hypot(second[0] - first[0], second[1] - first[1])
            if line_len > 4.0:
                mid_x = (first[0] + second[0]) * 0.5
                mid_y = (first[1] + second[1]) * 0.5
                pts = [
                    (first[0], first[1], 0.25),
                    (mid_x, mid_y, 0.85),
                    (second[0], second[1], 0.25),
                ]
            else:
                pts = [(first[0], first[1], 0.65), (second[0], second[1], 0.65)]
            strokes.append(
                _make_stroke(
                    program,
                    operation,
                    start_index + len(strokes),
                    pts,
                )
            )
        y += spacing

    # フォールバック: polygon が薄く走査線間隔内に収まらなかった場合、中心断面をサンプリング
    if not strokes and start_index < limit:
        mid_y = (min_y + max_y) * 0.5
        segments = _scanline_segments(rotated, mid_y)
        if not segments:
            min_x = min(p[0] for p in rotated)
            max_x = max(p[0] for p in rotated)
            if max_x - min_x >= 0.25:
                segments = [(min_x, max_x)]
            else:
                segments = [(min_x, max_x + 0.5)]
        for start_x, end_x in segments:
            if start_index + len(strokes) >= limit:
                break
            first = _rotate((start_x, mid_y), center, angle)
            second = _rotate((end_x, mid_y), center, angle)
            strokes.append(
                _make_stroke(
                    program,
                    operation,
                    start_index + len(strokes),
                    [(first[0], first[1], 0.65), (second[0], second[1], 0.65)],
                )
            )

    return strokes


def _compile_hatch(program: StrokeProgram, operation: HatchOperation, limit: int) -> list[Stroke]:
    first_pass_limit = max(1, math.ceil(limit / 2)) if operation.cross else limit
    strokes = _compile_hatch_angle(program, operation, operation.angle_deg, 0, first_pass_limit)
    if operation.cross and len(strokes) < limit:
        strokes.extend(_compile_hatch_angle(program, operation, operation.angle_deg + 90.0, len(strokes), limit))
    return strokes


def _compile_particles(program: StrokeProgram, operation: ParticleOperation, limit: int) -> list[Stroke]:
    rng = random.Random(f"{program.seed}:{operation.id}")
    x0, y0, x1, y1 = operation.bounds
    base_length = operation.length * min(program.canvas_width, program.canvas_height)
    strokes: list[Stroke] = []
    shape = getattr(operation, "shape", "petal")
    for index in range(min(operation.count, limit)):
        start_x = rng.uniform(x0, x1) * program.canvas_width
        start_y = rng.uniform(y0, y1) * program.canvas_height
        angle = math.radians(operation.angle_deg + rng.uniform(-operation.angle_jitter, operation.angle_jitter))
        length = base_length * rng.uniform(0.65, 1.35)
        dx = math.cos(angle) * length
        dy = math.sin(angle) * length
        end_x = start_x + dx
        end_y = start_y + dy

        if shape == "petal":
            curl_mag = length * rng.uniform(-0.35, 0.35)
            norm_x = -dy / max(1e-5, length)
            norm_y = dx / max(1e-5, length)
            mid_x = start_x + dx * 0.5 + norm_x * curl_mag
            mid_y = start_y + dy * 0.5 + norm_y * curl_mag
            pts = [
                (start_x, start_y, 0.25),
                (mid_x, mid_y, 0.95),
                (end_x, end_y, 0.15),
            ]
        elif shape == "sparkle":
            mid_x = (start_x + end_x) * 0.5
            mid_y = (start_y + end_y) * 0.5
            norm_x = -dy / max(1e-5, length)
            norm_y = dx / max(1e-5, length)
            cross = length * 0.42
            pts = [
                (start_x, start_y, 0.15),
                (mid_x, mid_y, 1.0),
                (end_x, end_y, 0.15),
                (mid_x, mid_y, 0.25),
                (mid_x + norm_x * cross, mid_y + norm_y * cross, 0.12),
                (mid_x, mid_y, 1.0),
                (mid_x - norm_x * cross, mid_y - norm_y * cross, 0.12),
            ]
        elif shape == "bokeh":
            center_x = (start_x + end_x) * 0.5
            center_y = (start_y + end_y) * 0.5
            radius = max(0.5, length * 0.5)
            pts = [
                (
                    center_x + math.cos(math.tau * point_index / 12.0) * radius,
                    center_y + math.sin(math.tau * point_index / 12.0) * radius,
                    0.35,
                )
                for point_index in range(13)
            ]
        elif shape == "drift":
            p1_x = start_x + dx * 0.33 + (-dy / max(1e-5, length)) * length * 0.15
            p1_y = start_y + dy * 0.33 + (dx / max(1e-5, length)) * length * 0.15
            p2_x = start_x + dx * 0.66 - (-dy / max(1e-5, length)) * length * 0.15
            p2_y = start_y + dy * 0.66 - (dx / max(1e-5, length)) * length * 0.15
            pts = [
                (start_x, start_y, 0.3),
                (p1_x, p1_y, 0.8),
                (p2_x, p2_y, 0.7),
                (end_x, end_y, 0.1),
            ]
        else:
            pts = [(start_x, start_y, 0.7), (end_x, end_y, 0.15)]

        strokes.append(_make_stroke(program, operation, index, pts))
    return strokes


def _polygon_area(operation: FillOperation | GradientFillOperation) -> float:
    points = operation.polygon
    return (
        abs(
            sum(
                first.x * second.y - second.x * first.y
                for first, second in zip(points, (*points[1:], points[0]), strict=False)
            )
        )
        * 0.5
    )


def _allocate_fill_budgets(operations: Sequence[FillOperation | GradientFillOperation], total: int) -> list[int]:
    """大面積の背景を優先しつつ、選択した各 fill に最低1本を割り当てる。"""
    if total <= 0 or not operations:
        return [0] * len(operations)
    selected_count = min(len(operations), total)
    ranked = sorted(range(len(operations)), key=lambda index: _polygon_area(operations[index]), reverse=True)
    selected = set(ranked[:selected_count])
    budgets = [1 if index in selected else 0 for index in range(len(operations))]
    remaining = total - selected_count
    if remaining <= 0:
        return budgets

    weights = [
        math.sqrt(max(1e-6, _polygon_area(operation))) if index in selected else 0.0
        for index, operation in enumerate(operations)
    ]
    weight_sum = sum(weights)
    raw_shares = [remaining * weight / weight_sum if weight_sum > 0 else 0.0 for weight in weights]
    for index, share in enumerate(raw_shares):
        budgets[index] += int(math.floor(share))
    leftover = total - sum(budgets)
    fractional_order = sorted(
        selected,
        key=lambda index: (raw_shares[index] - math.floor(raw_shares[index]), weights[index]),
        reverse=True,
    )
    for index in fractional_order[:leftover]:
        budgets[index] += 1
    return budgets


def _compile_fill_to_budget(program: StrokeProgram, operation: FillOperation, limit: int) -> list[Stroke]:
    """走査線を欠落させず、予算が少ない時はブラシを太く再コンパイルする。"""
    if limit <= 0:
        return []
    polygon = [(point.x * program.canvas_width, point.y * program.canvas_height) for point in operation.polygon]
    span = max(max(point[1] for point in polygon) - min(point[1] for point in polygon), 1.0)
    if operation.style in {"directional", "radial", "contour"} or abs(operation.angle_deg) > 1e-3:
        span = max(span, max(point[0] for point in polygon) - min(point[0] for point in polygon))
    spacing_scale = 0.50 if operation.style in {"wash", "feathered"} else operation.spacing
    current_px = operation.brush.size_px(program.canvas_width, program.canvas_height)
    required_px = span / max(1.0, limit * spacing_scale) * 1.08
    adjusted = operation
    if required_px > current_px:
        adjusted_size = (
            required_px
            if operation.brush.size_mode == "px"
            else required_px / min(program.canvas_width, program.canvas_height)
        )
        adjusted = replace(operation, brush=replace(operation.brush, size=adjusted_size))
    return _compile_fill(program, adjusted, limit)


def _create_macro_stroke(
    stroke_id: str,
    points: Sequence[tuple[float, float, float] | tuple[float, float]],
    profile: str,
    color: str,
    size_px: float,
    layer_name: str,
    opacity: float = 1.0,
    is_eraser: bool = False,
) -> Stroke:
    stroke_points: list[StrokePoint] = []
    for idx, pt in enumerate(points):
        x = float(pt[0])
        y = float(pt[1])
        pressure = float(pt[2]) if len(pt) >= 3 else 0.8
        stroke_points.append(StrokePoint(x=x, y=y, pressure=max(0.05, min(1.0, pressure)), time_ms=idx * 15))
    return Stroke(
        id=stroke_id,
        points=tuple(stroke_points),
        brush_preset=brush_preset_for_profile(profile),
        color=normalize_hex_color(color),
        size_px=max(0.5, size_px),
        layer_name=layer_name,
        opacity=max(0.0, min(1.0, opacity)),
        is_eraser=is_eraser,
    )


def _compile_macro(
    program: StrokeProgram,
    operation: MacroOperation,
    limit: int,
) -> list[Stroke]:
    """プロシージャル・セマンティック・マクロを高品位なストローク群へコンパイルする。"""
    w = program.canvas_width
    h = program.canvas_height
    scale = min(w, h)
    name = operation.name.lower()
    cx = operation.center[0] * w
    cy = operation.center[1] * h
    r = operation.radius * scale
    # ``hash()`` is intentionally randomized between Python processes.  Using it
    # here made the same seed produce different macro artwork after a restart,
    # which also made saved/replayed programs impossible to compare reliably.
    operation_digest = hashlib.blake2s(operation.id.encode("utf-8"), digest_size=8).digest()
    operation_hash = int.from_bytes(operation_digest, "big")
    rng = random.Random(program.seed + (operation_hash % 100000))
    strokes: list[Stroke] = []

    def uid(sub: str, idx: int = 0) -> str:
        return str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/macro/{program.seed}/{operation.id}/{sub}/{idx}"))

    # -------------------------------------------------------------------------
    # 1. 花・桜・バラ・野花 (Flower / Sakura / Rose / Wildflower)
    # -------------------------------------------------------------------------
    if any(k in name for k in ("flower", "sakura", "rose", "blossom", "wildflower", "botanical")):
        is_sakura = "sakura" in name or "blossom" in name
        is_rose = "rose" in name
        is_wildflower = "wildflower" in name or "meadow" in name or "garden" in name

        base_colors = (
            list(operation.colors)
            if operation.colors
            else (
                ["#ffb8cd", "#ffd6e5", "#ff9ebb"]
                if is_sakura
                else ["#e05370", "#d62246", "#f28b9d"]
                if is_rose
                else ["#d95d8a", "#e9a13a", "#8b6fc0"]
            )
        )
        # 花房のベース（ふんわりした3層の重なり円弧スプライン）
        for b_i in range(3):
            if len(strokes) >= limit:
                break
            b_r = r * (0.4 + b_i * 0.25)
            b_ang = b_i * 1.8
            b_cx = cx + math.cos(b_ang) * r * 0.15
            b_cy = cy + math.sin(b_ang) * r * 0.15
            pts_circ = [
                (
                    b_cx + math.cos(deg) * b_r * (1.0 + rng.uniform(-0.08, 0.08)),
                    b_cy + math.sin(deg) * b_r * (1.0 + rng.uniform(-0.08, 0.08)),
                )
                for deg in (0.0, math.pi * 0.5, math.pi, math.pi * 1.5, math.pi * 2.0)
            ]
            spline_circ = _catmull_rom_spline(pts_circ, samples_per_segment=6)
            pts_with_p = [(sx, sy, 0.75 + rng.uniform(-0.1, 0.1)) for sx, sy in spline_circ]
            strokes.append(
                _create_macro_stroke(
                    stroke_id=uid("flower_base", b_i),
                    points=pts_with_p,
                    profile="watercolor",
                    color=base_colors[b_i % len(base_colors)],
                    size_px=max(12.0, r * 0.45),
                    layer_name="Flats",
                    opacity=0.60,
                )
            )

        # 花弁の有機的スプライン
        if is_rose:
            petal_layers = 4
            for p_layer in range(petal_layers):
                petals_in_layer = 4 + p_layer * 2
                r_layer = r * (0.2 + p_layer * 0.18)
                for p_idx in range(petals_in_layer):
                    if len(strokes) >= limit:
                        break
                    base_ang = (p_idx / petals_in_layer) * math.pi * 2.0 + p_layer * 0.6
                    p0 = (cx + math.cos(base_ang) * r_layer * 0.8, cy + math.sin(base_ang) * r_layer * 0.8)
                    p1 = (cx + math.cos(base_ang + 0.3) * r_layer * 1.2, cy + math.sin(base_ang + 0.3) * r_layer * 1.2)
                    p2 = (cx + math.cos(base_ang + 0.6) * r_layer * 1.1, cy + math.sin(base_ang + 0.6) * r_layer * 1.1)
                    p3 = (cx + math.cos(base_ang + 0.9) * r_layer * 0.8, cy + math.sin(base_ang + 0.9) * r_layer * 0.8)
                    petal_spline = _catmull_rom_spline([p0, p1, p2, p3], 6)
                    col = base_colors[(p_layer + p_idx) % len(base_colors)]
                    pts_p = [
                        (sx, sy, 0.3 + 0.6 * math.sin(i / max(1, len(petal_spline) - 1) * math.pi))
                        for i, (sx, sy) in enumerate(petal_spline)
                    ]
                    strokes.append(
                        _create_macro_stroke(
                            stroke_id=uid("rose_petal", p_layer * 10 + p_idx),
                            points=pts_p,
                            profile="gpen" if p_layer > 1 else "brush",
                            color=col,
                            size_px=max(2.5, scale * 0.005),
                            layer_name="Lineart" if p_layer > 1 else "Flats",
                            opacity=0.85,
                        )
                    )
        else:
            num_petals = 5 if is_sakura else 6
            for p_i in range(num_petals):
                if len(strokes) >= limit:
                    break
                p_ang = (p_i / num_petals) * math.pi * 2.0 + rng.uniform(-0.1, 0.1)
                p_len = r * (0.65 + rng.uniform(0.0, 0.35))
                tip_x = cx + math.cos(p_ang) * p_len
                tip_y = cy + math.sin(p_ang) * p_len
                side_w = p_len * 0.38
                perp_ang = p_ang + math.pi * 0.5
                c1_x = cx + math.cos(p_ang) * p_len * 0.45 + math.cos(perp_ang) * side_w
                c1_y = cy + math.sin(p_ang) * p_len * 0.45 + math.sin(perp_ang) * side_w
                c2_x = cx + math.cos(p_ang) * p_len * 0.45 - math.cos(perp_ang) * side_w
                c2_y = cy + math.sin(p_ang) * p_len * 0.45 - math.sin(perp_ang) * side_w

                petal_pts = _catmull_rom_spline([(cx, cy), (c1_x, c1_y), (tip_x, tip_y), (c2_x, c2_y), (cx, cy)], 6)
                pts_with_p = [
                    (sx, sy, 0.4 + 0.55 * math.sin(idx / max(1, len(petal_pts) - 1) * math.pi))
                    for idx, (sx, sy) in enumerate(petal_pts)
                ]
                strokes.append(
                    _create_macro_stroke(
                        stroke_id=uid("petal_stroke", p_i),
                        points=pts_with_p,
                        profile="watercolor",
                        color=base_colors[p_i % len(base_colors)],
                        size_px=max(4.0, p_len * 0.35),
                        layer_name="Flats",
                        opacity=0.75,
                    )
                )

                strokes.append(
                    _create_macro_stroke(
                        stroke_id=uid("petal_line", p_i),
                        points=[(sx, sy, max(0.15, p * 0.8)) for sx, sy, p in pts_with_p],
                        profile="gpen",
                        color="#3a1c28" if is_sakura else "#4a2a35",
                        size_px=max(1.5, scale * 0.0028),
                        layer_name="Lineart",
                        opacity=0.85,
                    )
                )

        # 花芯・めしべ
        if len(strokes) < limit:
            center_pts = _catmull_rom_spline(
                [
                    (cx - r * 0.08, cy - r * 0.08),
                    (cx + r * 0.08, cy - r * 0.06),
                    (cx + r * 0.06, cy + r * 0.08),
                    (cx - r * 0.08, cy + r * 0.06),
                    (cx - r * 0.08, cy - r * 0.08),
                ],
                4,
            )
            strokes.append(
                _create_macro_stroke(
                    stroke_id=uid("flower_center"),
                    points=[(x, y, 0.95) for x, y in center_pts],
                    profile="marupen",
                    color="#ffd700" if is_sakura or is_wildflower else "#ffdd88",
                    size_px=max(2.5, r * 0.18),
                    layer_name="Highlights",
                    opacity=0.95,
                )
            )

        # 茎
        if (operation.params.get("has_stem", True) or is_wildflower) and len(strokes) < limit:
            stem_end_y = min(h, cy + r * 2.2)
            stem_pts = _catmull_rom_spline(
                [
                    (cx, cy + r * 0.2),
                    (cx + rng.uniform(-r * 0.2, r * 0.2), cy + r * 1.1),
                    (cx + rng.uniform(-r * 0.1, r * 0.1), stem_end_y),
                ],
                6,
            )
            strokes.append(
                _create_macro_stroke(
                    stroke_id=uid("flower_stem"),
                    points=[(x, y, 0.8) for x, y in stem_pts],
                    profile="brush",
                    color="#3f7f58",
                    size_px=max(2.5, scale * 0.005),
                    layer_name="Lineart",
                    opacity=0.85,
                )
            )

        return strokes

    # -------------------------------------------------------------------------
    # 2. 樹木・桜の木・枝分かれ (Branch Tree / Trunk / Canopy)
    # -------------------------------------------------------------------------
    if any(k in name for k in ("tree", "branch", "trunk", "wood")):
        root_x = cx
        root_y = min(h * 0.98, cy + r)
        tree_h = max(30.0, r * 1.8)
        top_y = root_y - tree_h
        trunk_color = operation.colors[0] if operation.colors else "#342017"
        bark_shadow = "#1f120c"

        # 主幹 (根張り・うねり・テーパー)
        trunk_ctrls = [
            (root_x - scale * 0.015, root_y),
            (root_x, root_y - tree_h * 0.15),
            (root_x - scale * 0.018, root_y - tree_h * 0.45),
            (root_x + scale * 0.014, root_y - tree_h * 0.72),
            (root_x - scale * 0.008, top_y),
        ]
        trunk_pts = _catmull_rom_spline(trunk_ctrls, 10)
        pts_trunk_p = [
            (x, y, max(0.18, 0.95 - (i / max(1, len(trunk_pts) - 1)) * 0.72)) for i, (x, y) in enumerate(trunk_pts)
        ]
        # 幹の陰影（立体感のある樹皮シャドウ）
        strokes.append(
            _create_macro_stroke(
                stroke_id=uid("trunk_shadow"),
                points=[(x + scale * 0.004, y, p * 0.8) for x, y, p in pts_trunk_p],
                profile="brush",
                color=bark_shadow,
                size_px=max(6.0, scale * 0.024),
                layer_name="Shading",
                opacity=0.65,
            )
        )
        strokes.append(
            _create_macro_stroke(
                stroke_id=uid("trunk_main"),
                points=pts_trunk_p,
                profile="gpen",
                color=trunk_color,
                size_px=max(5.5, scale * 0.020),
                layer_name="Lineart",
                opacity=0.95,
            )
        )
        # 根張りの広がり
        left_root = _catmull_rom_spline([(root_x, root_y - tree_h * 0.08), (root_x - scale * 0.04, root_y)], 6)
        right_root = _catmull_rom_spline([(root_x, root_y - tree_h * 0.08), (root_x + scale * 0.035, root_y)], 6)
        strokes.append(
            _create_macro_stroke(
                stroke_id=uid("root_l"),
                points=[(x, y, 0.75 - 0.4 * (i / max(1, len(left_root) - 1))) for i, (x, y) in enumerate(left_root)],
                profile="gpen",
                color=trunk_color,
                size_px=max(3.5, scale * 0.012),
                layer_name="Lineart",
                opacity=0.90,
            )
        )
        strokes.append(
            _create_macro_stroke(
                stroke_id=uid("root_r"),
                points=[(x, y, 0.75 - 0.4 * (i / max(1, len(right_root) - 1))) for i, (x, y) in enumerate(right_root)],
                profile="gpen",
                color=trunk_color,
                size_px=max(3.5, scale * 0.012),
                layer_name="Lineart",
                opacity=0.90,
            )
        )

        # 有機的な大枝・中枝の階層分岐 (Boughs & Twigs)
        bough_specs = [
            (-1.0, 0.48, 0.38, 0.18, 0.32),
            (1.0, 0.58, 0.42, 0.14, 0.28),
            (-1.0, 0.72, 0.32, 0.10, 0.22),
            (1.0, 0.80, 0.28, 0.08, 0.18),
            (-0.6, 0.90, 0.18, 0.06, 0.12),
            (0.7, 0.94, 0.16, 0.05, 0.10),
        ]
        for b_idx, (side, h_frac, spread, lift, arc) in enumerate(bough_specs):
            if len(strokes) >= limit:
                break
            b_start_y = root_y - tree_h * h_frac
            b_start_x = root_x + side * scale * 0.012
            b_ctrl1_x = b_start_x + side * scale * spread * 0.35
            b_ctrl1_y = b_start_y - scale * lift * 0.2
            b_ctrl2_x = b_start_x + side * scale * spread * 0.75
            b_ctrl2_y = b_start_y - scale * (lift + arc) * 0.65
            b_end_x = b_start_x + side * scale * spread
            b_end_y = b_start_y - scale * (lift + arc)
            b_pts = _catmull_rom_spline(
                [(b_start_x, b_start_y), (b_ctrl1_x, b_ctrl1_y), (b_ctrl2_x, b_ctrl2_y), (b_end_x, b_end_y)],
                8,
            )
            b_pts_p = [(x, y, max(0.12, 0.80 - (i / max(1, len(b_pts) - 1)) * 0.68)) for i, (x, y) in enumerate(b_pts)]
            strokes.append(
                _create_macro_stroke(
                    stroke_id=uid("tree_bough", b_idx),
                    points=b_pts_p,
                    profile="gpen",
                    color=trunk_color,
                    size_px=max(2.8, scale * 0.010 * (1.0 - h_frac * 0.35)),
                    layer_name="Lineart",
                    opacity=0.92,
                )
            )

            # 小枝分岐 (Twigs)
            if b_idx < 4 and len(strokes) < limit:
                t_start_i = len(b_pts) // 2
                t_start = b_pts[t_start_i]
                t_end_x = t_start[0] + side * scale * spread * 0.35 + rng.uniform(-scale * 0.01, scale * 0.01)
                t_end_y = t_start[1] - scale * 0.04
                t_pts = _catmull_rom_spline(
                    [t_start, ((t_start[0] + t_end_x) * 0.5, t_start[1] - scale * 0.02), (t_end_x, t_end_y)], 6
                )
                strokes.append(
                    _create_macro_stroke(
                        stroke_id=uid(f"tree_twig_{b_idx}"),
                        points=[
                            (x, y, max(0.10, 0.60 - (i / max(1, len(t_pts) - 1)) * 0.50))
                            for i, (x, y) in enumerate(t_pts)
                        ],
                        profile="gpen",
                        color=trunk_color,
                        size_px=max(1.8, scale * 0.005),
                        layer_name="Lineart",
                        opacity=0.88,
                    )
                )

        # 樹冠の花房・葉の多層ボリューム (Canopy Layers)
        if operation.params.get("foliage", True):
            is_sakura_theme = "sakura" in name or "pink" in str(operation.colors) or "blossom" in name
            deep_shadow_col = "#9d4b68" if is_sakura_theme else "#234d31"
            blossom_colors = (
                ["#f7a8c4", "#ffb8cd", "#ffd6e5", "#fff0f5"]
                if is_sakura_theme
                else ["#3a6b47", "#528c60", "#76ab82", "#9ec4a5"]
            )
            cluster_centers = [
                (root_x - scale * 0.16, root_y - tree_h * 0.68, scale * 0.14),
                (root_x + scale * 0.18, root_y - tree_h * 0.74, scale * 0.15),
                (root_x - scale * 0.06, top_y - scale * 0.05, scale * 0.17),
                (root_x + scale * 0.09, root_y - tree_h * 0.58, scale * 0.13),
                (root_x - scale * 0.24, root_y - tree_h * 0.52, scale * 0.11),
                (root_x + scale * 0.25, root_y - tree_h * 0.60, scale * 0.12),
                (root_x - scale * 0.02, root_y - tree_h * 0.85, scale * 0.15),
            ]

            # 1. 奥の影塊 (Deep Shadow Canopy Mass)
            for c_i, (cc_x, cc_y, cc_r) in enumerate(cluster_centers[:4]):
                if len(strokes) >= limit:
                    break
                c_pts = [
                    (
                        cc_x + math.cos(deg) * cc_r * (0.9 + rng.uniform(-0.06, 0.06)),
                        cc_y + math.sin(deg) * cc_r * (0.9 + rng.uniform(-0.06, 0.06)),
                    )
                    for deg in (0.0, math.pi * 0.5, math.pi, math.pi * 1.5, math.pi * 2.0)
                ]
                strokes.append(
                    _create_macro_stroke(
                        stroke_id=uid("tree_foliage_shadow", c_i),
                        points=[(x, y, 0.70) for x, y in _catmull_rom_spline(c_pts, 6)],
                        profile="watercolor",
                        color=deep_shadow_col,
                        size_px=max(20.0, cc_r * 0.75),
                        layer_name="Flats",
                        opacity=0.60,
                    )
                )

            # 2. 中景〜前景の花房クラスタ (Midtone & Highlights Canopy)
            for c_i, (cc_x, cc_y, cc_r) in enumerate(cluster_centers):
                if len(strokes) >= limit:
                    break
                col = blossom_colors[c_i % len(blossom_colors)]
                c_pts = [
                    (
                        cc_x + math.cos(deg) * cc_r * (1.0 + rng.uniform(-0.08, 0.08)),
                        cc_y + math.sin(deg) * cc_r * (1.0 + rng.uniform(-0.08, 0.08)),
                    )
                    for deg in (0.0, math.pi * 0.5, math.pi, math.pi * 1.5, math.pi * 2.0)
                ]
                strokes.append(
                    _create_macro_stroke(
                        stroke_id=uid("tree_foliage", c_i),
                        points=[(x, y, 0.85) for x, y in _catmull_rom_spline(c_pts, 8)],
                        profile="watercolor",
                        color=col,
                        size_px=max(18.0, cc_r * 0.65),
                        layer_name="Flats",
                        opacity=0.72,
                    )
                )

            # 3. 外周に舞い散る花びら (Drifting Petals / Highlights)
            if is_sakura_theme and len(strokes) < limit:
                for p_i in range(8):
                    if len(strokes) >= limit:
                        break
                    px = root_x + rng.uniform(-scale * 0.28, scale * 0.32)
                    py = root_y - tree_h * rng.uniform(0.2, 0.95)
                    drift_pts = [(px, py), (px + scale * rng.uniform(0.01, 0.03), py + scale * rng.uniform(0.01, 0.02))]
                    strokes.append(
                        _create_macro_stroke(
                            stroke_id=uid("falling_petal", p_i),
                            points=[(x, y, 0.8) for x, y in drift_pts],
                            profile="gpen",
                            color="#fff0f5" if p_i % 2 == 0 else "#ffc2d6",
                            size_px=max(2.5, scale * 0.004),
                            layer_name="Highlights",
                            opacity=0.88,
                        )
                    )

        return strokes

    # -------------------------------------------------------------------------
    # 3. 山岳・山並み (Mountain Range / Peaks)
    # -------------------------------------------------------------------------
    if any(k in name for k in ("mountain", "peak", "ridge", "hill")):
        base_y = cy if operation.center[1] != 0.5 else h * 0.58
        m_colors = list(operation.colors) if operation.colors else ["#6f829d", "#4a5568", "#283e50"]
        layers = min(3, len(m_colors))
        for m_layer in range(layers):
            if len(strokes) >= limit:
                break
            layer_base_y = base_y + m_layer * scale * 0.07
            steps = 14
            m_pts = [(0.0, layer_base_y)]
            for s_i in range(1, steps):
                sx = (s_i / steps) * w
                sy = layer_base_y - (rng.uniform(0.08, 0.22) * scale) / (m_layer + 1)
                m_pts.append((sx, sy))
            m_pts.append((w, layer_base_y))
            m_spline = _catmull_rom_spline(m_pts, 10)

            # 山体の面塗り (Flats) - 稜線直下・中腹・下部の3段で白抜けを防ぐ
            col = m_colors[m_layer % len(m_colors)]
            fill_bands = [0.0, 0.35, 0.70]
            for fb_idx, fb_frac in enumerate(fill_bands):
                if len(strokes) >= limit:
                    break
                band_pts = [(x, y + (layer_base_y - y) * fb_frac, 0.85) for x, y in m_spline]
                strokes.append(
                    _create_macro_stroke(
                        stroke_id=uid(f"mountain_mass_{m_layer}", fb_idx),
                        points=band_pts,
                        profile="watercolor",
                        color=col,
                        size_px=max(28.0, scale * 0.09),
                        layer_name="Flats",
                        opacity=0.82 if fb_idx == 0 else 0.70,
                    )
                )

            # 山稜線の輪郭 (Lineart)
            strokes.append(
                _create_macro_stroke(
                    stroke_id=uid("mountain_ridge", m_layer),
                    points=[(x, y, 0.75) for x, y in m_spline],
                    profile="gpen",
                    color="#1a2733",
                    size_px=max(2.0, scale * 0.0035 - m_layer * 0.5),
                    layer_name="Lineart",
                    opacity=0.85,
                )
            )

        return strokes

    # -------------------------------------------------------------------------
    # 4. 水彩ウォッシュ (Watercolor Wash / Sky / Ground)
    # -------------------------------------------------------------------------
    if any(k in name for k in ("wash", "watercolor_wash", "sky", "ground", "horizon")):
        x0, y0, x1, y1 = (
            operation.bounds[0] * w,
            operation.bounds[1] * h,
            operation.bounds[2] * w,
            operation.bounds[3] * h,
        )
        wash_colors = list(operation.colors) if operation.colors else ["#2b5c8f", "#5c93cf", "#b8d8f8", "#eef6ff"]
        total_h = max(1.0, y1 - y0)
        # キャンバス白抜けを防ぐため、密な段数と十分なオーバーラップを持たせる
        rows = max(4, min(limit, max(len(wash_colors), int(total_h / max(20.0, scale * 0.08)))))
        band_thickness = max(35.0, (total_h / max(1, rows - 1)) * 1.6)
        for r_i in range(rows):
            if len(strokes) >= limit:
                break
            t_row = r_i / max(1, rows - 1)
            curr_y = y0 + t_row * total_h
            c_col = wash_colors[min(len(wash_colors) - 1, int(t_row * len(wash_colors)))]
            w_pts = _catmull_rom_spline(
                [
                    (x0, curr_y),
                    (x0 + (x1 - x0) * 0.33, curr_y + scale * rng.uniform(-0.015, 0.015)),
                    (x0 + (x1 - x0) * 0.67, curr_y + scale * rng.uniform(-0.015, 0.015)),
                    (x1, curr_y),
                ],
                8,
            )
            strokes.append(
                _create_macro_stroke(
                    stroke_id=uid("wash_band", r_i),
                    points=[(x, y, 0.85) for x, y in w_pts],
                    profile="watercolor",
                    color=c_col,
                    size_px=band_thickness,
                    layer_name="Flats",
                    opacity=0.75,
                )
            )
        return strokes

    # -------------------------------------------------------------------------
    # 5. 雲・積乱雲・もくもく雲 (Cloud Cluster / Cumulus Clouds)
    # -------------------------------------------------------------------------
    if any(k in name for k in ("cloud", "cumulus", "sky_cloud")):
        cloud_colors = list(operation.colors) if operation.colors else ["#8ca6c7", "#bfd4ea", "#f0f5fb", "#ffffff"]
        c_shadow = cloud_colors[0]
        c_mid = cloud_colors[1] if len(cloud_colors) > 1 else "#e0ecf8"
        c_body = cloud_colors[2] if len(cloud_colors) > 2 else "#ffffff"
        c_hl = cloud_colors[-1]

        # 雲を構成する球体クラスタのオフセット定義 (底面フラット・上部ふんわり)
        puffs = [
            (-0.35, 0.05, 0.45),
            (-0.15, -0.15, 0.60),
            (0.12, -0.22, 0.65),
            (0.38, -0.05, 0.48),
            (0.00, 0.02, 0.55),
            (-0.25, -0.05, 0.50),
            (0.25, -0.12, 0.52),
        ]

        # 1. 雲底のシャドウ層 (Cloud Bottom Shadow - Flats/Shading)
        shadow_pts: list[tuple[float, float]] = []
        for p_ox, p_oy, _p_r in puffs:
            px = cx + p_ox * r
            py = cy + (p_oy + 0.12) * r
            shadow_pts.append((px, py))
        shadow_pts.sort(key=lambda pt: pt[0])
        s_spline = _catmull_rom_spline(shadow_pts, 6)
        strokes.append(
            _create_macro_stroke(
                stroke_id=uid("cloud_shadow_base"),
                points=[(x, y, 0.75) for x, y in s_spline],
                profile="watercolor",
                color=c_shadow,
                size_px=max(25.0, r * 0.55),
                layer_name="Shading",
                opacity=0.65,
            )
        )

        # 2. 雲のふんわり本体ボリューム (Flats)
        for idx, (p_ox, p_oy, p_r) in enumerate(puffs):
            if len(strokes) >= limit:
                break
            px = cx + p_ox * r
            py = cy + p_oy * r
            puff_radius = p_r * r
            circ_pts = [
                (
                    px + math.cos(deg) * puff_radius * (1.0 + rng.uniform(-0.06, 0.06)),
                    py + math.sin(deg) * puff_radius * (1.0 + rng.uniform(-0.06, 0.06)),
                )
                for deg in (0.0, math.pi * 0.5, math.pi, math.pi * 1.5, math.pi * 2.0)
            ]
            c_spline = _catmull_rom_spline(circ_pts, 6)
            # 中間トーン
            strokes.append(
                _create_macro_stroke(
                    stroke_id=uid("cloud_body_mid", idx),
                    points=[(x, y, 0.85) for x, y in c_spline],
                    profile="watercolor",
                    color=c_mid,
                    size_px=max(20.0, puff_radius * 0.70),
                    layer_name="Flats",
                    opacity=0.75,
                )
            )
            # 明るいホワイト本体
            strokes.append(
                _create_macro_stroke(
                    stroke_id=uid("cloud_body_bright", idx),
                    points=[(x, y - puff_radius * 0.15, 0.90) for x, y in c_spline],
                    profile="watercolor",
                    color=c_body,
                    size_px=max(16.0, puff_radius * 0.58),
                    layer_name="Flats",
                    opacity=0.88,
                )
            )

        # 3. 雲頂の輝くリムハイライト (Highlights)
        top_puffs = sorted(puffs[:4], key=lambda p: p[0])
        hl_pts: list[tuple[float, float]] = []
        for p_ox, p_oy, p_r in top_puffs:
            px = cx + p_ox * r
            py = cy + (p_oy - p_r * 0.85) * r
            hl_pts.append((px, py))
        if len(hl_pts) >= 2 and len(strokes) < limit:
            hl_spline = _catmull_rom_spline(hl_pts, 8)
            strokes.append(
                _create_macro_stroke(
                    stroke_id=uid("cloud_rim_highlight"),
                    points=[
                        (x, y, 0.4 + 0.55 * math.sin(i / max(1, len(hl_spline) - 1) * math.pi))
                        for i, (x, y) in enumerate(hl_spline)
                    ],
                    profile="gpen",
                    color=c_hl,
                    size_px=max(3.0, scale * 0.0045),
                    layer_name="Highlights",
                    opacity=0.92,
                )
            )

        return strokes

    # -------------------------------------------------------------------------
    # 6. アニメ・キャラクター顔 / ポートレート (Character Face / Anime Portrait)
    # -------------------------------------------------------------------------
    if any(k in name for k in ("character", "portrait", "face", "girl", "boy", "anime_face")):
        from .procedural.character import generate_character_strokes

        # プロンプト、マクロ名、マクロパラメータを統合して属性を完全伝達
        params_str = " ".join(f"{k} {v}" for k, v in operation.params.items() if isinstance(v, (str, int, float, bool)))
        combined_prompt = f"{program.prompt} {operation.name} {params_str}".strip()
        pal = program.metadata.get("palette", "anime")

        raw_char_strokes = generate_character_strokes(
            prompt=combined_prompt,
            seed=program.seed + (operation_hash % 1000),
            count=min(limit, 150),
            width=w,
            height=h,
            palette_name=pal,
        )

        # マクロの center / radius に基づくスマート座標配置（Shift & Scale）
        std_cx = w * 0.5
        std_cy = h * 0.47
        std_scale = min(w, h) * 0.85
        target_scale = operation.radius * 2.0 * min(w, h)
        scale_ratio = target_scale / max(1.0, std_scale) if operation.radius != 0.2 else 1.0
        target_cx = cx
        target_cy = cy

        need_transform = (
            abs(operation.center[0] - 0.5) > 0.01
            or abs(operation.center[1] - 0.5) > 0.01
            or abs(operation.radius - 0.2) > 0.01
        )
        if need_transform:
            transformed: list[Stroke] = []
            for st in raw_char_strokes:
                new_points = [
                    StrokePoint(
                        x=target_cx + (pt.x - std_cx) * scale_ratio,
                        y=target_cy + (pt.y - std_cy) * scale_ratio,
                        pressure=pt.pressure,
                        time_ms=pt.time_ms,
                    )
                    for pt in st.points
                ]
                transformed.append(
                    replace(
                        st,
                        points=tuple(new_points),
                        size_px=max(1.0, st.size_px * scale_ratio),
                    )
                )
            return transformed
        return raw_char_strokes

    # -------------------------------------------------------------------------
    # 6.5. 衣服のシワ・落ち影 (Clothing Folds / Drapery AO)
    # -------------------------------------------------------------------------
    if any(k in name for k in ("fold", "crease", "clothing", "drapery")):
        fold_col = operation.colors[0] if operation.colors else "#1f2233"
        f_strokes: list[Stroke] = []
        for f_idx in range(min(limit, 8)):
            ang_fold = math.radians(f_idx * 45.0 + rng.uniform(-10.0, 10.0))
            f_len = r * (0.4 + rng.uniform(0.0, 0.4))
            p_start = (cx + math.cos(ang_fold) * r * 0.2, cy + math.sin(ang_fold) * r * 0.2)
            p_mid = (
                p_start[0] + math.cos(ang_fold + 0.3) * f_len * 0.5,
                p_start[1] + math.sin(ang_fold + 0.3) * f_len * 0.5,
            )
            p_end = (p_start[0] + math.cos(ang_fold) * f_len, p_start[1] + math.sin(ang_fold) * f_len)
            f_pts = _catmull_rom_spline([p_start, p_mid, p_end], 6)
            f_strokes.append(
                _create_macro_stroke(
                    stroke_id=uid("fold", f_idx),
                    points=[(x, y, 0.85) for x, y in f_pts],
                    profile="gpen",
                    color=fold_col,
                    size_px=max(2.0, scale * 0.004),
                    layer_name="Shading",
                    opacity=0.65,
                )
            )
        return f_strokes

    # -------------------------------------------------------------------------
    # 7. 幾何学・都市・魔法陣 (Magic Circle / Cyber City)
    # -------------------------------------------------------------------------
    if any(k in name for k in ("magic_circle", "rune", "magic")):
        from .procedural.manga_fx import generate_manga_fx_strokes

        raw_strokes = generate_manga_fx_strokes(
            prompt="magic circle",
            seed=program.seed,
            count=min(limit, 80),
            width=w,
            height=h,
        )
        return [replace(s, id=_operation_uuid(program, operation.id, idx)) for idx, s in enumerate(raw_strokes)]

    if any(k in name for k in ("city", "cyber", "skyline", "building")):
        from .procedural.geometry import generate_geometry_strokes

        raw_strokes = generate_geometry_strokes(
            prompt="cyberpunk city",
            seed=program.seed,
            count=min(limit, 100),
            width=w,
            height=h,
        )
        return [replace(s, id=_operation_uuid(program, operation.id, idx)) for idx, s in enumerate(raw_strokes)]

    if not strokes and limit > 0:
        ring_segments = 16
        pts_fallback = [
            (
                cx + math.cos(i / ring_segments * math.tau) * r,
                cy + math.sin(i / ring_segments * math.tau) * r,
                0.75,
            )
            for i in range(ring_segments + 1)
        ]
        strokes.append(
            _create_macro_stroke(
                stroke_id=uid("fallback_accent", 0),
                points=pts_fallback,
                profile=operation.brush.profile,
                color=operation.brush.color,
                size_px=operation.brush.size_px(w, h),
                layer_name=operation.layer,
                opacity=operation.brush.opacity,
            )
        )
    return strokes


def _compile_gradient_fill(
    program: StrokeProgram,
    operation: GradientFillOperation,
    limit: int,
) -> list[Stroke]:
    """多色グラデーションスキャンライン塗り。"""
    w = program.canvas_width
    h = program.canvas_height
    poly = [(p.x * w, p.y * h) for p in operation.polygon]
    if len(poly) < 3:
        return []
    center = (sum(p[0] for p in poly) / len(poly), sum(p[1] for p in poly) / len(poly))
    angle = math.radians(operation.angle_deg)
    rotated = [_rotate(p, center, -angle) for p in poly]
    min_y = min(p[1] for p in rotated)
    max_y = max(p[1] for p in rotated)
    dy_total = max(1.0, max_y - min_y)
    spacing_scale = 0.55 if operation.style in {"wash", "linear"} else operation.spacing
    spacing = max(0.5, operation.brush.size_px(w, h) * spacing_scale)
    strokes: list[Stroke] = []
    row = 0
    y = min_y + spacing * 0.5
    while y < max_y and len(strokes) < limit:
        segments = _scanline_segments(rotated, y)
        if row % 2:
            segments.reverse()
        t_pos = (y - min_y) / dy_total
        cur_color = _multi_color_interpolate(operation.colors, t_pos)
        colored_brush = replace(operation.brush, color=cur_color)
        for start_x, end_x in segments:
            if len(strokes) >= limit:
                break
            first_x, second_x = (end_x, start_x) if row % 2 else (start_x, end_x)
            first_rot = _rotate((first_x, y), center, angle)
            second_rot = _rotate((second_x, y), center, angle)
            pts = [(first_rot[0], first_rot[1], 1.0), (second_rot[0], second_rot[1], 1.0)]
            strokes.append(_make_stroke(program, replace(operation, brush=colored_brush), len(strokes), pts))
        row += 1
        y += spacing

    if not strokes and limit > 0:
        mid_y = (min_y + max_y) * 0.5
        segments = _scanline_segments(rotated, mid_y)
        if not segments:
            min_x = min(p[0] for p in rotated)
            max_x = max(p[0] for p in rotated)
            if max_x - min_x >= 0.25:
                segments = [(min_x, max_x)]
            else:
                segments = [(min_x, max_x + 0.5)]
        for start_x, end_x in segments:
            if len(strokes) >= limit:
                break
            first_rot = _rotate((start_x, mid_y), center, angle)
            second_rot = _rotate((end_x, mid_y), center, angle)
            pts = [(first_rot[0], first_rot[1], 1.0), (second_rot[0], second_rot[1], 1.0)]
            t_pos = (mid_y - min_y) / dy_total
            cur_color = _multi_color_interpolate(operation.colors, t_pos)
            colored_brush = replace(operation.brush, color=cur_color)
            strokes.append(_make_stroke(program, replace(operation, brush=colored_brush), len(strokes), pts))

    return strokes


def compile_stroke_program(
    program: StrokeProgram,
    count: int | None = None,
    *,
    target_width: float | None = None,
    target_height: float | None = None,
) -> DrawingPlan:
    """高水準命令を、既存レンダラーと保存形式が扱える DrawingPlan へ変換する。"""
    if not isinstance(program, StrokeProgram):
        raise TypeError("program は StrokeProgram である必要があります")
    if target_width is not None or target_height is not None:
        tw = target_width if target_width is not None else program.canvas_width
        th = target_height if target_height is not None else program.canvas_height
        program = program.with_canvas_size(tw, th)
    if count is not None and (
        isinstance(count, bool) or not isinstance(count, int) or not 1 <= count <= MAX_PLAN_STROKES
    ):
        raise ValueError(f"count は1から{MAX_PLAN_STROKES}またはNoneである必要があります")
    operation_budget = max(1, math.ceil(MAX_PLAN_STROKES / len(program.operations)))
    fill_operations = [
        operation for operation in program.operations if isinstance(operation, (FillOperation, GradientFillOperation))
    ]
    fill_budgets: list[int]
    if count is None:
        fill_budgets = [operation_budget] * len(fill_operations)
    else:
        has_non_fill = len(fill_operations) < len(program.operations)
        reserved = count if not has_non_fill else min(count, max(len(fill_operations), round(count * 0.48)))
        fill_budgets = _allocate_fill_budgets(fill_operations, reserved)
        if len(fill_budgets) < len(fill_operations):
            fill_budgets = [max(1, reserved // len(fill_operations))] * len(fill_operations)

    fill_index = 0
    fill_strokes: list[Stroke] = []
    other_strokes: list[Stroke] = []
    for operation in program.operations:
        if isinstance(operation, FillOperation):
            budget = fill_budgets[fill_index] if fill_index < len(fill_budgets) else operation_budget
            compiled = _compile_fill_to_budget(program, operation, budget)
            fill_index += 1
            fill_strokes.extend(compiled)
        elif isinstance(operation, GradientFillOperation):
            budget = fill_budgets[fill_index] if fill_index < len(fill_budgets) else operation_budget
            compiled = _compile_gradient_fill(program, operation, budget)
            fill_index += 1
            fill_strokes.extend(compiled)
        elif isinstance(operation, RibbonOperation):
            other_strokes.extend(_compile_ribbon(program, operation, operation_budget))
        elif isinstance(operation, PathOperation):
            other_strokes.extend(_compile_path(program, operation))
        elif isinstance(operation, HatchOperation):
            other_strokes.extend(_compile_hatch(program, operation, operation_budget))
        elif isinstance(operation, ParticleOperation):
            other_strokes.extend(_compile_particles(program, operation, operation_budget))
        elif isinstance(operation, MacroOperation):
            other_strokes.extend(_compile_macro(program, operation, operation_budget))

    if count is None:
        strokes = [*fill_strokes, *other_strokes]
    else:
        remaining = max(0, count - len(fill_strokes))
        strokes = [*fill_strokes, *_sample_strokes_by_priority(other_strokes, remaining)]
    if len(strokes) > MAX_PLAN_STROKES:
        strokes = _sample_strokes_by_priority(strokes, MAX_PLAN_STROKES)
    if not strokes:
        raise PlanValidationError("StrokeProgram から有効なストロークを生成できませんでした")
    metadata = {
        **dict(program.metadata),
        "source_schema_version": PROGRAM_SCHEMA_VERSION,
        "operation_count": len(program.operations),
        "budget_strategy": "operation_aware_v1",
    }
    return DrawingPlan(
        prompt=program.prompt,
        seed=program.seed,
        strokes=strokes,
        title=program.title,
        iteration=program.iteration,
        metadata=metadata,
        canvas_width=program.canvas_width,
        canvas_height=program.canvas_height,
        goal_reached=program.goal_reached,
        completion_score=program.completion_score,
    )


def drawing_plan_to_stroke_program(plan: DrawingPlan) -> StrokeProgram:
    """v1 DrawingPlan を損失の少ない v2 path operation 群へ移行する。"""
    if not isinstance(plan, DrawingPlan):
        raise TypeError("plan は DrawingPlan である必要があります")
    if not plan.strokes:
        raise PlanValidationError("空の DrawingPlan は StrokeProgram へ移行できません")
    inferred_width = max((point.x for stroke in plan.strokes for point in stroke.points), default=1.5) + 0.5
    inferred_height = max((point.y for stroke in plan.strokes for point in stroke.points), default=1.5) + 0.5
    width = max(2.0, plan.canvas_width if plan.canvas_width is not None else inferred_width)
    height = max(2.0, plan.canvas_height if plan.canvas_height is not None else inferred_height)
    operations: list[ProgramOperation] = []
    for stroke in plan.strokes:
        points = tuple(
            ProgramPoint(
                max(0.0, min(1.0, point.x / width)),
                max(0.0, min(1.0, point.y / height)),
                point.pressure,
            )
            for point in stroke.points
        )
        operations.append(
            PathOperation(
                id=stroke.id,
                points=points,
                brush=ProgramBrush(
                    profile=infer_brush_profile(stroke.brush_preset, is_eraser=stroke.is_eraser),
                    preset_hint=stroke.brush_preset,
                    color=stroke.color,
                    size=stroke.size_px,
                    size_mode="px",
                    opacity=stroke.opacity,
                    is_eraser=stroke.is_eraser,
                ),
                layer=stroke.layer_name,
                smooth=False,
            )
        )
    return StrokeProgram(
        prompt=plan.prompt,
        seed=plan.seed,
        operations=operations,
        canvas_width=width,
        canvas_height=height,
        title=plan.title,
        iteration=plan.iteration,
        metadata={**dict(plan.metadata), "migrated_from_schema_version": 1},
        goal_reached=plan.goal_reached,
        completion_score=plan.completion_score,
    )
