"""外部通信を行わない決定論的な MVP 用 Planner。"""

from __future__ import annotations

import math
import random
import uuid

from .domain import DrawingPlan, Stroke, StrokePoint
from .ports import PlannerPort


def _bezier(p0, p1, p2, p3, t):
    u = 1.0 - t
    return (
        u**3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t**3 * p3[0],
        u**3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t**3 * p3[1],
    )


def _pressure(t, base, rng):
    taper = min(1.0, t / 0.16, (1.0 - t) / 0.22)
    wave = 0.04 * math.sin(t * math.pi * 2.0)
    return max(0.05, min(1.0, base * (0.25 + 0.75 * taper) + wave + rng.uniform(-0.012, 0.012)))


def _bounded(value, maximum):
    return max(0.0, min(float(maximum) - 1.0, value))


def _valid_dimension(value, name):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 2:
        raise ValueError("%s は 2 以上の有限数である必要があります" % name)
    return float(value)


def validate_plan_request(prompt, seed, count, width, height):
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

    def plan(self, prompt, seed, count, width, height):
        prompt, seed, count, width, height = validate_plan_request(prompt, seed, count, width, height)

        rng = random.Random(seed)
        strokes = []
        cx, cy = width * 0.5, height * 0.48
        scale = min(width, height)
        prompt_l = prompt.lower()
        hair = "hair" in prompt_l or "髪" in prompt or "s字" in prompt_l
        jitter = min(16.0, max(1.0, scale * 0.015))

        for index in range(count):
            phase = (index - (count - 1) / 2) / max(1, count - 1)
            if hair:
                p0 = (cx + phase * scale * 0.34 + rng.uniform(-jitter * 0.5, jitter * 0.5), cy - scale * 0.26 + rng.uniform(-jitter * 0.5, jitter * 0.5))
                p1 = (cx + phase * scale * 0.46 + rng.uniform(-jitter, jitter), cy - scale * 0.05)
                p2 = (cx + phase * scale * 0.12 + rng.uniform(-jitter, jitter), cy + scale * 0.13)
                p3 = (cx + phase * scale * 0.30 + rng.uniform(-jitter * 0.6, jitter * 0.6), cy + scale * 0.32 + rng.uniform(-jitter * 0.5, jitter * 0.5))
            else:
                y = cy + (index - (count - 1) / 2) * scale * 0.035
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
                        _bounded(x, width),
                        _bounded(y, height),
                        _pressure(t, base_pressure, rng),
                        sample_index * 12,
                    )
                )

            stroke_id = str(uuid.uuid5(uuid.NAMESPACE_URL, "ai-stroke-painter/v1/%s/%s/%s" % (seed, prompt, index)))
            strokes.append(Stroke(stroke_id, points, size_px=rng.uniform(5.0, 10.0)))
        return DrawingPlan(prompt, seed, strokes)
