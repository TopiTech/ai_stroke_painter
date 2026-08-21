"""参照画像（PNG/JPEG等）を解析し、手描き風ストローク計画（DrawingPlan）へ変換する画像コンバータ。"""

from __future__ import annotations

import math
import random
from typing import Any
import uuid

from .domain import DrawingPlan, Stroke
from .procedural.base import catmull_rom_spline, create_stroke, sample_strokes_by_priority
from .qt_compat import QImage


class ImageStrokeConverter:
    """参照画像を解析して、輪郭線・陰影・色を抽出した Stroke 群を生成する。"""

    def __init__(self) -> None:
        self.qimage_cls = QImage

    def convert_image_to_plan(
        self,
        image_bytes: bytes,
        prompt: str,
        seed: int,
        count: int,
        target_width: float,
        target_height: float,
    ) -> DrawingPlan:
        """画像バイトデータから DrawingPlan を生成する。"""
        rng = random.Random(seed)
        strokes: list[Stroke] = []

        if self.qimage_cls is not None and image_bytes:
            qimg = self.qimage_cls()
            loaded = qimg.loadFromData(image_bytes)
            if loaded:
                strokes = self._process_qimage(qimg, seed, count, target_width, target_height, rng)

        # 画像パースに失敗した場合、またはQt環境がない場合のプロシージャルフォールバック
        if not strokes:
            from .procedural import generate_procedural_plan

            return generate_procedural_plan(prompt, seed, count, target_width, target_height)

        return DrawingPlan(
            prompt=f"Image2Stroke: {prompt}" if prompt else "Image to Stroke Art",
            seed=seed,
            strokes=strokes,
            title="Image Reference Art",
            iteration=1,
            layers=["Flats", "Shading", "Lineart", "Highlights"],
        )

    def _process_qimage(
        self,
        qimg: Any,
        seed: int,
        count: int,
        target_width: float,
        target_height: float,
        rng: random.Random,
    ) -> list[Stroke]:
        # 処理速度と解析精度のバランスのため、グリッドサイズを正規化（最大 160x160）
        grid_w = min(160, qimg.width())
        grid_h = min(160, qimg.height())
        if grid_w <= 0 or grid_h <= 0:
            return []
        scaled = qimg.scaled(grid_w, grid_h)

        # 輝度マップとカラーマップの構築
        luminance_map: list[list[float]] = []
        color_map: list[list[str]] = []

        for y in range(grid_h):
            lum_row: list[float] = []
            col_row: list[str] = []
            for x in range(grid_w):
                pixel = scaled.pixelColor(x, y)
                r, g, b = pixel.red(), pixel.green(), pixel.blue()
                lum = (0.299 * r + 0.587 * g + 0.114 * b) / 255.0
                lum_row.append(lum)
                col_row.append(f"#{r:02x}{g:02x}{b:02x}")
            luminance_map.append(lum_row)
            color_map.append(col_row)

        strokes: list[Stroke] = []
        scale_x = target_width / grid_w
        scale_y = target_height / grid_h

        def uid(name: str, idx: int = 0) -> str:
            return str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/img/{seed}/{name}/{idx}"))

        # 1. エッジ検出（Sobel風フィルタによる輪郭抽出）-> Lineart
        edge_threshold = 0.18
        edge_points: list[tuple[float, float, str]] = []

        for y in range(1, grid_h - 1):
            for x in range(1, grid_w - 1):
                # 勾配の算出
                dx = (luminance_map[y - 1][x + 1] + 2 * luminance_map[y][x + 1] + luminance_map[y + 1][x + 1]) - (
                    luminance_map[y - 1][x - 1] + 2 * luminance_map[y][x - 1] + luminance_map[y + 1][x - 1]
                )
                dy = (luminance_map[y + 1][x - 1] + 2 * luminance_map[y + 1][x] + luminance_map[y + 1][x + 1]) - (
                    luminance_map[y - 1][x - 1] + 2 * luminance_map[y - 1][x] + luminance_map[y - 1][x + 1]
                )
                mag = math.sqrt(dx * dx + dy * dy)
                if mag > edge_threshold:
                    edge_points.append((x * scale_x, y * scale_y, color_map[y][x]))

        # エッジ点から連続ストロークを構築
        rng.shuffle(edge_points)
        for i, (ex, ey, col) in enumerate(edge_points[: min(count * 2, 80)]):
            # 短い曲線ストロークを生成
            tangent_len = rng.uniform(scale_x * 2.0, scale_x * 6.0)
            p0 = (ex - tangent_len * 0.5 + rng.uniform(-2, 2), ey + rng.uniform(-2, 2))
            p1 = (ex, ey)
            p2 = (ex + tangent_len * 0.5 + rng.uniform(-2, 2), ey + rng.uniform(-2, 2))
            spline = catmull_rom_spline([p0, p1, p2], 4)
            strokes.append(
                create_stroke(
                    spline,
                    profile_type="gpen",
                    base_pressure=0.85,
                    color="#1a1a1a" if rng.random() > 0.3 else col,
                    size_px=rng.uniform(3.5, 6.0),
                    layer_name="Lineart",
                    rng=rng,
                    width=target_width,
                    height=target_height,
                    stroke_id=uid("edge", i),
                )
            )

        # 2. 暗部領域の陰影ハッチング -> Shading
        dark_step = max(2, int(grid_w / 25))
        for y in range(0, grid_h, dark_step):
            for x in range(0, grid_w, dark_step):
                lum = luminance_map[y][x]
                if lum < 0.45:  # 暗い領域
                    hx = x * scale_x
                    hy = y * scale_y
                    h_len = scale_x * dark_step * 1.5
                    h_stroke = [(hx, hy), (hx + h_len, hy + h_len * 0.5)]
                    strokes.append(
                        create_stroke(
                            h_stroke,
                            profile_type="marupen",
                            base_pressure=0.6,
                            color=color_map[y][x],
                            size_px=3.0,
                            layer_name="Shading",
                            opacity=0.7,
                            rng=rng,
                            width=target_width,
                            height=target_height,
                            stroke_id=uid("shade", len(strokes)),
                        )
                    )

        # 3. カラーパレットサンプリングによる下塗りストローク -> Flats
        flat_step = max(3, int(grid_w / 15))
        for y in range(0, grid_h, flat_step):
            for x in range(0, grid_w, flat_step):
                fx = x * scale_x
                fy = y * scale_y
                f_stroke = [(fx - scale_x * 2, fy), (fx + scale_x * flat_step * 0.8, fy)]
                strokes.append(
                    create_stroke(
                        f_stroke,
                        profile_type="brush",
                        base_pressure=0.75,
                        color=color_map[y][x],
                        size_px=scale_y * flat_step * 0.9,
                        layer_name="Flats",
                        opacity=0.85,
                        rng=rng,
                        width=target_width,
                        height=target_height,
                        stroke_id=uid("flat", len(strokes)),
                    )
                )

        return sample_strokes_by_priority(strokes, count)
