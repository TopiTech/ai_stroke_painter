"""高水準の描画命令を検証し、既存 DrawingPlan へ決定論的にコンパイルする。"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
import math
import random
import re
from typing import Any, Literal, TypeAlias
import uuid

from .brushes import brush_preset_for_profile, canonical_brush_profile, infer_brush_profile
from .domain import MAX_PLAN_STROKES, DrawingPlan, PlanValidationError, Stroke, StrokePoint

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


@dataclass(frozen=True)
class FillOperation:
    id: str
    polygon: Sequence[ProgramPoint]
    brush: ProgramBrush = field(default_factory=lambda: ProgramBrush(profile="marker", size=0.035))
    layer: str = "Flats"
    spacing: float = 0.72
    style: str = "wash"
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
        }:
            raise PlanValidationError("fill style は wash, scanline, feathered, contour のいずれかである必要があります")
        object.__setattr__(self, "style", self.style.strip().lower())


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
        }:
            raise PlanValidationError(
                "particle shape は petal, line, sparkle, drift, bokeh のいずれかである必要があります"
            )
        object.__setattr__(self, "shape", self.shape.strip().lower())


ProgramOperation: TypeAlias = PathOperation | FillOperation | HatchOperation | ParticleOperation


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
            {"kind", "id", "points", "brush", "layer", "layer_name", "closed", "smooth"},
            "path operation",
        )
        return PathOperation(
            id=operation_id,
            points=_points_from(value.get("points", ()), "path points", canvas_w=canvas_w, canvas_h=canvas_h),
            brush=ProgramBrush.from_dict(value.get("brush")),
            layer=value.get("layer", value.get("layer_name", "Lineart")),
            closed=value.get("closed", False),
            smooth=value.get("smooth", True),
        )
    if kind == "fill":
        _reject_unknown_keys(
            value,
            {"kind", "id", "polygon", "points", "brush", "layer", "layer_name", "spacing", "style"},
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
            not isinstance(operation, (PathOperation, FillOperation, HatchOperation, ParticleOperation))
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
                )
            elif isinstance(operation, (FillOperation, HatchOperation)):
                item["polygon"] = [point.as_list() for point in operation.polygon]
                item["spacing"] = operation.spacing
                if isinstance(operation, FillOperation):
                    item["style"] = operation.style
                elif isinstance(operation, HatchOperation):
                    item["angle_deg"] = operation.angle_deg
                    item["cross"] = operation.cross
            else:
                item.update(
                    bounds=list(operation.bounds),
                    count=operation.count,
                    length=operation.length,
                    angle_deg=operation.angle_deg,
                    angle_jitter=operation.angle_jitter,
                    shape=operation.shape,
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


def _operation_uuid(program: StrokeProgram, operation_id: str, index: int) -> str:
    return str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/program/{program.seed}/{operation_id}/{index}"))


def _make_stroke(
    program: StrokeProgram,
    operation: ProgramOperation,
    index: int,
    points: Sequence[tuple[float, float, float]],
) -> Stroke:
    bounded: list[StrokePoint] = []
    for point_index, (x, y, pressure) in enumerate(points):
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
    return [_make_stroke(program, operation, 0, dense)]


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


def _compile_fill(program: StrokeProgram, operation: FillOperation, limit: int) -> list[Stroke]:
    polygon = [(point.x * program.canvas_width, point.y * program.canvas_height) for point in operation.polygon]
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
                p1_x = first_x + (second_x - first_x) * 0.12
                p2_x = second_x - (second_x - first_x) * 0.12
                pts = [
                    (first_x, y, 0.45),
                    (p1_x, y, 0.95),
                    (p2_x, y, 0.95),
                    (second_x, y, 0.45),
                ]
            else:
                pts = [(first_x, y, 1.0), (second_x, y, 1.0)]
            strokes.append(_make_stroke(program, operation, len(strokes), pts))
        row += 1
        y += spacing
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
            pts = [
                (start_x, start_y, 0.15),
                (mid_x, mid_y, 1.0),
                (end_x, end_y, 0.15),
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


def compile_stroke_program(program: StrokeProgram, count: int | None = None) -> DrawingPlan:
    """高水準命令を、既存レンダラーと保存形式が扱える DrawingPlan へ変換する。"""
    if not isinstance(program, StrokeProgram):
        raise TypeError("program は StrokeProgram である必要があります")
    if count is not None and (
        isinstance(count, bool) or not isinstance(count, int) or not 1 <= count <= MAX_PLAN_STROKES
    ):
        raise ValueError(f"count は1から{MAX_PLAN_STROKES}またはNoneである必要があります")
    strokes: list[Stroke] = []
    operation_budget = max(1, math.ceil(MAX_PLAN_STROKES / len(program.operations)))
    for operation in program.operations:
        if isinstance(operation, PathOperation):
            compiled = _compile_path(program, operation)
        elif isinstance(operation, FillOperation):
            compiled = _compile_fill(program, operation, operation_budget)
        elif isinstance(operation, HatchOperation):
            compiled = _compile_hatch(program, operation, operation_budget)
        else:
            compiled = _compile_particles(program, operation, operation_budget)
        strokes.extend(compiled)
    if len(strokes) > MAX_PLAN_STROKES:
        strokes = _sample_strokes_by_priority(strokes, MAX_PLAN_STROKES)
    if count is not None:
        strokes = _sample_strokes_by_priority(strokes, count)
    if not strokes:
        raise PlanValidationError("StrokeProgram から有効なストロークを生成できませんでした")
    metadata = {
        **dict(program.metadata),
        "source_schema_version": PROGRAM_SCHEMA_VERSION,
        "operation_count": len(program.operations),
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
