"""本格プロシージャル・イラスト生成のための幾何計算、スプライン補間、筆圧プロファイル。"""

from __future__ import annotations

import math
import random
import uuid

from ..domain import Stroke, StrokePoint


def catmull_rom_spline(
    control_points: list[tuple[float, float]],
    samples_per_segment: int = 10,
) -> list[tuple[float, float]]:
    """Catmull-Rom スプライン補間により、制御点を通る滑らかな手描き風曲線を生成する。"""
    if len(control_points) < 2:
        return list(control_points)
    if len(control_points) == 2:
        p0, p1 = control_points
        return [
            (p0[0] + (p1[0] - p0[0]) * (i / samples_per_segment), p0[1] + (p1[1] - p0[1]) * (i / samples_per_segment))
            for i in range(samples_per_segment + 1)
        ]

    pts = [control_points[0]] + list(control_points) + [control_points[-1]]
    result: list[tuple[float, float]] = []

    for i in range(1, len(pts) - 2):
        p0, p1, p2, p3 = pts[i - 1], pts[i], pts[i + 1], pts[i + 2]
        for step in range(samples_per_segment if i < len(pts) - 3 else samples_per_segment + 1):
            t = step / samples_per_segment
            t2 = t * t
            t3 = t2 * t

            x = 0.5 * (
                (2 * p1[0])
                + (-p0[0] + p2[0]) * t
                + (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2
                + (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * t3
            )
            y = 0.5 * (
                (2 * p1[1])
                + (-p0[1] + p2[1]) * t
                + (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2
                + (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * t3
            )
            result.append((x, y))
    return result


def bezier_cubic(
    p0: tuple[float, float],
    p1: tuple[float, float],
    p2: tuple[float, float],
    p3: tuple[float, float],
    samples: int = 20,
) -> list[tuple[float, float]]:
    """3次ベジェ曲線から点列を生成。"""
    points: list[tuple[float, float]] = []
    for i in range(samples + 1):
        t = i / samples
        u = 1.0 - t
        x = u**3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t**3 * p3[0]
        y = u**3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t**3 * p3[1]
        points.append((x, y))
    return points


def pressure_profile(
    t: float,
    profile_type: str = "gpen",
    base: float = 0.8,
    rng: random.Random | None = None,
) -> float:
    """描画スタイルに応じた本格的な筆圧ダイナミクスを算出 (0.05〜1.0)。"""
    noise = rng.uniform(-0.02, 0.02) if rng is not None else 0.0

    if profile_type == "gpen":
        taper_in = min(1.0, t / 0.12)
        taper_out = min(1.0, (1.0 - t) / 0.15)
        curve = math.sin(t * math.pi) ** 0.8
        p = base * (0.2 + 0.8 * taper_in * taper_out * curve)
    elif profile_type == "marupen":
        taper = min(1.0, t / 0.08, (1.0 - t) / 0.08)
        p = base * (0.5 + 0.5 * taper)
    elif profile_type == "brush":
        taper_in = min(1.0, t / 0.2)
        taper_out = min(1.0, (1.0 - t) / 0.25)
        wave = 0.08 * math.sin(t * math.pi * 3.0)
        p = base * (0.3 + 0.7 * taper_in * taper_out) + wave
    elif profile_type == "marker":
        taper = min(1.0, t / 0.04, (1.0 - t) / 0.04)
        p = base * (0.8 + 0.2 * taper)
    else:  # soft
        p = base * math.sin(t * math.pi)

    return float(max(0.05, min(1.0, p + noise)))


def create_stroke(
    points_2d: list[tuple[float, float]],
    profile_type: str = "gpen",
    base_pressure: float = 0.8,
    color: str = "#232323",
    size_px: float = 6.0,
    layer_name: str = "Lineart",
    opacity: float = 1.0,
    brush_preset: str = "Basic-5 Size",
    rng: random.Random | None = None,
    width: float = 1000.0,
    height: float = 1000.0,
    stroke_id: str | None = None,
) -> Stroke:
    """2D座標点列から筆圧付き Stroke を構築する。"""
    if len(points_2d) < 2:
        if len(points_2d) == 1:
            p_single = points_2d[0]
            points_2d = [p_single, (p_single[0] + 0.5, p_single[1] + 0.5)]
        else:
            points_2d = [(0.0, 0.0), (1.0, 1.0)]

    pts: list[StrokePoint] = []
    n = len(points_2d) - 1
    for i, (px, py) in enumerate(points_2d):
        t = i / max(1, n)
        press = pressure_profile(t, profile_type, base_pressure, rng)
        bounded_x = max(0.0, min(width - 1.0, px))
        bounded_y = max(0.0, min(height - 1.0, py))
        pts.append(StrokePoint(bounded_x, bounded_y, press, i * 10))

    sid = stroke_id or str(uuid.uuid4())
    return Stroke(
        id=sid,
        points=pts,
        brush_preset=brush_preset,
        color=color,
        size_px=size_px,
        layer_name=layer_name,
        opacity=opacity,
    )


def color_palette(name: str) -> dict[str, str]:
    """テーマ別カラーパレット定義。"""
    palettes = {
        "anime": {
            "draft": "#6ba3db",
            "lineart": "#282030",
            "skin_base": "#ffebe0",
            "skin_shadow": "#f5c5b5",
            "hair_main": "#e85d75",
            "hair_shadow": "#9e2a4b",
            "hair_highlight": "#ffd6de",
            "eye_dark": "#1a2a4b",
            "eye_light": "#4a90e2",
            "highlight": "#ffffff",
            "cloth_main": "#3f51b5",
            "cloth_shadow": "#283593",
            "fx": "#ffd700",
        },
        "monochrome": {
            "draft": "#88aacc",
            "lineart": "#1a1a1a",
            "skin_base": "#f0f0f0",
            "skin_shadow": "#a0a0a0",
            "hair_main": "#2b2b2b",
            "hair_shadow": "#111111",
            "hair_highlight": "#ffffff",
            "eye_dark": "#0a0a0a",
            "eye_light": "#666666",
            "highlight": "#ffffff",
            "cloth_main": "#444444",
            "cloth_shadow": "#1f1f1f",
            "fx": "#888888",
        },
        "cyberpunk": {
            "draft": "#00f0ff",
            "lineart": "#0a0614",
            "skin_base": "#fce4ec",
            "skin_shadow": "#ba68c8",
            "hair_main": "#00ffcc",
            "hair_shadow": "#008877",
            "hair_highlight": "#ffffff",
            "eye_dark": "#2a0845",
            "eye_light": "#ff007f",
            "highlight": "#00ffff",
            "cloth_main": "#2c003e",
            "cloth_shadow": "#150020",
            "fx": "#ff007f",
        },
        "nature": {
            "draft": "#a1c181",
            "lineart": "#2b2d42",
            "skin_base": "#fefae0",
            "skin_shadow": "#dda15e",
            "hair_main": "#606c38",
            "hair_shadow": "#283618",
            "hair_highlight": "#dda15e",
            "eye_dark": "#283618",
            "eye_light": "#bc6c25",
            "highlight": "#ffffff",
            "cloth_main": "#bc6c25",
            "cloth_shadow": "#8c4a16",
            "fx": "#e76f51",
        },
    }
    return palettes.get(name.lower(), palettes["anime"])
