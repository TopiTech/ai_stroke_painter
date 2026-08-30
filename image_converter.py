"""参照画像（PNG/JPEG等）を解析し、手描き風ストローク計画（DrawingPlan）へ変換する画像コンバータ。"""

from __future__ import annotations

from collections import deque
import heapq
import math
import random
from typing import Any
import uuid

from .brushes import brush_policy_for_profile
from .domain import MAX_PLAN_STROKES, DrawingPlan, Stroke
from .procedural import infer_palette_from_prompt
from .procedural.base import catmull_rom_spline, color_palette, create_stroke, sample_strokes_by_priority
from .qt_compat import QBuffer, QByteArray, QImage, QIODevice, argb32_image_format, write_only_open_mode
from .stroke_program import compile_stroke_program, drawing_plan_to_stroke_program
from .version import generation_trace

MAX_DECODED_IMAGE_PIXELS = 50_000_000
MAX_ENCODED_IMAGE_BYTES = 25 * 1024 * 1024
# Auto は、描画時間と DrawingPlan 上限を守る品質予算として扱う。
# 手動指定の上限と揃えることで、暗部が多い画像でも予測可能な処理量に収める。
AUTO_STROKE_BUDGET = min(500, MAX_PLAN_STROKES)
MAX_ANALYSIS_DIMENSION = 768
MAX_ANALYSIS_PIXELS = 250_000


def _strip_png_private_metadata(data: bytes) -> bytes:
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("PNG signature が不正です")
    output = bytearray(data[:8])
    offset = 8
    saw_header = False
    saw_end = False
    safe_ancillary = {b"tRNS", b"cHRM", b"gAMA", b"sRGB", b"iCCP"}
    while offset + 12 <= len(data):
        length = int.from_bytes(data[offset : offset + 4], "big")
        chunk_end = offset + 12 + length
        if chunk_end > len(data):
            raise ValueError("PNG chunk が途中で切れています")
        chunk_type = data[offset + 4 : offset + 8]
        is_critical = bool(chunk_type and 65 <= chunk_type[0] <= 90)
        if is_critical or chunk_type in safe_ancillary:
            output.extend(data[offset:chunk_end])
        saw_header = saw_header or chunk_type == b"IHDR"
        if chunk_type == b"IEND":
            saw_end = True
            break
        offset = chunk_end
    if not saw_header or not saw_end:
        raise ValueError("PNG の必須 chunk が不足しています")
    return bytes(output)


def _strip_jpeg_private_metadata(data: bytes) -> bytes:
    if not data.startswith(b"\xff\xd8"):
        raise ValueError("JPEG signature が不正です")
    output = bytearray(data[:2])
    offset = 2
    saw_scan = False
    while offset < len(data):
        if data[offset] != 0xFF:
            raise ValueError("JPEG marker が不正です")
        marker_start = offset
        while offset < len(data) and data[offset] == 0xFF:
            offset += 1
        if offset >= len(data):
            break
        marker = data[offset]
        offset += 1
        if marker == 0xD9:
            output.extend(data[marker_start:offset])
            break
        if marker == 0xDA:
            if offset + 2 > len(data):
                raise ValueError("JPEG scan header が途中で切れています")
            segment_length = int.from_bytes(data[offset : offset + 2], "big")
            scan_start = offset + segment_length
            if segment_length < 2 or scan_start > len(data):
                raise ValueError("JPEG scan length が不正です")
            output.extend(data[marker_start:scan_start])
            # EOI (End Of Image) 以降に付加された末尾の不正データや私的メタデータを確実に除去する。
            eoi_index = data.rfind(b"\xff\xd9", scan_start)
            if eoi_index != -1:
                output.extend(data[scan_start : eoi_index + 2])
            else:
                output.extend(data[scan_start:])
            saw_scan = True
            break
        if marker in {0xD8} or 0xD0 <= marker <= 0xD7:
            output.extend(data[marker_start:offset])
            continue
        if offset + 2 > len(data):
            raise ValueError("JPEG segment が途中で切れています")
        segment_length = int.from_bytes(data[offset : offset + 2], "big")
        segment_end = offset + segment_length
        if segment_length < 2 or segment_end > len(data):
            raise ValueError("JPEG segment length が不正です")
        # APP1(EXIF/XMP), APP13(IPTC), COM は位置・作者情報を含み得るため除去する。
        if marker not in {0xE1, 0xED, 0xFE}:
            output.extend(data[marker_start:segment_end])
        offset = segment_end
    if not saw_scan:
        raise ValueError("JPEG scan data がありません")
    return bytes(output)


def sanitize_reference_image(image_bytes: bytes, *, max_dimension: int = 1024) -> bytes:
    """外部送信用に画像を縮小・PNG再符号化し、私的 metadata を除去する。"""
    if not isinstance(image_bytes, bytes) or not image_bytes:
        raise ValueError("参照画像は空でない bytes である必要があります")
    if isinstance(max_dimension, bool) or not isinstance(max_dimension, int) or not 64 <= max_dimension <= 4096:
        raise ValueError("max_dimension は64から4096の整数である必要があります")
    dimensions = _image_dimensions_from_header(image_bytes)
    if dimensions is None or dimensions[0] <= 0 or dimensions[1] <= 0:
        raise ValueError("参照画像の寸法を取得できません")
    if dimensions[0] * dimensions[1] > MAX_DECODED_IMAGE_PIXELS:
        raise ValueError("参照画像の総画素数が安全上限を超えています")

    image: Any = QImage() if QImage is not None and callable(QImage) else None
    if image is not None and hasattr(image, "loadFromData") and image.loadFromData(image_bytes):
        width = int(image.width())
        height = int(image.height())
        longest = max(width, height)
        if longest > max_dimension and hasattr(image, "scaled"):
            scale = max_dimension / float(longest)
            image = image.scaled(max(1, round(width * scale)), max(1, round(height * scale)))
        if callable(QByteArray) and callable(QBuffer):
            output = QByteArray()
            buffer: Any = QBuffer(output)
            if buffer.open(write_only_open_mode(QIODevice)) and image.save(buffer, "PNG"):
                encoded = output.data() if hasattr(output, "data") else b""
                if encoded:
                    return _strip_png_private_metadata(bytes(encoded))

    if max(dimensions) > max_dimension:
        raise ValueError("この環境では参照画像を安全な送信サイズへ縮小できません")
    if image_bytes.startswith(b"\x89PNG\r\n\x1a\n"):
        return _strip_png_private_metadata(image_bytes)
    if image_bytes.startswith(b"\xff\xd8"):
        return _strip_jpeg_private_metadata(image_bytes)
    raise ValueError("外部送信用に安全化できる画像形式は PNG/JPEG です")


def _image_dimensions_from_header(data: bytes) -> tuple[int, int] | None:
    """Decode dimensions without allocating the image (PNG/JPEG/GIF/BMP/WebP)."""
    if data.startswith(b"\x89PNG\r\n\x1a\n") and len(data) >= 24:
        return int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")
    if data.startswith(b"BM") and len(data) >= 26:
        width = abs(int.from_bytes(data[18:22], "little", signed=True))
        height = abs(int.from_bytes(data[22:26], "little", signed=True))
        return width, height
    if data.startswith((b"GIF87a", b"GIF89a")) and len(data) >= 10:
        return int.from_bytes(data[6:8], "little"), int.from_bytes(data[8:10], "little")
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
    try:
        if len(h) in (3, 4):
            return int(h[0] * 2, 16), int(h[1] * 2, 16), int(h[2] * 2, 16)
        if len(h) in (6, 8):
            return int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)
    except ValueError:
        pass
    raise ValueError(f"不正な HEX カラー: {hex_str!r}")


def _srgb_channel_to_linear(value: float) -> float:
    normalized = value / 255.0
    return normalized / 12.92 if normalized <= 0.04045 else ((normalized + 0.055) / 1.055) ** 2.4


def _rgb_to_oklab(r: int, g: int, b: int) -> tuple[float, float, float]:
    """sRGB を知覚的な距離比較に向く OKLab へ変換する。"""
    red = _srgb_channel_to_linear(float(r))
    green = _srgb_channel_to_linear(float(g))
    blue = _srgb_channel_to_linear(float(b))
    light = 0.4122214708 * red + 0.5363325363 * green + 0.0514459929 * blue
    medium = 0.2119034982 * red + 0.6806995451 * green + 0.1073969566 * blue
    short = 0.0883024619 * red + 0.2817188376 * green + 0.6299787005 * blue
    light_root = math.copysign(abs(light) ** (1.0 / 3.0), light)
    medium_root = math.copysign(abs(medium) ** (1.0 / 3.0), medium)
    short_root = math.copysign(abs(short) ** (1.0 / 3.0), short)
    return (
        0.2104542553 * light_root + 0.7936177850 * medium_root - 0.0040720468 * short_root,
        1.9779984951 * light_root - 2.4285922050 * medium_root + 0.4505937099 * short_root,
        0.0259040371 * light_root + 0.7827717662 * medium_root - 0.8086757660 * short_root,
    )


def _find_closest_palette_color(r: int, g: int, b: int, palette_hex_list: list[str]) -> str:
    best_color = palette_hex_list[0] if palette_hex_list else f"#{r:02x}{g:02x}{b:02x}"
    min_dist_sq = float("inf")
    source_l, source_a, source_b = _rgb_to_oklab(r, g, b)
    for p_hex in palette_hex_list:
        try:
            pr, pg, pb = _hex_to_rgb(p_hex)
        except ValueError:
            continue
        palette_l, palette_a, palette_b = _rgb_to_oklab(pr, pg, pb)
        # 色相を保ちつつ、明度の破綻も避ける知覚距離。
        dist_sq = 1.15 * (source_l - palette_l) ** 2 + (source_a - palette_a) ** 2 + (source_b - palette_b) ** 2
        if dist_sq < min_dist_sq:
            min_dist_sq = dist_sq
            best_color = p_hex
    return best_color


def _infer_best_palette_for_image(
    pixels: list[tuple[int, int, int]],
    prompt: str = "",
) -> str:
    """プロンプトおよび画像画素の色彩統計から最適なカラーパレットを自動選択する。"""
    if prompt:
        prompt_palette = infer_palette_from_prompt(prompt)
        if prompt_palette != "anime":
            return prompt_palette
    if not pixels:
        return "anime"

    candidate_names = (
        "anime",
        "monochrome",
        "cyberpunk",
        "nature",
        "pastel",
        "watercolor",
        "retro_pop",
        "dark_fantasy",
        "sepia",
        "botanical",
        "sumie",
        "cyber_gold",
    )
    candidate_palettes: list[tuple[str, list[tuple[float, float, float]]]] = []
    for name in candidate_names:
        hexes = list(color_palette(name).values())
        oklab_colors: list[tuple[float, float, float]] = []
        for h in hexes:
            try:
                rgb_t = _hex_to_rgb(h)
            except ValueError:
                continue
            oklab_colors.append(_rgb_to_oklab(*rgb_t))
        candidate_palettes.append((name, oklab_colors))

    sample_stride = max(1, len(pixels) // 100)
    sample_oklabs = [_rgb_to_oklab(*px) for px in pixels[::sample_stride]]
    if not sample_oklabs:
        return "anime"

    best_palette = "anime"
    min_total_dist = float("inf")
    for name, pal_oklabs in candidate_palettes:
        if not pal_oklabs:
            continue
        total_dist = 0.0
        for sl, sa, sb in sample_oklabs:
            best_pixel_dist = min(1.15 * (sl - pl) ** 2 + (sa - pa) ** 2 + (sb - pb) ** 2 for pl, pa, pb in pal_oklabs)
            total_dist += best_pixel_dist
        if total_dist < min_total_dist:
            min_total_dist = total_dist
            best_palette = name

    return best_palette


_EDGE_NEIGHBORS = ((-1, -1), (0, -1), (1, -1), (-1, 0), (1, 0), (-1, 1), (0, 1), (1, 1))


def _zhang_suen_thinning(mask: list[list[bool]], max_iters: int = 8) -> list[list[bool]]:
    """Zhang-Suen 細線化アルゴリズムにより、太いエッジを1ピクセル幅の中心骨格線（Centerline）へ圧縮する。"""
    h = len(mask)
    if h < 3:
        return mask
    w = len(mask[0])
    if w < 3:
        return mask

    thinned = [row[:] for row in mask]

    for _ in range(max_iters):
        # Step 1
        to_remove_step1: list[tuple[int, int]] = []
        for y in range(1, h - 1):
            row_curr = thinned[y]
            row_prev = thinned[y - 1]
            row_next = thinned[y + 1]
            for x in range(1, w - 1):
                if not row_curr[x]:
                    continue
                p2 = 1 if row_prev[x] else 0
                p3 = 1 if row_prev[x + 1] else 0
                p4 = 1 if row_curr[x + 1] else 0
                p5 = 1 if row_next[x + 1] else 0
                p6 = 1 if row_next[x] else 0
                p7 = 1 if row_next[x - 1] else 0
                p8 = 1 if row_curr[x - 1] else 0
                p9 = 1 if row_prev[x - 1] else 0

                b = p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9
                if not (2 <= b <= 6):
                    continue

                seq = (p2, p3, p4, p5, p6, p7, p8, p9, p2)
                a = sum(1 for i in range(8) if seq[i] == 0 and seq[i + 1] == 1)
                if a != 1:
                    continue

                if p2 * p4 * p6 == 0 and p4 * p6 * p8 == 0:
                    to_remove_step1.append((x, y))

        for x, y in to_remove_step1:
            thinned[y][x] = False

        # Step 2
        to_remove_step2: list[tuple[int, int]] = []
        for y in range(1, h - 1):
            row_curr = thinned[y]
            row_prev = thinned[y - 1]
            row_next = thinned[y + 1]
            for x in range(1, w - 1):
                if not row_curr[x]:
                    continue
                p2 = 1 if row_prev[x] else 0
                p3 = 1 if row_prev[x + 1] else 0
                p4 = 1 if row_curr[x + 1] else 0
                p5 = 1 if row_next[x + 1] else 0
                p6 = 1 if row_next[x] else 0
                p7 = 1 if row_next[x - 1] else 0
                p8 = 1 if row_curr[x - 1] else 0
                p9 = 1 if row_prev[x - 1] else 0

                b = p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9
                if not (2 <= b <= 6):
                    continue

                seq = (p2, p3, p4, p5, p6, p7, p8, p9, p2)
                a = sum(1 for i in range(8) if seq[i] == 0 and seq[i + 1] == 1)
                if a != 1:
                    continue

                if p2 * p4 * p8 == 0 and p2 * p6 * p8 == 0:
                    to_remove_step2.append((x, y))

        for x, y in to_remove_step2:
            thinned[y][x] = False

        if not to_remove_step1 and not to_remove_step2:
            break

    return thinned


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


def _rdp_simplify(points: list[tuple[float, float]], epsilon: float) -> list[tuple[float, float]]:
    """Ramer-Douglas-Peucker (RDP) アルゴリズムによる点列の幾何学的単純化（スタック反復実装）。"""
    n = len(points)
    if n < 3:
        return list(points)

    keep = [False] * n
    keep[0] = True
    keep[-1] = True

    stack: list[tuple[int, int]] = [(0, n - 1)]

    while stack:
        start, end = stack.pop()
        if end <= start + 1:
            continue

        p1 = points[start]
        p2 = points[end]
        dx = p2[0] - p1[0]
        dy = p2[1] - p1[1]
        line_len = math.hypot(dx, dy)

        dmax = 0.0
        max_idx = start

        for i in range(start + 1, end):
            p = points[i]
            if line_len > 1e-6:
                d = abs(dy * p[0] - dx * p[1] + p2[0] * p1[1] - p2[1] * p1[0]) / line_len
            else:
                d = math.hypot(p[0] - p1[0], p[1] - p1[1])
            if d > dmax:
                dmax = d
                max_idx = i

        if dmax > epsilon:
            keep[max_idx] = True
            stack.append((start, max_idx))
            stack.append((max_idx, end))

    return [points[i] for i in range(n) if keep[i]]


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
        if count is not None and (
            isinstance(count, bool) or not isinstance(count, int) or not 1 <= count <= MAX_PLAN_STROKES
        ):
            raise ValueError(f"count は 1 から {MAX_PLAN_STROKES} の整数または None である必要があります")
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
        if header_dimensions is None:
            raise ValueError("対応画像形式のヘッダーを確認できませんでした")
        header_width, header_height = header_dimensions
        if header_width <= 0 or header_height <= 0:
            raise ValueError("参照画像の寸法が不正です")
        if header_width * header_height > MAX_DECODED_IMAGE_PIXELS:
            raise ValueError("参照画像の画素数が上限を超えています")
        qimg = self.qimage_cls()
        if not qimg.loadFromData(image_bytes):
            raise ValueError("参照画像をデコードできませんでした")
        decoded_width = int(qimg.width())
        decoded_height = int(qimg.height())
        if decoded_width <= 0 or decoded_height <= 0:
            raise ValueError("参照画像のデコード結果が空です")
        if decoded_width * decoded_height > MAX_DECODED_IMAGE_PIXELS:
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
            prompt=prompt,
        )

        if not strokes:
            raise ValueError("参照画像から有効なストロークを抽出できませんでした")

        legacy_plan = DrawingPlan(
            prompt=f"Image2Stroke: {prompt}" if prompt else "Image to Stroke Art",
            seed=seed,
            strokes=strokes,
            title="Image Reference Art",
            iteration=1,
            layers=["Flats", "Shading", "Lineart", "Highlights"],
            metadata={
                "generator": "image_to_stroke",
                "palette": palette_name,
                "color_mode": color_mode,
                "generation_seed": seed,
                "brush_policy": brush_policy_for_profile(brush_profile).as_dict(),
                "draft_policy": "preview_only",
                "generation_trace": generation_trace(generator="image_to_stroke", requested_count=count),
            },
            canvas_width=target_width,
            canvas_height=target_height,
        )
        return compile_stroke_program(
            drawing_plan_to_stroke_program(legacy_plan),
            count=count,
            target_width=target_width,
            target_height=target_height,
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
        prompt: str = "",
    ) -> list[Stroke]:
        brush_policy = brush_policy_for_profile(brush_profile)

        def profile_for(layer_name: str, default_profile: str) -> str:
            return brush_policy.profile_for(layer_name, default_profile=default_profile, operation_kind="path")

        # 細部を固定160pxへ潰さず、長辺と総画素の二重予算で解析解像度を適応させる。
        source_w = qimg.width()
        source_h = qimg.height()
        if source_w <= 0 or source_h <= 0:
            return []
        pixel_scale = math.sqrt(MAX_ANALYSIS_PIXELS / float(source_w * source_h))
        downscale = min(
            1.0,
            MAX_ANALYSIS_DIMENSION / source_w,
            MAX_ANALYSIS_DIMENSION / source_h,
            pixel_scale,
        )
        grid_w = max(1, round(source_w * downscale))
        grid_h = max(1, round(source_h * downscale))
        if grid_w <= 0 or grid_h <= 0:
            return []
        scaled = qimg.scaled(grid_w, grid_h)
        argb32_format = argb32_image_format(self.qimage_cls)
        if hasattr(scaled, "convertToFormat") and argb32_format is not None:
            scaled = scaled.convertToFormat(argb32_format)

        if color_mode == "palette":
            is_auto_palette = not palette_name or palette_name.strip().lower() == "auto"
            if is_auto_palette:
                sample_pixels: list[tuple[int, int, int]] = []
                for y in range(grid_h):
                    for x in range(grid_w):
                        px = scaled.pixelColor(x, y)
                        sample_pixels.append((px.red(), px.green(), px.blue()))
                resolved_palette = _infer_best_palette_for_image(sample_pixels, prompt)
            else:
                resolved_palette = palette_name.strip().lower()
            palette_hexes = list(color_palette(resolved_palette).values())
        else:
            palette_hexes = []

        # 輝度マップとカラーマップの構築
        luminance_map: list[list[float]] = []
        color_map: list[list[str]] = []
        alpha_map: list[list[float]] = []
        rgb_map: list[list[tuple[int, int, int]]] = []

        for y in range(grid_h):
            lum_row: list[float] = []
            col_row: list[str] = []
            alpha_row: list[float] = []
            rgb_row: list[tuple[int, int, int]] = []
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
                rgb_row.append((sample_r, sample_g, sample_b))
                if color_mode == "palette":
                    col_row.append(_find_closest_palette_color(sample_r, sample_g, sample_b, palette_hexes))
                else:
                    col_row.append(f"#{sample_r:02x}{sample_g:02x}{sample_b:02x}")
                alpha_row.append(alpha)
            luminance_map.append(lum_row)
            color_map.append(col_row)
            alpha_map.append(alpha_row)
            rgb_map.append(rgb_row)

        strokes: list[Stroke] = []
        fit_scale = min(target_width / grid_w, target_height / grid_h)
        offset_x = (target_width - grid_w * fit_scale) * 0.5
        offset_y = (target_height - grid_h * fit_scale) * 0.5

        def uid(name: str, idx: int = 0) -> str:
            return str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/img/{seed}/{name}/{idx}"))

        visible_samples = [
            (x, y, rgb_map[y][x], luminance_map[y][x])
            for y in range(grid_h)
            for x in range(grid_w)
            if alpha_map[y][x] >= 0.05
        ]
        if not visible_samples:
            return []
        visible_pixels = [(rgb, luminance) for _x, _y, rgb, luminance in visible_samples]
        channel_ranges = [
            max(rgb[channel] for rgb, _lum in visible_pixels) - min(rgb[channel] for rgb, _lum in visible_pixels)
            for channel in range(3)
        ]
        luminances = [lum for _rgb, lum in visible_pixels]
        if max(channel_ranges) <= 5 and max(luminances) - min(luminances) <= 0.02:
            average_rgb: tuple[int, int, int] = (
                round(sum(rgb[0] for rgb, _lum in visible_pixels) / len(visible_pixels)),
                round(sum(rgb[1] for rgb, _lum in visible_pixels) / len(visible_pixels)),
                round(sum(rgb[2] for rgb, _lum in visible_pixels) / len(visible_pixels)),
            )
            average_luminance = sum(luminances) / len(luminances)
            # 紙色だけの画像を数百本の白ストロークへ変換しない。
            if average_luminance >= 0.96:
                return []
            if not enable_flats:
                return []
            uniform_color = (
                _find_closest_palette_color(*average_rgb, palette_hexes)
                if color_mode == "palette"
                else f"#{average_rgb[0]:02x}{average_rgb[1]:02x}{average_rgb[2]:02x}"
            )
            visible_x = [x for x, _y, _rgb, _luminance in visible_samples]
            visible_y = [y for _x, y, _rgb, _luminance in visible_samples]
            fill_x0 = offset_x + min(visible_x) * fit_scale
            fill_x1 = offset_x + (max(visible_x) + 1) * fit_scale
            fill_y0 = offset_y + min(visible_y) * fit_scale
            fill_y1 = offset_y + (max(visible_y) + 1) * fit_scale
            return [
                create_stroke(
                    [(fill_x0, (fill_y0 + fill_y1) * 0.5), (fill_x1, (fill_y0 + fill_y1) * 0.5)],
                    profile_type="brush",
                    base_pressure=1.0,
                    color=uniform_color,
                    size_px=max(2.0, (fill_y1 - fill_y0) * 1.05),
                    layer_name="Flats",
                    opacity=1.0,
                    rng=rng,
                    width=target_width,
                    height=target_height,
                    stroke_id=uid("uniform_fill"),
                    preferred_profile=profile_for("Flats", "brush"),
                )
            ]

        # 明るい／透明な外周から連結する領域だけを背景とみなし、白い被写体内部は保持する。
        border_samples = [
            rgb_map[y][x] for y in range(grid_h) for x in range(grid_w) if x in {0, grid_w - 1} or y in {0, grid_h - 1}
        ]
        if not border_samples:
            return strokes
        border_rgb = tuple(
            round(sum(rgb[channel] for rgb in border_samples) / len(border_samples)) for channel in range(3)
        )
        border_luminance = sum(0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2] for rgb in border_samples) / (
            255.0 * len(border_samples)
        )

        def is_background_candidate(x: int, y: int) -> bool:
            if alpha_map[y][x] < 0.05:
                return True
            if border_luminance < 0.72:
                return False
            rgb = rgb_map[y][x]
            color_delta = math.sqrt(sum((rgb[channel] - border_rgb[channel]) ** 2 for channel in range(3))) / 441.7
            return color_delta <= 0.10 and abs(luminance_map[y][x] - border_luminance) <= 0.12

        background_mask = [[False for _x in range(grid_w)] for _y in range(grid_h)]
        background_queue: deque[tuple[int, int]] = deque()
        for y in range(grid_h):
            for x in range(grid_w):
                if x not in {0, grid_w - 1} and y not in {0, grid_h - 1}:
                    continue
                if is_background_candidate(x, y) and not background_mask[y][x]:
                    background_mask[y][x] = True
                    background_queue.append((x, y))
        while background_queue:
            x, y = background_queue.popleft()
            for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                nx, ny = x + dx, y + dy
                if not (0 <= nx < grid_w and 0 <= ny < grid_h) or background_mask[ny][nx]:
                    continue
                if is_background_candidate(nx, ny):
                    background_mask[ny][nx] = True
                    background_queue.append((nx, ny))

        # 1. エッジ検出（Sobel + ヒステリシス輪郭追跡による高精細エッジ抽出）-> Lineart
        safe_edge_threshold = max(0.02, min(0.60, edge_threshold))
        low_edge_threshold = safe_edge_threshold * 0.45
        strong_edge_mask = [[False for _x in range(grid_w)] for _y in range(grid_h)]
        candidate_mask = [[False for _x in range(grid_w)] for _y in range(grid_h)]

        for y in range(1, grid_h - 1):
            for x in range(1, grid_w - 1):
                # 勾配の算出
                lum_dx = (luminance_map[y - 1][x + 1] + 2 * luminance_map[y][x + 1] + luminance_map[y + 1][x + 1]) - (
                    luminance_map[y - 1][x - 1] + 2 * luminance_map[y][x - 1] + luminance_map[y + 1][x - 1]
                )
                lum_dy = (luminance_map[y + 1][x - 1] + 2 * luminance_map[y + 1][x] + luminance_map[y + 1][x + 1]) - (
                    luminance_map[y - 1][x - 1] + 2 * luminance_map[y - 1][x] + luminance_map[y - 1][x + 1]
                )
                color_gradient_sq = 0.0
                for channel in range(3):
                    color_dx = (rgb_map[y][x + 1][channel] - rgb_map[y][x - 1][channel]) / 255.0
                    color_dy = (rgb_map[y + 1][x][channel] - rgb_map[y - 1][x][channel]) / 255.0
                    color_gradient_sq += color_dx * color_dx + color_dy * color_dy
                mag = max(math.hypot(lum_dx, lum_dy), 0.65 * math.sqrt(color_gradient_sq))
                touches_subject = not background_mask[y][x] or any(
                    not background_mask[y + ny][x + nx] for nx, ny in ((-1, 0), (1, 0), (0, -1), (0, 1))
                )
                if touches_subject:
                    if mag > safe_edge_threshold:
                        strong_edge_mask[y][x] = True
                    elif mag > low_edge_threshold:
                        candidate_mask[y][x] = True

        # 強いエッジから連結する中程度エッジをヒステリシス追跡して確定
        edge_mask = [[strong_edge_mask[y][x] for x in range(grid_w)] for y in range(grid_h)]
        edge_queue: deque[tuple[int, int]] = deque(
            (x, y) for y in range(grid_h) for x in range(grid_w) if strong_edge_mask[y][x]
        )
        while edge_queue:
            ex, ey = edge_queue.popleft()
            for dx, dy in ((-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1)):
                nx, ny = ex + dx, ey + dy
                if 0 <= nx < grid_w and 0 <= ny < grid_h and candidate_mask[ny][nx] and not edge_mask[ny][nx]:
                    edge_mask[ny][nx] = True
                    edge_queue.append((nx, ny))

        # エッジを細線化（Zhang-Suen Thinning）して中心線（Centerline）を抽出し、二重線を解消
        thinned_edge_mask = _zhang_suen_thinning(edge_mask)

        # エッジを連結し、輪郭に沿う連続ストロークを構築する（微小ノイズパスはフィルタ）。
        max_paths = min(max(20, (count * 3) if count is not None else 180), MAX_PLAN_STROKES)
        raw_edge_paths = _trace_edge_paths(thinned_edge_mask, max_paths=max_paths)
        if not raw_edge_paths:
            raw_edge_paths = _trace_edge_paths(edge_mask, max_paths=max_paths)
        # 1ピクセルのみの微小孤立ノイズをカットし、意味のある輪郭線のみを保持
        edge_paths = [p for p in raw_edge_paths if len(p) >= 2 or len(raw_edge_paths) <= 10]
        if not edge_paths and raw_edge_paths:
            edge_paths = raw_edge_paths

        for i, pixel_path in enumerate(edge_paths):
            raw_controls = [(offset_x + px * fit_scale, offset_y + py * fit_scale) for px, py in pixel_path]
            simplified = _rdp_simplify(raw_controls, epsilon=max(0.8, fit_scale * 0.55))
            if len(simplified) > 24:
                sample_step = max(1, math.ceil(len(simplified) / 24))
                simplified = simplified[::sample_step]
                if simplified[-1] != raw_controls[-1]:
                    simplified.append(raw_controls[-1])
            mid_x, mid_y = pixel_path[len(pixel_path) // 2]
            col = color_map[mid_y][mid_x]
            spline = catmull_rom_spline(simplified, 4) if len(simplified) >= 3 else simplified
            strokes.append(
                create_stroke(
                    spline,
                    profile_type="gpen",
                    base_pressure=0.85,
                    color="#1a1a1a" if (rng.random() > 0.25 and color_mode != "palette") else col,
                    size_px=rng.uniform(3.0, 5.5),
                    layer_name="Lineart",
                    rng=rng,
                    width=target_width,
                    height=target_height,
                    stroke_id=uid("edge", i),
                    preferred_profile=profile_for("Lineart", "gpen"),
                )
            )

        # 2. 暗部領域の立体感ハッチング（法線勾配適応 & クロスハッチング） -> Shading
        if shading_density != "off":
            if shading_density == "low":
                dark_step = max(3, int(grid_w / 18))
                lum_cutoff = 0.35
            elif shading_density == "high":
                dark_step = max(1, int(grid_w / 38))
                lum_cutoff = 0.55
            else:  # medium
                dark_step = max(2, int(grid_w / 28))
                lum_cutoff = 0.45

            for y in range(0, grid_h, dark_step):
                for x in range(0, grid_w, dark_step):
                    lum = luminance_map[y][x]
                    if lum < lum_cutoff:
                        if alpha_map[y][x] < 0.05 or background_mask[y][x]:
                            continue
                        # 局所輝度勾配（Sobel法線推定）から面の接線（等高線）方向を算出
                        if 1 <= x < grid_w - 1 and 1 <= y < grid_h - 1:
                            gx = (luminance_map[y][x + 1] - luminance_map[y][x - 1]) * 0.5
                            gy = (luminance_map[y + 1][x] - luminance_map[y - 1][x]) * 0.5
                            grad_mag = math.hypot(gx, gy)
                            if grad_mag > 0.04:
                                # 勾配に垂直な等高線方向（面の丸みに沿う方向）
                                dir_x = -gy / grad_mag
                                dir_y = gx / grad_mag
                            else:
                                dir_x, dir_y = 0.866, 0.5  # 30度標準
                        else:
                            dir_x, dir_y = 0.866, 0.5

                        def clipped_hatch(
                            origin_x: int,
                            origin_y: int,
                            direction_x: float,
                            direction_y: float,
                            maximum_distance: float,
                        ) -> list[tuple[float, float]]:
                            endpoints: list[tuple[float, float]] = []
                            for sign in (-1.0, 1.0):
                                last_x, last_y = float(origin_x), float(origin_y)
                                distance = 0.5
                                while distance <= maximum_distance:
                                    sample_x = origin_x + direction_x * distance * sign
                                    sample_y = origin_y + direction_y * distance * sign
                                    ix, iy = round(sample_x), round(sample_y)
                                    if not (0 <= ix < grid_w and 0 <= iy < grid_h):
                                        break
                                    if alpha_map[iy][ix] < 0.05 or background_mask[iy][ix]:
                                        break
                                    last_x, last_y = sample_x, sample_y
                                    distance += 0.5
                                endpoints.append((offset_x + last_x * fit_scale, offset_y + last_y * fit_scale))
                            return endpoints

                        # 第1方向ハッチング（被写体マスク内で曲面に沿う）
                        h_stroke = clipped_hatch(x, y, dir_x, dir_y, dark_step * 1.5)
                        if math.dist(h_stroke[0], h_stroke[1]) < max(1.0, fit_scale * 0.5):
                            continue
                        strokes.append(
                            create_stroke(
                                h_stroke,
                                profile_type="marupen",
                                base_pressure=0.65,
                                color=color_map[y][x],
                                size_px=3.0,
                                layer_name="Shading",
                                opacity=0.75,
                                rng=rng,
                                width=target_width,
                                height=target_height,
                                stroke_id=uid("shade", len(strokes)),
                                preferred_profile=profile_for("Shading", "marupen"),
                            )
                        )
                        # 最暗部（lum < 0.20）ではクロスハッチングを追加して深みを表現
                        if lum < 0.20 and shading_density in {"medium", "high"}:
                            cross_stroke = clipped_hatch(x, y, dir_y, -dir_x, dark_step * 1.5)
                            if math.dist(cross_stroke[0], cross_stroke[1]) < max(1.0, fit_scale * 0.5):
                                continue
                            strokes.append(
                                create_stroke(
                                    cross_stroke,
                                    profile_type="marupen",
                                    base_pressure=0.55,
                                    color=color_map[y][x],
                                    size_px=2.5,
                                    layer_name="Shading",
                                    opacity=0.60,
                                    rng=rng,
                                    width=target_width,
                                    height=target_height,
                                    stroke_id=uid("cross_shade", len(strokes)),
                                    preferred_profile=profile_for("Shading", "marupen"),
                                )
                            )

        # 3. カラーパレットサンプリングによる下塗りストローク -> Flats (有機的手描きタッチ)
        if enable_flats:
            flat_step = max(2, int(grid_w / 20))
            for y in range(0, grid_h, flat_step):
                for x in range(0, grid_w, flat_step):
                    if alpha_map[y][x] < 0.05 or background_mask[y][x]:
                        continue
                    fx = offset_x + x * fit_scale
                    fy = offset_y + y * fit_scale
                    half_span = fit_scale * flat_step * 0.70
                    # 手描きの自然なストローク角度（わずかな傾きとランダム性）
                    angle_deg = rng.uniform(-14.0, 14.0)
                    angle_rad = math.radians(angle_deg)
                    cos_a, sin_a = math.cos(angle_rad), math.sin(angle_rad)
                    # 3点による自然なカーブスプライン
                    p0 = (fx - half_span * cos_a, fy - half_span * sin_a)
                    p_mid = (fx + rng.uniform(-0.1, 0.1) * half_span, fy + rng.uniform(-0.15, 0.15) * half_span)
                    p1 = (fx + half_span * cos_a * 1.15, fy + half_span * sin_a * 1.15)
                    f_curve = catmull_rom_spline([p0, p_mid, p1], 3)
                    strokes.append(
                        create_stroke(
                            f_curve,
                            profile_type="brush",
                            base_pressure=0.82,
                            color=color_map[y][x],
                            size_px=fit_scale * flat_step * 1.35,
                            layer_name="Flats",
                            opacity=0.88,
                            rng=rng,
                            width=target_width,
                            height=target_height,
                            stroke_id=uid("flat", len(strokes)),
                            preferred_profile=profile_for("Flats", "brush"),
                        )
                    )

        # 4. 高輝度ハイライトストロークの自動抽出 -> Highlights (有機的タッチ)
        hl_step = max(4, int(grid_w / 20))
        for y in range(0, grid_h, hl_step):
            for x in range(0, grid_w, hl_step):
                lum = luminance_map[y][x]
                if lum > 0.90 and alpha_map[y][x] > 0.5 and not background_mask[y][x]:
                    hlx = offset_x + x * fit_scale
                    hly = offset_y + y * fit_scale
                    hl_angle = rng.uniform(-30.0, 30.0)
                    hl_rad = math.radians(hl_angle)
                    hl_len = fit_scale * rng.uniform(2.0, 4.0)
                    hl_p0 = (hlx, hly)
                    hl_p1 = (hlx + math.cos(hl_rad) * hl_len, hly + math.sin(hl_rad) * hl_len)
                    strokes.append(
                        create_stroke(
                            [hl_p0, hl_p1],
                            profile_type="gpen",
                            base_pressure=0.90,
                            color="#ffffff",
                            size_px=3.2,
                            layer_name="Highlights",
                            opacity=0.85,
                            rng=rng,
                            width=target_width,
                            height=target_height,
                            stroke_id=uid("hl", len(strokes)),
                            preferred_profile=profile_for("Highlights", "gpen"),
                        )
                    )

        effective_budget = count if count is not None else AUTO_STROKE_BUDGET
        return sample_strokes_by_priority(strokes, effective_budget)
