"""外部通信を行わない決定論的な MVP 用 Planner。"""

from __future__ import annotations

import math
import random
from typing import Any
import uuid

from .domain import DrawingPlan, Stroke, StrokePoint
from .ports import PlannerPort


def _bezier(
    p0: tuple[float, float],
    p1: tuple[float, float],
    p2: tuple[float, float],
    p3: tuple[float, float],
    t: float,
) -> tuple[float, float]:
    u = 1.0 - t
    return (
        u**3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t**3 * p3[0],
        u**3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t**3 * p3[1],
    )


def _pressure(t: float, base: float, rng: random.Random) -> float:
    taper = min(1.0, t / 0.16, (1.0 - t) / 0.22)
    wave = 0.04 * math.sin(t * math.pi * 2.0)
    return max(0.05, min(1.0, base * (0.25 + 0.75 * taper) + wave + rng.uniform(-0.012, 0.012)))


def _bounded(value: float, maximum: float) -> float:
    return max(0.0, min(float(maximum) - 1.0, value))


def _valid_dimension(value: Any, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 2:
        raise ValueError(f"{name} は 2 以上の有限数である必要があります")
    return float(value)


def validate_plan_request(
    prompt: Any,
    seed: Any,
    count: Any,
    width: Any,
    height: Any,
) -> tuple[str, int, int, float, float]:
    """Planner 実装で共通の入力契約を検証して正規化する。"""
    if not isinstance(prompt, str):
        raise ValueError("prompt は文字列である必要があります")
    if isinstance(seed, bool) or not isinstance(seed, int) or seed < 0:
        raise ValueError("seed は 0 以上の整数である必要があります")
    if isinstance(count, bool) or not isinstance(count, int) or not 1 <= count <= 200:
        raise ValueError("count は 1 から 200 の整数である必要があります")
    return prompt, seed, count, _valid_dimension(width, "width"), _valid_dimension(height, "height")


class RuleBasedPlanner(PlannerPort):
    """決定論的でオフラインの Planner。将来 LLM Adapter と差し替え可能。"""

    def plan(self, prompt: str, seed: int, count: int, width: float, height: float) -> DrawingPlan:
        valid_prompt, valid_seed, valid_count, valid_width, valid_height = validate_plan_request(
            prompt, seed, count, width, height
        )

        rng = random.Random(valid_seed)
        strokes = []
        cx, cy = valid_width * 0.5, valid_height * 0.48
        scale = min(valid_width, valid_height)
        prompt_l = valid_prompt.lower()
        hair = "hair" in prompt_l or "髪" in valid_prompt or "s字" in prompt_l
        jitter = min(16.0, max(1.0, scale * 0.015))

        for index in range(valid_count):
            phase = (index - (valid_count - 1) / 2) / max(1, valid_count - 1)
            if hair:
                p0 = (
                    cx + phase * scale * 0.34 + rng.uniform(-jitter * 0.5, jitter * 0.5),
                    cy - scale * 0.26 + rng.uniform(-jitter * 0.5, jitter * 0.5),
                )
                p1 = (cx + phase * scale * 0.46 + rng.uniform(-jitter, jitter), cy - scale * 0.05)
                p2 = (cx + phase * scale * 0.12 + rng.uniform(-jitter, jitter), cy + scale * 0.13)
                p3 = (
                    cx + phase * scale * 0.30 + rng.uniform(-jitter * 0.6, jitter * 0.6),
                    cy + scale * 0.32 + rng.uniform(-jitter * 0.5, jitter * 0.5),
                )
            else:
                y = cy + (index - (valid_count - 1) / 2) * scale * 0.035
                p0 = (cx - scale * 0.30, y)
                p1 = (cx - scale * 0.08, y - scale * 0.12)
                p2 = (cx + scale * 0.08, y + scale * 0.12)
                p3 = (cx + scale * 0.30, y)

            samples = max(20, int(scale / 25))
            base_pressure = rng.uniform(0.55, 0.9)
            points = []
            for sample_index in range(samples + 1):
                t = sample_index / samples
                x, y = _bezier(p0, p1, p2, p3, t)
                points.append(
                    StrokePoint(
                        _bounded(x, valid_width),
                        _bounded(y, valid_height),
                        _pressure(t, base_pressure, rng),
                        sample_index * 12,
                    )
                )

            stroke_id = str(
                uuid.uuid5(
                    uuid.NAMESPACE_URL,
                    f"ai-stroke-painter/v1/{valid_seed}/{valid_prompt}/{index}",
                )
            )
            strokes.append(Stroke(stroke_id, points, size_px=rng.uniform(5.0, 10.0)))
        return DrawingPlan(valid_prompt, valid_seed, strokes)
