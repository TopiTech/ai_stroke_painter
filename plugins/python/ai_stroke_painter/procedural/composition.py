"""SceneSpec から主役・同伴物・効果の正規化配置領域を決める。"""

from __future__ import annotations

from dataclasses import dataclass, replace
import math

from ..domain import Stroke, StrokePoint
from ..scene_spec import SceneSpec


@dataclass(frozen=True)
class CompositionBox:
    x0: float
    y0: float
    x1: float
    y1: float

    def __post_init__(self) -> None:
        values = (self.x0, self.y0, self.x1, self.y1)
        if any(
            isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value)
            for value in values
        ):
            raise ValueError("CompositionBox は有限数値である必要があります")
        if not (0.0 <= self.x0 < self.x1 <= 1.0 and 0.0 <= self.y0 < self.y1 <= 1.0):
            raise ValueError("CompositionBox は0から1の範囲で正の面積を持つ必要があります")

    @property
    def width(self) -> float:
        return self.x1 - self.x0

    @property
    def height(self) -> float:
        return self.y1 - self.y0

    @property
    def center(self) -> tuple[float, float]:
        return (self.x0 + self.x1) * 0.5, (self.y0 + self.y1) * 0.5

    def as_list(self) -> list[float]:
        return [round(self.x0, 4), round(self.y0, 4), round(self.x1, 4), round(self.y1, 4)]

    def as_tuple(self) -> tuple[float, float, float, float]:
        return self.x0, self.y0, self.x1, self.y1

    def map_point(self, x: float, y: float) -> tuple[float, float]:
        return self.x0 + x * self.width, self.y0 + y * self.height


@dataclass(frozen=True)
class CompositionPlan:
    primary: CompositionBox
    companions: tuple[CompositionBox, ...]
    effect: CompositionBox
    subject_safe: CompositionBox
    mode: str

    def as_dict(self) -> dict[str, object]:
        return {
            "mode": self.mode,
            "primary_box": self.primary.as_list(),
            "companion_boxes": [box.as_list() for box in self.companions],
            "effect_box": self.effect.as_list(),
            "subject_safe_box": self.subject_safe.as_list(),
        }


def _safe_box(primary: CompositionBox, *, vertical_phase: float) -> CompositionBox:
    center_x = primary.center[0]
    center_y = primary.y0 + primary.height * vertical_phase
    half_width = min(0.14, primary.width * 0.20)
    half_height = min(0.18, primary.height * 0.18)
    return CompositionBox(
        max(0.0, center_x - half_width),
        max(0.0, center_y - half_height),
        min(1.0, center_x + half_width),
        min(1.0, center_y + half_height),
    )


def plan_scene_composition(scene_spec: SceneSpec) -> CompositionPlan:
    """主役数と構図指定から、重複しにくい決定論的レイアウトを返す。"""
    creature_count = sum(subject in {"cat", "dog", "bird", "dragon", "wolf"} for subject in scene_spec.subjects)
    has_character = "character" in scene_spec.subjects
    is_wide = scene_spec.composition == "wide"
    companions: tuple[CompositionBox, ...]

    if has_character and creature_count:
        primary = CompositionBox(0.02, 0.02, 0.68 if not is_wide else 0.58, 1.0)
        if creature_count == 1:
            companions = (CompositionBox(0.54, 0.34, 0.98, 0.98),)
        else:
            companions = tuple(
                CompositionBox(
                    0.64,
                    0.12 + index * (0.76 / creature_count),
                    0.98,
                    0.12 + (index + 1) * (0.76 / creature_count),
                )
                for index in range(creature_count)
            )
        effect = CompositionBox(0.0, 0.0, 0.72, 1.0)
        mode = "subject-companion"
        safe = _safe_box(primary, vertical_phase=0.46)
    elif scene_spec.primary_domain == "character":
        primary = CompositionBox(0.12, 0.0, 0.88, 1.0)
        companions = ()
        effect = CompositionBox(0.08, 0.0, 0.92, 1.0)
        mode = "portrait"
        safe = _safe_box(primary, vertical_phase=0.46)
    elif scene_spec.primary_domain == "creature" and creature_count > 1:
        boxes: list[CompositionBox] = []
        gap = 0.02
        usable_width = 0.92 - gap * (creature_count - 1)
        box_width = usable_width / creature_count
        for index in range(creature_count):
            x0 = 0.04 + index * (box_width + gap)
            boxes.append(CompositionBox(x0, 0.12, x0 + box_width, 0.94))
        primary, *rest = boxes
        companions = tuple(rest)
        effect = CompositionBox(0.02, 0.02, 0.98, 0.98)
        mode = "creature-ensemble"
        safe = _safe_box(primary, vertical_phase=0.52)
    elif scene_spec.primary_domain == "creature":
        primary = CompositionBox(0.14, 0.08, 0.86, 0.94)
        companions = ()
        effect = CompositionBox(0.08, 0.02, 0.92, 0.98)
        mode = "creature-single"
        safe = _safe_box(primary, vertical_phase=0.52)
    else:
        primary = CompositionBox(0.0, 0.0, 1.0, 1.0)
        companions = ()
        effect = CompositionBox(0.0, 0.0, 1.0, 1.0)
        mode = "wide-scene" if is_wide else "full-canvas"
        safe = CompositionBox(0.36, 0.32, 0.64, 0.68)
    return CompositionPlan(primary, companions, effect, safe, mode)


def transform_strokes_to_box(
    strokes: tuple[Stroke, ...] | list[Stroke],
    *,
    canvas_width: float,
    canvas_height: float,
    box: CompositionBox,
) -> tuple[Stroke, ...]:
    """キャンバス全体基準のストローク群を指定構図領域へ写像する。"""
    size_scale = math.sqrt(box.width * box.height)
    transformed: list[Stroke] = []
    for stroke in strokes:
        points: list[StrokePoint] = []
        for point in stroke.points:
            unit_x = max(0.0, min(1.0, point.x / canvas_width))
            unit_y = max(0.0, min(1.0, point.y / canvas_height))
            mapped_x, mapped_y = box.map_point(unit_x, unit_y)
            points.append(
                StrokePoint(
                    mapped_x * canvas_width,
                    mapped_y * canvas_height,
                    point.pressure,
                    point.time_ms,
                )
            )
        transformed.append(replace(stroke, points=tuple(points), size_px=max(0.5, stroke.size_px * size_scale)))
    return tuple(transformed)
