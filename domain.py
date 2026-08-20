"""Krita に依存しない、永続化可能な描画計画のドメインモデル。"""

from __future__ import annotations

from dataclasses import dataclass
import math
import re
from typing import Any, Dict, Mapping, Sequence, Tuple


SCHEMA_VERSION = 1
_COLOR_RE = re.compile(r"^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6})$")


class PlanValidationError(ValueError):
    """外部入力または保存済み計画が描画契約を満たさない場合の例外。"""


def _finite_number(value: Any, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise PlanValidationError("%s は有限の数値である必要があります" % field)
    return float(value)


def _non_negative_int(value: Any, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise PlanValidationError("%s は 0 以上の整数である必要があります" % field)
    return value


@dataclass(frozen=True)
class StrokePoint:
    x: float
    y: float
    pressure: float
    time_ms: int

    def __post_init__(self) -> None:
        object.__setattr__(self, "x", _finite_number(self.x, "x"))
        object.__setattr__(self, "y", _finite_number(self.y, "y"))
        pressure = _finite_number(self.pressure, "pressure")
        if not 0.0 <= pressure <= 1.0:
            raise PlanValidationError("pressure は 0.0 から 1.0 の範囲である必要があります")
        object.__setattr__(self, "pressure", pressure)
        object.__setattr__(self, "time_ms", _non_negative_int(self.time_ms, "time_ms"))

    def as_dict(self) -> Dict[str, Any]:
        return {"x": self.x, "y": self.y, "pressure": self.pressure, "time_ms": self.time_ms}

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "StrokePoint":
        if not isinstance(value, Mapping):
            raise PlanValidationError("point はオブジェクトである必要があります")
        try:
            return cls(value["x"], value["y"], value["pressure"], value["time_ms"])
        except KeyError as exc:
            raise PlanValidationError("point に必須項目 %s がありません" % exc.args[0]) from exc


@dataclass(frozen=True)
class Stroke:
    id: str
    points: Sequence[StrokePoint]
    brush_preset: str = "Basic-5 Size"
    color: str = "#232323"
    size_px: float = 8.0

    def __post_init__(self) -> None:
        if not isinstance(self.id, str) or not self.id.strip():
            raise PlanValidationError("stroke id は空でない文字列である必要があります")
        points = tuple(self.points)
        if len(points) < 2:
            raise PlanValidationError("stroke には少なくとも 2 点必要です")
        if any(not isinstance(point, StrokePoint) for point in points):
            raise PlanValidationError("stroke points は StrokePoint である必要があります")
        if any(b.time_ms < a.time_ms for a, b in zip(points, points[1:])):
            raise PlanValidationError("stroke points の time_ms は昇順である必要があります")
        object.__setattr__(self, "points", points)
        if not isinstance(self.brush_preset, str) or not self.brush_preset.strip():
            raise PlanValidationError("brush_preset は空でない文字列である必要があります")
        if not isinstance(self.color, str) or not _COLOR_RE.match(self.color):
            raise PlanValidationError("color は #RRGGBB または #RGB 形式である必要があります")
        size_px = _finite_number(self.size_px, "size_px")
        if size_px <= 0:
            raise PlanValidationError("size_px は正の数値である必要があります")
        object.__setattr__(self, "size_px", size_px)

    def as_dict(self) -> Dict[str, Any]:
        return {
            "schema_version": SCHEMA_VERSION,
            "id": self.id,
            "points": [point.as_dict() for point in self.points],
            "brush_preset": self.brush_preset,
            "color": self.color,
            "size_px": self.size_px,
        }

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "Stroke":
        if not isinstance(value, Mapping):
            raise PlanValidationError("stroke はオブジェクトである必要があります")
        version = value.get("schema_version", SCHEMA_VERSION)
        if version != SCHEMA_VERSION:
            raise PlanValidationError("未対応の stroke schema_version: %r" % version)
        try:
            points = tuple(StrokePoint.from_dict(point) for point in value["points"])
            return cls(
                id=value["id"],
                points=points,
                brush_preset=value.get("brush_preset", "Basic-5 Size"),
                color=value.get("color", "#232323"),
                size_px=value.get("size_px", 8.0),
            )
        except KeyError as exc:
            raise PlanValidationError("stroke に必須項目 %s がありません" % exc.args[0]) from exc
        except TypeError as exc:
            raise PlanValidationError("stroke points は配列である必要があります") from exc


@dataclass(frozen=True)
class DrawingPlan:
    prompt: str
    seed: int
    strokes: Sequence[Stroke]

    def __post_init__(self) -> None:
        if not isinstance(self.prompt, str):
            raise PlanValidationError("prompt は文字列である必要があります")
        object.__setattr__(self, "seed", _non_negative_int(self.seed, "seed"))
        strokes = tuple(self.strokes)
        if any(not isinstance(stroke, Stroke) for stroke in strokes):
            raise PlanValidationError("strokes は Stroke である必要があります")
        if len({stroke.id for stroke in strokes}) != len(strokes):
            raise PlanValidationError("stroke id は計画内で一意である必要があります")
        object.__setattr__(self, "strokes", strokes)

    def as_dict(self) -> Dict[str, Any]:
        return {
            "schema_version": SCHEMA_VERSION,
            "prompt": self.prompt,
            "seed": self.seed,
            "strokes": [stroke.as_dict() for stroke in self.strokes],
        }

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "DrawingPlan":
        if not isinstance(value, Mapping):
            raise PlanValidationError("drawing plan はオブジェクトである必要があります")
        if value.get("schema_version") != SCHEMA_VERSION:
            raise PlanValidationError("未対応の plan schema_version: %r" % value.get("schema_version"))
        try:
            strokes = tuple(Stroke.from_dict(stroke) for stroke in value["strokes"])
            return cls(prompt=value["prompt"], seed=value["seed"], strokes=strokes)
        except KeyError as exc:
            raise PlanValidationError("drawing plan に必須項目 %s がありません" % exc.args[0]) from exc
        except TypeError as exc:
            raise PlanValidationError("strokes は配列である必要があります") from exc
