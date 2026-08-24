"""参照画像（PNG/JPEG等）を解析し、手描き風ストローク計画（DrawingPlan）へ変換する画像コンバータ。"""

from __future__ import annotations

import heapq
import math
import random
from typing import Any
import uuid

from .domain import DrawingPlan, Stroke
from .procedural.base import catmull_rom_spline, color_palette, create_stroke, sample_strokes_by_priority
from .qt_compat import QImage, argb32_image_format

MAX_DECODED_IMAGE_PIXELS = 50_000_000
MAX_ENCODED_IMAGE_BYTES = 25 * 1024 * 1024


def _image_dimensions_from_header(data: bytes) -> tuple[int, int] | None:
    """Decode dimensions without allocating the image (PNG/JPEG/BMP/WebP)."""
    if data.startswith(b"\x89PNG\r\n\x1a\n") and len(data) >= 24:
        return int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")
    if data.startswith(b"BM") and len(data) >= 26:
        width = abs(int.from_bytes(data[18:22], "little", signed=True))
        height = abs(int.from_bytes(data[22:26], "little", signed=True))
        return width, height
    if data.startswith(b"RIFF") and len(data) >= 30 and data[8:12] == b"WEBP":
        chunk_type = data[12:16]
        if chunk_type == b"VP8X":
            width = 1 + int.from_bytes(data[24:27], "little")
            height = 1 + int.from_bytes(data[27:30], "little")
            return width, height
        if chunk_type == b"VP8 " and data[23:26] == b"\x9d\x01\x2a":
            width = int.from_bytes(data[26:28], "little") & 0x3FFF
            height = int.from_bytes(data[28:30], "little") & 0x3FFF
            return width, height
        if chunk_type == b"VP8L" and data[20] == 0x2F:
            packed = int.from_bytes(data[21:25], "little")
            width = 1 + (packed & 0x3FFF)
            height = 1 + ((packed >> 14) & 0x3FFF)
            return width, height
    if data.startswith(b"\xff\xd8"):
        offset = 2
        sof_markers = {
            0xC0,
            0xC1,
            0xC2,
            0xC3,
            0xC5,
            0xC6,
            0xC7,
            0xC9,
            0xCA,
            0xCB,
            0xCD,
            0xCE,
            0xCF,
        }
        while offset + 4 <= len(data):
            if data[offset] != 0xFF:
                offset += 1
                continue
            while offset < len(data) and data[offset] == 0xFF:
                offset += 1
            if offset >= len(data):
                break
            marker = data[offset]
            offset += 1
            if marker in {0xD8, 0xD9} or 0xD0 <= marker <= 0xD7:
                continue
            if offset + 2 > len(data):
                break
            segment_length = int.from_bytes(data[offset : offset + 2], "big")
            if segment_length < 2 or offset + segment_length > len(data):
                break
            if marker in sof_markers and segment_length >= 7:
                height = int.from_bytes(data[offset + 3 : offset + 5], "big")
                width = int.from_bytes(data[offset + 5 : offset + 7], "big")
                return width, height
            offset += segment_length
    return None


def _hex_to_rgb(hex_str: str) -> tuple[int, int, int]:
    h = hex_str.lstrip("#")
    if len(h) in (3, 4):
        return int(h[0] * 2, 16), int(h[1] * 2, 16), int(h[2] * 2, 16)
    if len(h) in (6, 8):
        return int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)
    return (0, 0, 0)


def _find_closest_palette_color(r: int, g: int, b: int, palette_hex_list: list[str]) -> str:
    best_color = palette_hex_list[0] if palette_hex_list else f"#{r:02x}{g:02x}{b:02x}"
    min_dist_sq = float("inf")
    for p_hex in palette_hex_list:
        pr, pg, pb = _hex_to_rgb(p_hex)
        dist_sq = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
        if dist_sq < min_dist_sq:
            min_dist_sq = dist_sq
            best_color = p_hex
    return best_color


_EDGE_NEIGHBORS = ((-1, -1), (0, -1), (1, -1), (-1, 0), (1, 0), (-1, 1), (0, 1), (1, 1))


def _trace_edge_paths(edge_mask: list[list[bool]], max_paths: int) -> list[list[tuple[int, int]]]:
    """8近傍で連結したエッジを、連続する決定論的な点列へ変換する。"""
    remaining = {(x, y) for y, row in enumerate(edge_mask) for x, is_edge in enumerate(row) if is_edge}
    paths: list[list[tuple[int, int]]] = []

    def neighbors(point: tuple[int, int]) -> list[tuple[int, int]]:
        x, y = point
        return [(x + dx, y + dy) for dx, dy in _EDGE_NEIGHBORS if (x + dx, y + dy) in remaining]

    degrees = {point: len(neighbors(point)) for point in remaining}
    endpoints = [(point[1], point[0]) for point, degree in degrees.items() if degree <= 1]
    heapq.heapify(endpoints)

    def remove_point(point: tuple[int, int]) -> None:
        remaining.remove(point)
        x, y = point
        for dx, dy in _EDGE_NEIGHBORS:
            adjacent = (x + dx, y + dy)
            if adjacent not in remaining:
                continue
            degrees[adjacent] -= 1
            if degrees[adjacent] <= 1:
                heapq.heappush(endpoints, (adjacent[1], adjacent[0]))

    def next_start() -> tuple[int, int]:
        while endpoints:
            y, x = heapq.heappop(endpoints)
            if (x, y) in remaining and degrees[(x, y)] <= 1:
                return x, y
        return min(remaining, key=lambda point: (point[1], point[0]))

    while remaining and len(paths) < max_paths:
        current = next_start()
        remove_point(current)
        path = [current]
        previous: tuple[int, int] | None = None

        while True:
            candidates = neighbors(current)
            if not candidates:
                break
            if previous is None:
                next_point = min(candidates, key=lambda point: (point[1], point[0]))
            else:
                incoming = (current[0] - previous[0], current[1] - previous[1])

                def continuity_score(
                    point: tuple[int, int],
                    origin: tuple[int, int] = current,
                    direction: tuple[int, int] = incoming,
                ) -> tuple[int, int, int]:
                    outgoing = (point[0] - origin[0], point[1] - origin[1])
                    dot = direction[0] * outgoing[0] + direction[1] * outgoing[1]
                    return dot, -point[1], -point[0]

                next_point = max(candidates, key=continuity_score)
            remove_point(next_point)
            previous, current = current, next_point
            path.append(current)

        if len(path) >= 2:
            paths.append(path)

    paths.sort(key=lambda path: (-len(path), path[0][1], path[0][0]))
    return paths


class ImageStrokeConverter:
    """参照画像を解析して、輪郭線・陰影・色を抽出した Stroke 群を生成する。"""

    def __init__(self) -> None:
        self.qimage_cls = QImage

    def convert_image_to_plan(
        self,
        image_bytes: bytes,
        prompt: str,
        seed: int,
        count: int | None,
        target_width: float,
        target_height: float,
        edge_threshold: float = 0.18,
        shading_density: str = "medium",
        enable_flats: bool = True,
        color_mode: str = "original",
        palette_name: str = "anime",
        brush_profile: str = "auto",
    ) -> DrawingPlan:
        """画像バイトデータから DrawingPlan を生成する。"""
        if not isinstance(image_bytes, bytes):
            raise ValueError("image_bytes は bytes である必要があります")
        if len(image_bytes) > MAX_ENCODED_IMAGE_BYTES:
            raise ValueError("参照画像のファイルサイズが上限を超えています")
        if not isinstance(prompt, str):
            raise ValueError("prompt は文字列である必要があります")
        if isinstance(seed, bool) or not isinstance(seed, int) or seed < 0:
            raise ValueError("seed は 0 以上の整数である必要があります")
        if count is not None and (isinstance(count, bool) or not isinstance(count, int) or not 1 <= count <= 500):
            raise ValueError("count は 1 から 500 の整数または None である必要があります")
        if any(
            isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 2
            for value in (target_width, target_height)
        ):
            raise ValueError("target_width と target_height は 2 以上の有限数値である必要があります")
        if (
            isinstance(edge_threshold, bool)
            or not isinstance(edge_threshold, (int, float))
            or not math.isfinite(edge_threshold)
        ):
            raise ValueError("edge_threshold は有限数値である必要があります")
        if shading_density not in {"off", "low", "medium", "high"}:
            raise ValueError("shading_density は off/low/medium/high のいずれかである必要があります")
        if color_mode not in {"original", "palette"}:
            raise ValueError("color_mode は original または palette である必要があります")
        if not isinstance(enable_flats, bool):
            raise ValueError("enable_flats は真偽値である必要があります")
        rng = random.Random(seed)
        strokes: list[Stroke] = []

        if self.qimage_cls is None:
            raise RuntimeError("この環境では参照画像をデコードできません")
        header_dimensions = _image_dimensions_from_header(image_bytes)
        if header_dimensions is not None:
            header_width, header_height = header_dimensions
            if header_width <= 0 or header_height <= 0:
                raise ValueError("参照画像の寸法が不正です")
            if header_width * header_height > MAX_DECODED_IMAGE_PIXELS:
                raise ValueError("参照画像の画素数が上限を超えています")
        qimg = self.qimage_cls()
        if not qimg.loadFromData(image_bytes):
            raise ValueError("参照画像をデコードできませんでした")
        if qimg.width() * qimg.height() > MAX_DECODED_IMAGE_PIXELS:
            raise ValueError("参照画像の画素数が上限を超えています")
        strokes = self._process_qimage(
            qimg=qimg,
            seed=seed,
            count=count,
            target_width=target_width,
            target_height=target_height,
            rng=rng,
            edge_threshold=edge_threshold,
            shading_density=shading_density,
            enable_flats=enable_flats,
            color_mode=color_mode,
            palette_name=palette_name,
            brush_profile=brush_profile,
        )

        if not strokes:
            raise ValueError("参照画像から有効なストロークを抽出できませんでした")

        return DrawingPlan(
            prompt=f"Image2Stroke: {prompt}" if prompt else "Image to Stroke Art",
            seed=seed,
            strokes=strokes,
            title="Image Reference Art",
            iteration=1,
            layers=["Flats", "Shading", "Lineart", "Highlights"],
            canvas_width=target_width,
            canvas_height=target_height,
        )

    def _process_qimage(
        self,
        qimg: Any,
        seed: int,
        count: int | None,
        target_width: float,
        target_height: float,
        rng: random.Random,
        edge_threshold: float = 0.18,
        shading_density: str = "medium",
        enable_flats: bool = True,
        color_mode: str = "original",
        palette_name: str = "anime",
        brush_profile: str = "auto",
    ) -> list[Stroke]:
        # 長辺を最大160pxに抑えつつ、入力画像の縦横比を維持する。
        source_w = qimg.width()
        source_h = qimg.height()
        if source_w <= 0 or source_h <= 0:
            return []
        downscale = min(1.0, 160.0 / source_w, 160.0 / source_h)
        grid_w = max(1, round(source_w * downscale))
        grid_h = max(1, round(source_h * downscale))
        if grid_w <= 0 or grid_h <= 0:
            return []
        scaled = qimg.scaled(grid_w, grid_h)
        argb32_format = argb32_image_format(self.qimage_cls)
        if hasattr(scaled, "convertToFormat") and argb32_format is not None:
            scaled = scaled.convertToFormat(argb32_format)

        palette_hexes = list(color_palette(palette_name).values()) if color_mode == "palette" else []

        # 輝度マップとカラーマップの構築
        luminance_map: list[list[float]] = []
        color_map: list[list[str]] = []
        alpha_map: list[list[float]] = []

        for y in range(grid_h):
            lum_row: list[float] = []
            col_row: list[str] = []
            alpha_row: list[float] = []
            for x in range(grid_w):
                pixel = scaled.pixelColor(x, y)
                r, g, b = pixel.red(), pixel.green(), pixel.blue()
                alpha = pixel.alpha() / 255.0 if hasattr(pixel, "alpha") else 1.0
                composited_r = r * alpha + 255.0 * (1.0 - alpha)
                composited_g = g * alpha + 255.0 * (1.0 - alpha)
                composited_b = b * alpha + 255.0 * (1.0 - alpha)
                lum = (0.299 * composited_r + 0.587 * composited_g + 0.114 * composited_b) / 255.0
                lum_row.append(lum)
                sample_r = round(composited_r)
                sample_g = round(composited_g)
                sample_b = round(composited_b)
                if color_mode == "palette":
                    col_row.append(_find_closest_palette_color(sample_r, sample_g, sample_b, palette_hexes))
                else:
                    col_row.append(f"#{sample_r:02x}{sample_g:02x}{sample_b:02x}")
                alpha_row.append(alpha)
            luminance_map.append(lum_row)
            color_map.append(col_row)
            alpha_map.append(alpha_row)

        strokes: list[Stroke] = []
        fit_scale = min(target_width / grid_w, target_height / grid_h)
        offset_x = (target_width - grid_w * fit_scale) * 0.5
        offset_y = (target_height - grid_h * fit_scale) * 0.5

        def uid(name: str, idx: int = 0) -> str:
            return str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/img/{seed}/{name}/{idx}"))

        # 1. エッジ検出（Sobel風フィルタによる輪郭抽出）-> Lineart
        safe_edge_threshold = max(0.02, min(0.60, edge_threshold))
        edge_mask = [[False for _x in range(grid_w)] for _y in range(grid_h)]

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
                if mag > safe_edge_threshold:
                    edge_mask[y][x] = True

        # エッジを連結し、輪郭に沿う連続ストロークを構築する。
        max_paths = min(max(20, (count * 3) if count is not None else 150), 500)
        edge_paths = _trace_edge_paths(edge_mask, max_paths=max_paths)
        for i, pixel_path in enumerate(edge_paths):
            sample_step = max(1, math.ceil(len(pixel_path) / 24))
            control_pixels = pixel_path[::sample_step]
            if control_pixels[-1] != pixel_path[-1]:
                control_pixels.append(pixel_path[-1])
            controls = [(offset_x + px * fit_scale, offset_y + py * fit_scale) for px, py in control_pixels]
            mid_x, mid_y = pixel_path[len(pixel_path) // 2]
            col = color_map[mid_y][mid_x]
            spline = catmull_rom_spline(controls, 2) if len(controls) >= 3 else controls
            strokes.append(
                create_stroke(
                    spline,
                    profile_type="gpen",
                    base_pressure=0.85,
                    color="#1a1a1a" if (rng.random() > 0.3 and color_mode != "palette") else col,
                    size_px=rng.uniform(3.5, 6.0),
                    layer_name="Lineart",
                    rng=rng,
                    width=target_width,
                    height=target_height,
                    stroke_id=uid("edge", i),
                    preferred_profile=brush_profile,
                )
            )

        # 2. 暗部領域の陰影ハッチング -> Shading
        if shading_density != "off":
            if shading_density == "low":
                dark_step = max(3, int(grid_w / 18))
                lum_cutoff = 0.35
            elif shading_density == "high":
                dark_step = max(1, int(grid_w / 35))
                lum_cutoff = 0.55
            else:  # medium
                dark_step = max(2, int(grid_w / 25))
                lum_cutoff = 0.45

            for y in range(0, grid_h, dark_step):
                for x in range(0, grid_w, dark_step):
                    lum = luminance_map[y][x]
                    if lum < lum_cutoff:
                        if alpha_map[y][x] < 0.05:
                            continue
                        hx = offset_x + x * fit_scale
                        hy = offset_y + y * fit_scale
                        h_len = fit_scale * dark_step * 1.5
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
                                preferred_profile=brush_profile,
                            )
                        )

        # 3. カラーパレットサンプリングによる下塗りストローク -> Flats
        if enable_flats:
            flat_step = max(3, int(grid_w / 15))
            for y in range(0, grid_h, flat_step):
                for x in range(0, grid_w, flat_step):
                    if alpha_map[y][x] < 0.05:
                        continue
                    fx = offset_x + x * fit_scale
                    fy = offset_y + y * fit_scale
                    f_stroke = [(fx - fit_scale * 2, fy), (fx + fit_scale * flat_step * 0.8, fy)]
                    strokes.append(
                        create_stroke(
                            f_stroke,
                            profile_type="brush",
                            base_pressure=0.75,
                            color=color_map[y][x],
                            size_px=fit_scale * flat_step * 0.9,
                            layer_name="Flats",
                            opacity=0.85,
                            rng=rng,
                            width=target_width,
                            height=target_height,
                            stroke_id=uid("flat", len(strokes)),
                            preferred_profile=brush_profile,
                        )
                    )

        return sample_strokes_by_priority(strokes, count)
