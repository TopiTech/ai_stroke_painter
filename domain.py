"""Krita に依存しない、永続化・エクスポート可能な描画計画のドメインモデル。"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
import html
import math
import re
from typing import Any

SCHEMA_VERSION = 1
_COLOR_RE = re.compile(r"^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{4}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$")


class PlanValidationError(ValueError):
    """外部入力または保存済み計画が描画契約を満たさない場合の例外。"""


def _finite_number(value: Any, field_name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise PlanValidationError(f"{field_name} は有限の数値である必要があります")
    return float(value)


def _non_negative_int(value: Any, field_name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise PlanValidationError(f"{field_name} は 0 以上の整数である必要があります")
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

    def as_dict(self) -> dict[str, Any]:
        return {"x": self.x, "y": self.y, "pressure": self.pressure, "time_ms": self.time_ms}

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> StrokePoint:
        if not isinstance(value, Mapping):
            raise PlanValidationError("point はオブジェクトである必要があります")
        try:
            return cls(value["x"], value["y"], value["pressure"], value["time_ms"])
        except KeyError as exc:
            raise PlanValidationError(f"point に必須項目 {exc.args[0]} がありません") from exc


@dataclass(frozen=True)
class Stroke:
    id: str
    points: Sequence[StrokePoint]
    brush_preset: str = "Basic-5 Size"
    color: str = "#232323"
    size_px: float = 8.0
    layer_name: str = "Lineart"
    opacity: float = 1.0

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
            raise PlanValidationError("color は #RGB, #RGBA, #RRGGBB, #RRGGBBAA 形式である必要があります")
        size_px = _finite_number(self.size_px, "size_px")
        if size_px <= 0:
            raise PlanValidationError("size_px は正の数値である必要があります")
        object.__setattr__(self, "size_px", size_px)

        if not isinstance(self.layer_name, str) or not self.layer_name.strip():
            object.__setattr__(self, "layer_name", "Lineart")
        opacity = _finite_number(self.opacity, "opacity")
        if not 0.0 <= opacity <= 1.0:
            raise PlanValidationError("opacity は 0.0 から 1.0 の範囲である必要があります")
        object.__setattr__(self, "opacity", opacity)

    def as_dict(self) -> dict[str, Any]:
        return {
            "schema_version": SCHEMA_VERSION,
            "id": self.id,
            "points": [point.as_dict() for point in self.points],
            "brush_preset": self.brush_preset,
            "color": self.color,
            "size_px": self.size_px,
            "layer_name": self.layer_name,
            "opacity": self.opacity,
        }

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> Stroke:
        if not isinstance(value, Mapping):
            raise PlanValidationError("stroke はオブジェクトである必要があります")
        version = value.get("schema_version", SCHEMA_VERSION)
        if version != SCHEMA_VERSION:
            raise PlanValidationError(f"未対応の stroke schema_version: {version!r}")
        try:
            points = tuple(StrokePoint.from_dict(point) for point in value["points"])
            return cls(
                id=value["id"],
                points=points,
                brush_preset=value.get("brush_preset", "Basic-5 Size"),
                color=value.get("color", "#232323"),
                size_px=value.get("size_px", 8.0),
                layer_name=value.get("layer_name", "Lineart"),
                opacity=value.get("opacity", 1.0),
            )
        except KeyError as exc:
            raise PlanValidationError(f"stroke に必須項目 {exc.args[0]} がありません") from exc
        except TypeError as exc:
            raise PlanValidationError("stroke points は配列である必要があります") from exc


@dataclass(frozen=True)
class VisionCritique:
    """自律ビジョン改善ループにおける進捗評価と次の提案。"""

    evaluation: str
    completion_score: float  # 0.0 - 1.0
    suggested_action: str
    iteration: int

    def __post_init__(self) -> None:
        object.__setattr__(self, "completion_score", max(0.0, min(1.0, float(self.completion_score))))
        object.__setattr__(self, "iteration", _non_negative_int(self.iteration, "iteration"))

    def as_dict(self) -> dict[str, Any]:
        return {
            "evaluation": self.evaluation,
            "completion_score": self.completion_score,
            "suggested_action": self.suggested_action,
            "iteration": self.iteration,
        }

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> VisionCritique:
        return cls(
            evaluation=str(value.get("evaluation", "")),
            completion_score=float(value.get("completion_score", 0.0)),
            suggested_action=str(value.get("suggested_action", "")),
            iteration=int(value.get("iteration", 1)),
        )


@dataclass(frozen=True)
class DrawingPlan:
    prompt: str
    seed: int
    strokes: Sequence[Stroke]
    title: str = ""
    iteration: int = 1
    layers: Sequence[str] = field(default_factory=tuple)
    metadata: Mapping[str, Any] = field(default_factory=dict)

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
        object.__setattr__(self, "title", str(self.title))
        object.__setattr__(self, "iteration", _non_negative_int(self.iteration, "iteration"))

        # レイヤー一覧を自動推定または指定値で初期化
        if self.layers:
            object.__setattr__(self, "layers", tuple(str(x) for x in self.layers))
        else:
            inferred_layers: list[str] = []
            for stroke in strokes:
                if stroke.layer_name not in inferred_layers:
                    inferred_layers.append(stroke.layer_name)
            object.__setattr__(self, "layers", tuple(inferred_layers) if inferred_layers else ("Lineart",))

        object.__setattr__(self, "metadata", dict(self.metadata))

    def as_dict(self) -> dict[str, Any]:
        result: dict[str, Any] = {
            "schema_version": SCHEMA_VERSION,
            "prompt": self.prompt,
            "seed": self.seed,
            "strokes": [stroke.as_dict() for stroke in self.strokes],
            "title": self.title,
            "iteration": self.iteration,
            "layers": list(self.layers),
        }
        if self.metadata:
            result["metadata"] = dict(self.metadata)
        return result

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> DrawingPlan:
        if not isinstance(value, Mapping):
            raise PlanValidationError("drawing plan はオブジェクトである必要があります")
        version = value.get("schema_version", SCHEMA_VERSION)
        if version != SCHEMA_VERSION:
            raise PlanValidationError(f"未対応の plan schema_version: {version!r}")
        try:
            strokes = tuple(Stroke.from_dict(stroke) for stroke in value["strokes"])
            return cls(
                prompt=str(value.get("prompt", "")),
                seed=int(value.get("seed", 0)),
                strokes=strokes,
                title=str(value.get("title", "")),
                iteration=int(value.get("iteration", 1)),
                layers=value.get("layers", ()),
                metadata=value.get("metadata", {}),
            )
        except KeyError as exc:
            raise PlanValidationError(f"drawing plan に必須項目 {exc.args[0]} がありません") from exc
        except TypeError as exc:
            raise PlanValidationError("strokes は配列である必要があります") from exc

    def to_svg(self, width: float | None = None, height: float | None = None) -> str:
        """高品質な SVG ベクター形式として出力する。"""
        if width is None or height is None:
            max_x = max((p.x for s in self.strokes for p in s.points), default=800.0)
            max_y = max((p.y for s in self.strokes for p in s.points), default=800.0)
            w = max(100.0, max_x + 20.0)
            h = max(100.0, max_y + 20.0)
        else:
            w = float(width)
            h = float(height)

        # XML コメント内で "--" は禁止されているため置換する
        safe_prompt = html.escape(self.prompt).replace("--", "﹣﹣")
        svg_parts: list[str] = [
            f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w:.1f} {h:.1f}" width="{w:.1f}" height="{h:.1f}">',
            f"  <!-- AI Stroke Painter: {safe_prompt} (Seed: {self.seed}) -->",
            "  <defs><style>.stroke { stroke-linecap: round; stroke-linejoin: round; fill: none; }</style></defs>",
        ]

        # レイヤーごとにグループ化
        by_layer: dict[str, list[Stroke]] = {}
        for stroke in self.strokes:
            by_layer.setdefault(stroke.layer_name, []).append(stroke)

        ordered_layers = list(self.layers) if self.layers else list(by_layer.keys())
        for layer in by_layer:
            if layer not in ordered_layers:
                ordered_layers.append(layer)

        for layer_name in ordered_layers:
            strokes = by_layer.get(layer_name, [])
            if not strokes:
                continue
            svg_parts.append(f'  <g id="layer_{html.escape(layer_name)}">')
            for stroke in strokes:
                pts = stroke.points
                if len(pts) < 2:
                    continue
                # 可変筆圧をセグメントのストローク幅に反映
                for p0, p1 in zip(pts, pts[1:]):
                    avg_pressure = (p0.pressure + p1.pressure) * 0.5
                    stroke_w = max(0.5, stroke.size_px * avg_pressure)
                    opacity_attr = f' stroke-opacity="{stroke.opacity:.2f}"' if stroke.opacity < 1.0 else ""
                    svg_parts.append(
                        f'    <line x1="{p0.x:.2f}" y1="{p0.y:.2f}" x2="{p1.x:.2f}" y2="{p1.y:.2f}" '
                        f'stroke="{stroke.color}" stroke-width="{stroke_w:.2f}" class="stroke"{opacity_attr} />'
                    )
            svg_parts.append("  </g>")

        svg_parts.append("</svg>")
        return "\n".join(svg_parts)
