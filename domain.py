"""Krita に依存しない、永続化・エクスポート可能な描画計画のドメインモデル。"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
import html
import math
import re
from typing import Any

SCHEMA_VERSION = 1
MAX_PLAN_STROKES = 2_000
MAX_STROKE_POINTS = 1_000
LAYER_RENDER_ORDER: dict[str, int] = {
    "Draft": 10,
    "Flats": 20,
    "Shading": 30,
    "Lineart": 40,
    "Highlights": 50,
    "FX": 60,
}
_COLOR_RE = re.compile(r"^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{4}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$")


class PlanValidationError(ValueError):
    """外部入力または保存済み計画が描画契約を満たさない場合の例外。"""


def _xml_safe_text(value: str) -> str:
    """XML 1.0 で許可される文字だけを残す。"""
    return "".join(
        character
        for character in value
        if character in "\t\n\r"
        or 0x20 <= ord(character) <= 0xD7FF
        or 0xE000 <= ord(character) <= 0xFFFD
        or ord(character) >= 0x10000
    )


def split_color_alpha(color: str) -> tuple[str, float]:
    """Validated CSS-style hex colorを RGB 部分と独立した alpha に分ける。"""
    if len(color) == 5:
        return color[:4], int(color[4] * 2, 16) / 255.0
    if len(color) == 9:
        return color[:7], int(color[7:9], 16) / 255.0
    return color, 1.0


def _stroke_svg_lines(stroke: Stroke, *, mask: bool = False) -> list[str]:
    lines: list[str] = []
    for first, second in zip(stroke.points, stroke.points[1:], strict=False):
        average_pressure = (first.pressure + second.pressure) * 0.5
        stroke_width = max(0.5, stroke.size_px * average_pressure)
        if mask:
            color = "#000000"
            _rgb_color, color_alpha = split_color_alpha(stroke.color)
            combined_opacity = stroke.opacity * color_alpha
            css_class = "stroke eraser"
        else:
            color, color_alpha = split_color_alpha(stroke.color)
            combined_opacity = stroke.opacity * color_alpha
            css_class = "stroke"
        opacity = f' stroke-opacity="{combined_opacity:.2f}"' if combined_opacity < 1.0 else ""
        lines.append(
            f'<line x1="{first.x:.2f}" y1="{first.y:.2f}" x2="{second.x:.2f}" y2="{second.y:.2f}" '
            f'stroke="{color}" stroke-width="{stroke_width:.2f}" class="{css_class}"{opacity} />'
        )
    return lines


def _finite_number(value: Any, field_name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise PlanValidationError(f"{field_name} は有限の数値である必要があります")
    return float(value)


def _non_negative_int(value: Any, field_name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise PlanValidationError(f"{field_name} は 0 以上の整数である必要があります")
    return value


def _positive_int(value: Any, field_name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 1:
        raise PlanValidationError(f"{field_name} は 1 以上の整数である必要があります")
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
    def from_dict(cls, value: Any) -> StrokePoint:
        if isinstance(value, (list, tuple)):
            if len(value) < 2:
                raise PlanValidationError("point 配列には少なくとも [x, y] の 2 要素が必要です")
            x = value[0]
            y = value[1]
            pressure = value[2] if len(value) >= 3 else 0.8
            time_ms = value[3] if len(value) >= 4 else 0
            return cls(x=x, y=y, pressure=pressure, time_ms=time_ms)

        if not isinstance(value, Mapping):
            raise PlanValidationError("point はオブジェクトまたは配列である必要があります")
        try:
            x = value["x"]
            y = value["y"]
            pressure = value.get("pressure", 0.8)
            time_ms = value.get("time_ms", 0)
            return cls(x=x, y=y, pressure=pressure, time_ms=time_ms)
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
    is_eraser: bool = False

    def __post_init__(self) -> None:
        if not isinstance(self.id, str) or not self.id.strip():
            raise PlanValidationError("stroke id は空でない文字列である必要があります")
        raw_points: Any = self.points
        if isinstance(raw_points, (str, bytes)) or not isinstance(raw_points, Sequence):
            raise PlanValidationError("stroke points は StrokePoint の配列である必要があります")
        points = tuple(raw_points)
        if len(points) < 2:
            raise PlanValidationError("stroke には少なくとも 2 点必要です")
        if len(points) > MAX_STROKE_POINTS:
            raise PlanValidationError(f"stroke points は {MAX_STROKE_POINTS} 点以下である必要があります")
        if any(not isinstance(point, StrokePoint) for point in points):
            raise PlanValidationError("stroke points は StrokePoint である必要があります")
        if any(b.time_ms < a.time_ms for a, b in zip(points, points[1:], strict=False)):
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
            raise PlanValidationError("layer_name は空でない文字列である必要があります")
        opacity = _finite_number(self.opacity, "opacity")
        if not 0.0 <= opacity <= 1.0:
            raise PlanValidationError("opacity は 0.0 から 1.0 の範囲である必要があります")
        object.__setattr__(self, "opacity", opacity)
        if not isinstance(self.is_eraser, bool):
            raise PlanValidationError("is_eraser は真偽値である必要があります")
        object.__setattr__(self, "is_eraser", self.is_eraser)

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
            "is_eraser": self.is_eraser,
        }

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> Stroke:
        if not isinstance(value, Mapping):
            raise PlanValidationError("stroke はオブジェクトである必要があります")
        version = value.get("schema_version", SCHEMA_VERSION)
        if version != SCHEMA_VERSION:
            raise PlanValidationError(f"未対応の stroke schema_version: {version!r}")
        try:
            raw_points = value["points"]
            if not isinstance(raw_points, Sequence) or isinstance(raw_points, (str, bytes)):
                raise PlanValidationError("stroke points は配列である必要があります")
            if len(raw_points) > MAX_STROKE_POINTS:
                raise PlanValidationError(f"stroke points は {MAX_STROKE_POINTS} 点以下である必要があります")
            points_list: list[StrokePoint] = []
            curr_time = 0
            for idx, point in enumerate(raw_points):
                sp = StrokePoint.from_dict(point)
                t = max(curr_time, sp.time_ms) if (sp.time_ms > 0 or idx > 0) else 0
                if idx > 0 and t == curr_time and sp.time_ms == 0:
                    t = curr_time + 10
                curr_time = t
                points_list.append(StrokePoint(x=sp.x, y=sp.y, pressure=sp.pressure, time_ms=curr_time))
            preset_name = str(value.get("brush_preset") or "Basic-5 Size")
            # 外部 JSON の "false" のような非真偽値を truthy と解釈すると、
            # 通常ストロークを意図せず消しゴムとして描画してしまう。
            is_eraser_val = (
                value.get("is_eraser", False) is True
                or "eraser" in preset_name.lower()
                or str(value.get("layer_name", "")).lower() == "eraser"
            )
            return cls(
                id=value["id"],
                points=tuple(points_list),
                brush_preset=preset_name,
                color=value.get("color", "#232323"),
                size_px=value.get("size_px", 8.0),
                layer_name=value.get("layer_name", "Lineart"),
                opacity=value.get("opacity", 1.0),
                is_eraser=is_eraser_val,
            )
        except KeyError as exc:
            raise PlanValidationError(f"stroke に必須項目 {exc.args[0]} がありません") from exc
        except PlanValidationError:
            raise
        except (TypeError, ValueError) as exc:
            raise PlanValidationError(f"stroke の値が不正です: {exc}") from exc


@dataclass(frozen=True)
class VisionCritique:
    """自律ビジョン改善ループにおける進捗評価と次の提案。"""

    evaluation: str
    completion_score: float  # 0.0 - 1.0
    suggested_action: str
    iteration: int
    goal_reached: bool = False

    def __post_init__(self) -> None:
        if not isinstance(self.evaluation, str) or not isinstance(self.suggested_action, str):
            raise PlanValidationError("VisionCritique の評価と提案は文字列である必要があります")
        score = _finite_number(self.completion_score, "completion_score")
        object.__setattr__(self, "completion_score", max(0.0, min(1.0, score)))
        object.__setattr__(self, "iteration", _positive_int(self.iteration, "iteration"))
        if not isinstance(self.goal_reached, bool):
            raise PlanValidationError("goal_reached は真偽値である必要があります")
        object.__setattr__(self, "goal_reached", self.goal_reached)

    def as_dict(self) -> dict[str, Any]:
        return {
            "evaluation": self.evaluation,
            "completion_score": self.completion_score,
            "suggested_action": self.suggested_action,
            "iteration": self.iteration,
            "goal_reached": self.goal_reached,
        }

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> VisionCritique:
        if not isinstance(value, Mapping):
            raise PlanValidationError("VisionCritique はオブジェクトである必要があります")
        try:
            return cls(
                evaluation=value.get("evaluation", ""),
                completion_score=value.get("completion_score", 0.0),
                suggested_action=value.get("suggested_action", ""),
                iteration=value.get("iteration", 1),
                goal_reached=value.get("goal_reached", False) is True,
            )
        except (TypeError, ValueError) as exc:
            raise PlanValidationError(f"VisionCritique の値が不正です: {exc}") from exc


@dataclass(frozen=True)
class DrawingPlan:
    prompt: str
    seed: int
    strokes: Sequence[Stroke]
    title: str = ""
    iteration: int = 1
    layers: Sequence[str] = field(default_factory=tuple)
    request_canvas_image: bool = False
    metadata: Mapping[str, Any] = field(default_factory=dict)
    canvas_width: float | None = None
    canvas_height: float | None = None
    goal_reached: bool = False
    # 未指定の外部計画を完成扱いしない。完成度は Planner/Critic が明示する。
    completion_score: float = 0.0

    def __post_init__(self) -> None:
        if not isinstance(self.prompt, str):
            raise PlanValidationError("prompt は文字列である必要があります")
        object.__setattr__(self, "seed", _non_negative_int(self.seed, "seed"))
        raw_strokes: Any = self.strokes
        if isinstance(raw_strokes, (str, bytes)) or not isinstance(raw_strokes, Sequence):
            raise PlanValidationError("strokes は Stroke の配列である必要があります")
        strokes = tuple(raw_strokes)
        if len(strokes) > MAX_PLAN_STROKES:
            raise PlanValidationError(f"strokes は {MAX_PLAN_STROKES} 本以下である必要があります")
        if any(not isinstance(stroke, Stroke) for stroke in strokes):
            raise PlanValidationError("strokes は Stroke である必要があります")
        if len({stroke.id for stroke in strokes}) != len(strokes):
            raise PlanValidationError("stroke id は計画内で一意である必要があります")
        object.__setattr__(self, "strokes", strokes)
        if not isinstance(self.title, str):
            raise PlanValidationError("title は文字列である必要があります")
        object.__setattr__(self, "iteration", _positive_int(self.iteration, "iteration"))
        if not isinstance(self.request_canvas_image, bool):
            raise PlanValidationError("request_canvas_image は真偽値である必要があります")
        if not isinstance(self.goal_reached, bool):
            raise PlanValidationError("goal_reached は真偽値である必要があります")
        comp_score = _finite_number(self.completion_score, "completion_score")
        object.__setattr__(self, "completion_score", max(0.0, min(1.0, comp_score)))
        for field_name in ("canvas_width", "canvas_height"):
            value = getattr(self, field_name)
            if value is None:
                continue
            dimension = _finite_number(value, field_name)
            if dimension <= 0:
                raise PlanValidationError(f"{field_name} は正の有限数値である必要があります")
            object.__setattr__(self, field_name, dimension)

        # レイヤー一覧を自動推定または指定値で初期化
        if self.layers:
            if isinstance(self.layers, (str, bytes)) or not isinstance(self.layers, Sequence):
                raise PlanValidationError("layers は文字列の配列である必要があります")
            normalized_layers: list[str] = []
            for layer in self.layers:
                if not isinstance(layer, str) or not layer.strip():
                    raise PlanValidationError("layers の各要素は空でない文字列である必要があります")
                if layer not in normalized_layers:
                    normalized_layers.append(layer)
            object.__setattr__(self, "layers", tuple(normalized_layers))
        else:
            inferred_layers: list[str] = []
            for stroke in strokes:
                if stroke.layer_name not in inferred_layers:
                    inferred_layers.append(stroke.layer_name)
            object.__setattr__(self, "layers", tuple(inferred_layers) if inferred_layers else ("Lineart",))

        if not isinstance(self.metadata, Mapping):
            raise PlanValidationError("metadata はオブジェクトである必要があります")
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
            "request_canvas_image": self.request_canvas_image,
            "goal_reached": self.goal_reached,
            "completion_score": self.completion_score,
        }
        if self.canvas_width is not None:
            result["canvas_width"] = self.canvas_width
        if self.canvas_height is not None:
            result["canvas_height"] = self.canvas_height
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
            raw_strokes = value["strokes"]
            if not isinstance(raw_strokes, Sequence) or isinstance(raw_strokes, (str, bytes)):
                raise PlanValidationError("strokes は配列である必要があります")
            if len(raw_strokes) > MAX_PLAN_STROKES:
                raise PlanValidationError(f"strokes は {MAX_PLAN_STROKES} 本以下である必要があります")
            strokes = tuple(Stroke.from_dict(stroke) for stroke in raw_strokes)
            metadata_val = dict(value.get("metadata", {})) if isinstance(value.get("metadata"), Mapping) else {}
            goal_reached_val = (
                value.get("goal_reached", False) is True or metadata_val.get("goal_reached", False) is True
            )
            completion_score_val = float(
                value.get(
                    "completion_score",
                    metadata_val.get("completion_score", 0.0),
                )
            )
            return cls(
                prompt=value.get("prompt", ""),
                seed=value.get("seed", 0),
                strokes=strokes,
                title=value.get("title", ""),
                iteration=value.get("iteration", 1),
                layers=value.get("layers", ()),
                request_canvas_image=value.get("request_canvas_image", False),
                metadata=metadata_val,
                canvas_width=value.get("canvas_width"),
                canvas_height=value.get("canvas_height"),
                goal_reached=goal_reached_val,
                completion_score=completion_score_val,
            )
        except KeyError as exc:
            raise PlanValidationError(f"drawing plan に必須項目 {exc.args[0]} がありません") from exc
        except PlanValidationError:
            raise
        except (TypeError, ValueError) as exc:
            raise PlanValidationError(f"drawing plan の値が不正です: {exc}") from exc

    def to_svg(self, width: float | None = None, height: float | None = None) -> str:
        """高品質な SVG ベクター形式として出力する。"""
        max_x = max((p.x for s in self.strokes for p in s.points), default=800.0)
        max_y = max((p.y for s in self.strokes for p in s.points), default=800.0)
        w = width if width is not None else self.canvas_width
        h = height if height is not None else self.canvas_height
        w = max(100.0, max_x + 20.0) if w is None else _finite_number(w, "SVG width")
        h = max(100.0, max_y + 20.0) if h is None else _finite_number(h, "SVG height")
        if w <= 0 or h <= 0:
            raise PlanValidationError("SVG の幅と高さは正の有限数値である必要があります")

        # XML コメント内で "--" は禁止されているため置換する
        safe_prompt = html.escape(_xml_safe_text(self.prompt)).replace("--", "﹣﹣")
        svg_header: list[str] = [
            f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w:.1f} {h:.1f}" width="{w:.1f}" height="{h:.1f}">',
            f"  <!-- AI Stroke Painter: {safe_prompt} (Seed: {self.seed}) -->",
        ]
        svg_definitions = ["    <style>.stroke { stroke-linecap: round; stroke-linejoin: round; fill: none; }</style>"]
        svg_body: list[str] = []

        render_options = self.metadata.get("render_options", {})
        layer_mode = (
            str(render_options.get("layer_mode", "multi_layer"))
            if isinstance(render_options, Mapping)
            else "multi_layer"
        )

        # 実描画が単一／アクティブレイヤーなら、SVGも意味レイヤーの合成効果を足さず適用順を保つ。
        by_layer: dict[str, list[Stroke]] = {}
        if layer_mode == "multi_layer":
            for stroke in self.strokes:
                by_layer.setdefault(stroke.layer_name, []).append(stroke)
        else:
            by_layer["Combined"] = list(self.strokes)

        ordered_layers = list(self.layers) if layer_mode == "multi_layer" and self.layers else list(by_layer.keys())
        for layer in by_layer:
            if layer not in ordered_layers:
                ordered_layers.append(layer)
        original_order = {layer: index for index, layer in enumerate(ordered_layers)}
        ordered_layers.sort(key=lambda layer: (LAYER_RENDER_ORDER.get(layer, 35), original_order[layer]))

        mask_index = 0
        for layer_name in ordered_layers:
            strokes = by_layer.get(layer_name, [])
            if not strokes:
                continue
            layer_content: list[str] = []
            for stroke in strokes:
                if not stroke.is_eraser:
                    layer_content.extend(_stroke_svg_lines(stroke))
                    continue
                mask_id = f"eraser_mask_{mask_index}"
                mask_index += 1
                svg_definitions.append(
                    f'    <mask id="{mask_id}" maskUnits="userSpaceOnUse" x="0" y="0" width="{w:.1f}" height="{h:.1f}">'
                )
                svg_definitions.append(f'      <rect x="0" y="0" width="{w:.1f}" height="{h:.1f}" fill="#ffffff" />')
                svg_definitions.extend(f"      {line}" for line in _stroke_svg_lines(stroke, mask=True))
                svg_definitions.append("    </mask>")
                layer_content = [f'<g mask="url(#{mask_id})">', *layer_content, "</g>"]
            blend_attributes = ""
            lowered_layer = layer_name.lower()
            if layer_mode == "multi_layer" and ("shading" in lowered_layer or "shadow" in lowered_layer):
                blend_attributes = ' style="mix-blend-mode:multiply" data-krita-blend-mode="multiply"'
            elif layer_mode == "multi_layer" and any(token in lowered_layer for token in ("highlight", "fx", "glow")):
                # SVGにKritaの線形加算と同一の標準指定はないため、近似screenと元モード名を併記する。
                blend_attributes = ' style="mix-blend-mode:screen" data-krita-blend-mode="addition"'
            svg_id_layer_name = re.sub(r"[^\w.-]", "_", _xml_safe_text(layer_name))
            svg_body.append(f'  <g id="layer_{svg_id_layer_name}"{blend_attributes}>')
            svg_body.extend(f"    {line}" for line in layer_content)
            svg_body.append("  </g>")

        return "\n".join([*svg_header, "  <defs>", *svg_definitions, "  </defs>", *svg_body, "</svg>"])

    def scale_to(
        self,
        target_width: float,
        target_height: float,
        *,
        fit_mode: str = "scale",
        scale_brush_size: bool = True,
    ) -> DrawingPlan:
        """指定された解像度およびフィットモードに合わせて座標とストローク太さを変換した新しい計画を生成する。

        fit_mode:
            - 'scale' / 'stretch': 縦横それぞれ独立して拡大縮小する。
            - 'fit' / 'contain': アスペクト比を維持し、余白を中央配置してターゲット内に収める。
            - 'fill' / 'cover': アスペクト比を維持し、ターゲット全体を覆うように拡大（中央寄せ）。
            - 'center': スケールを変更せず中央に配置する。
        """
        tw = _finite_number(target_width, "target_width")
        th = _finite_number(target_height, "target_height")
        if tw <= 0 or th <= 0:
            raise PlanValidationError("target_width と target_height は正の有限数値である必要があります")

        allowed_modes = {"scale", "stretch", "fit", "contain", "fill", "cover", "center"}
        if fit_mode not in allowed_modes:
            raise PlanValidationError(f"未対応の fit_mode: {fit_mode!r} (有効値: {', '.join(sorted(allowed_modes))})")

        inferred_w = max((p.x for s in self.strokes for p in s.points), default=tw)
        inferred_h = max((p.y for s in self.strokes for p in s.points), default=th)
        src_w = max(1.0, self.canvas_width if self.canvas_width is not None else inferred_w)
        src_h = max(1.0, self.canvas_height if self.canvas_height is not None else inferred_h)

        if fit_mode in {"scale", "stretch"}:
            scale_x = tw / src_w
            scale_y = th / src_h
            offset_x = 0.0
            offset_y = 0.0
            brush_scale = math.sqrt(scale_x * scale_y)
        elif fit_mode in {"fit", "contain"}:
            scale = min(tw / src_w, th / src_h)
            scale_x = scale
            scale_y = scale
            offset_x = (tw - src_w * scale) * 0.5
            offset_y = (th - src_h * scale) * 0.5
            brush_scale = scale
        elif fit_mode in {"fill", "cover"}:
            scale = max(tw / src_w, th / src_h)
            scale_x = scale
            scale_y = scale
            offset_x = (tw - src_w * scale) * 0.5
            offset_y = (th - src_h * scale) * 0.5
            brush_scale = scale
        else:  # center
            scale_x = 1.0
            scale_y = 1.0
            offset_x = (tw - src_w) * 0.5
            offset_y = (th - src_h) * 0.5
            brush_scale = 1.0

        scaled_strokes: list[Stroke] = []
        for stroke in self.strokes:
            scaled_points = tuple(
                StrokePoint(
                    x=max(0.0, min(tw - 0.5, p.x * scale_x + offset_x)),
                    y=max(0.0, min(th - 0.5, p.y * scale_y + offset_y)),
                    pressure=p.pressure,
                    time_ms=p.time_ms,
                )
                for p in stroke.points
            )
            new_size = max(0.5, stroke.size_px * brush_scale) if scale_brush_size else stroke.size_px
            scaled_strokes.append(
                Stroke(
                    id=stroke.id,
                    points=scaled_points,
                    brush_preset=stroke.brush_preset,
                    color=stroke.color,
                    size_px=new_size,
                    layer_name=stroke.layer_name,
                    opacity=stroke.opacity,
                    is_eraser=stroke.is_eraser,
                )
            )

        metadata = dict(self.metadata)
        metadata["scaled_from"] = {
            "source_width": src_w,
            "source_height": src_h,
            "fit_mode": fit_mode,
            "scale_x": scale_x,
            "scale_y": scale_y,
        }
        return DrawingPlan(
            prompt=self.prompt,
            seed=self.seed,
            strokes=tuple(scaled_strokes),
            title=self.title,
            iteration=self.iteration,
            layers=self.layers,
            request_canvas_image=self.request_canvas_image,
            metadata=metadata,
            canvas_width=tw,
            canvas_height=th,
            goal_reached=self.goal_reached,
            completion_score=self.completion_score,
        )

    def with_canvas_size(self, width: float, height: float, *, fit_mode: str = "scale") -> DrawingPlan:
        """指定された解像度へスケーリングした新しい計画を生成する。"""
        return self.scale_to(width, height, fit_mode=fit_mode)


def combine_drawing_plans(plans: Sequence[DrawingPlan], *, auto_rescale: bool = False) -> DrawingPlan:
    """反復ごとの差分計画を、キャンバスへ適用した順序の単一計画へ統合する。"""
    if not isinstance(plans, Sequence) or not plans:
        raise PlanValidationError("plans は1件以上の DrawingPlan 配列である必要があります")
    normalized = tuple(plans)
    if any(not isinstance(plan, DrawingPlan) for plan in normalized):
        raise PlanValidationError("plans の各要素は DrawingPlan である必要があります")

    first = normalized[0]
    total_strokes = sum(len(plan.strokes) for plan in normalized)
    if total_strokes > MAX_PLAN_STROKES:
        raise PlanValidationError(
            f"反復を統合した strokes は {MAX_PLAN_STROKES} 本以下である必要があります（現在 {total_strokes} 本）"
        )

    for plan in normalized[1:]:
        if plan.prompt != first.prompt or plan.seed != first.seed:
            raise PlanValidationError("異なる prompt または seed の計画は同一セッションとして統合できません")

    # キャンバス寸法のチェックと必要に応じた自動スケール
    target_w = next((plan.canvas_width for plan in normalized if plan.canvas_width is not None), None)
    target_h = next((plan.canvas_height for plan in normalized if plan.canvas_height is not None), None)
    if auto_rescale and target_w is not None and target_h is not None:
        rescaled_plans: list[DrawingPlan] = []
        for plan in normalized:
            pw = plan.canvas_width
            ph = plan.canvas_height
            if (
                pw is not None
                and ph is not None
                and (
                    not math.isclose(target_w, pw, rel_tol=0.0, abs_tol=1e-6)
                    or not math.isclose(target_h, ph, rel_tol=0.0, abs_tol=1e-6)
                )
            ):
                rescaled_plans.append(plan.scale_to(target_w, target_h, fit_mode="scale"))
            else:
                rescaled_plans.append(plan)
        normalized = tuple(rescaled_plans)
    else:
        for field_name in ("canvas_width", "canvas_height"):
            known_values = [getattr(plan, field_name) for plan in normalized if getattr(plan, field_name) is not None]
            if known_values and any(
                not math.isclose(known_values[0], value, rel_tol=0.0, abs_tol=1e-6) for value in known_values[1:]
            ):
                raise PlanValidationError("キャンバス寸法が異なる計画は統合できません")

    merged_strokes: list[Stroke] = []
    used_ids: set[str] = set()
    for plan_index, plan in enumerate(normalized, start=1):
        for stroke in plan.strokes:
            stroke_id = stroke.id
            if stroke_id in used_ids:
                prefix = f"iteration-{plan.iteration}-{plan_index}"
                stroke_id = f"{prefix}:{stroke.id}"
                collision = 1
                while stroke_id in used_ids:
                    collision += 1
                    stroke_id = f"{prefix}-{collision}:{stroke.id}"
                stroke = Stroke(
                    id=stroke_id,
                    points=stroke.points,
                    brush_preset=stroke.brush_preset,
                    color=stroke.color,
                    size_px=stroke.size_px,
                    layer_name=stroke.layer_name,
                    opacity=stroke.opacity,
                    is_eraser=stroke.is_eraser,
                )
            used_ids.add(stroke_id)
            merged_strokes.append(stroke)

    merged_layers: list[str] = []
    for plan in normalized:
        for layer in plan.layers:
            if layer not in merged_layers:
                merged_layers.append(layer)
    last = normalized[-1]
    metadata = dict(last.metadata)
    metadata.update(
        {
            "cumulative": len(normalized) > 1,
            "iteration_count": len(normalized),
            "source_iterations": [plan.iteration for plan in normalized],
            "source_stroke_counts": [len(plan.strokes) for plan in normalized],
        }
    )
    return DrawingPlan(
        prompt=first.prompt,
        seed=first.seed,
        strokes=tuple(merged_strokes),
        title=last.title or first.title,
        iteration=last.iteration,
        layers=tuple(merged_layers),
        request_canvas_image=False,
        metadata=metadata,
        canvas_width=next((plan.canvas_width for plan in normalized if plan.canvas_width is not None), None),
        canvas_height=next((plan.canvas_height for plan in normalized if plan.canvas_height is not None), None),
        goal_reached=last.goal_reached,
        completion_score=last.completion_score,
    )


def materialize_render_options(
    plan: DrawingPlan,
    *,
    size_multiplier: float = 1.0,
    opacity_multiplier: float = 1.0,
    layer_mode: str = "multi_layer",
) -> DrawingPlan:
    """UI の描画倍率を計画へ焼き込み、保存物を実描画と一致させる。"""
    if not isinstance(plan, DrawingPlan):
        raise TypeError("plan は DrawingPlan である必要があります")
    size_value = _finite_number(size_multiplier, "size_multiplier")
    opacity_value = _finite_number(opacity_multiplier, "opacity_multiplier")
    if size_value <= 0:
        raise PlanValidationError("size_multiplier は正の有限数値である必要があります")
    if opacity_value < 0:
        raise PlanValidationError("opacity_multiplier は 0 以上の有限数値である必要があります")
    if layer_mode not in {"multi_layer", "single_layer", "active_layer"}:
        raise PlanValidationError("layer_mode が未対応です")

    strokes = tuple(
        Stroke(
            id=stroke.id,
            points=stroke.points,
            brush_preset=stroke.brush_preset,
            color=stroke.color,
            size_px=stroke.size_px * size_value,
            layer_name=stroke.layer_name,
            opacity=min(1.0, stroke.opacity * opacity_value),
            is_eraser=stroke.is_eraser,
        )
        for stroke in plan.strokes
    )
    metadata = dict(plan.metadata)
    metadata["render_options"] = {
        "size_multiplier": size_value,
        "opacity_multiplier": opacity_value,
        "layer_mode": layer_mode,
        "materialized": True,
    }
    return DrawingPlan(
        prompt=plan.prompt,
        seed=plan.seed,
        strokes=strokes,
        title=plan.title,
        iteration=plan.iteration,
        layers=plan.layers,
        request_canvas_image=plan.request_canvas_image,
        metadata=metadata,
        canvas_width=plan.canvas_width,
        canvas_height=plan.canvas_height,
        goal_reached=plan.goal_reached,
        completion_score=plan.completion_score,
    )
