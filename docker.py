"""AI Stroke Painter Pro Krita Docker UI および非同期制御ワーカー。"""

from __future__ import annotations

from collections.abc import Sequence
import contextlib
from dataclasses import replace
import datetime
import json
import math
import os
from pathlib import Path
import random
import threading
import time
import traceback
from typing import TYPE_CHECKING, Any, cast
from urllib.parse import urlsplit

from .domain import (
    LAYER_RENDER_ORDER,
    MAX_PLAN_STROKES,
    DrawingPlan,
    Stroke,
    combine_drawing_plans,
    materialize_render_options,
    split_color_alpha,
)
from .image_converter import MAX_ENCODED_IMAGE_BYTES, _image_dimensions_from_header
from .image_generator import ImageGeneratorSettings
from .krita_adapter import KritaCanvasAdapter
from .llm_planner import OpenAICompatiblePlanner, OpenAICompatibleSettings
from .planner import ImageGenerationPlanner, RuleBasedPlanner
from .ports import PlannerPort
from .procedural.base import sample_strokes_by_priority
from .qt_compat import (
    QApplication,
    QBrush,
    QCheckBox,
    QColor,
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QImage,
    QInputDialog,
    QLabel,
    QLineEdit,
    QMessageBox,
    QObject,
    QPainter,
    QPen,
    QPlainTextEdit,
    QPointF,
    QProgressBar,
    QPushButton,
    QScrollArea,
    QSettings,
    QSpinBox,
    Qt,
    QTabWidget,
    QVBoxLayout,
    QWidget,
    antialiasing_render_hint,
    argb32_image_format,
    composition_mode_destination_out,
    composition_mode_multiply,
    composition_mode_plus,
    composition_mode_source_over,
    password_echo_mode,
    pyqtSignal,
    round_cap_style,
    round_join_style,
)
from .quality import evaluate_plan_quality
from .storage import save_plan, save_svg

MAX_REFERENCE_IMAGE_BYTES = MAX_ENCODED_IMAGE_BYTES
RENDER_WAIT_TIMEOUT_SECONDS = 15 * 60

_ELEMENT_LABELS = {
    "character": "人物",
    "cat": "猫",
    "dog": "犬",
    "bird": "鳥",
    "dragon": "ドラゴン",
    "wolf": "狼",
    "city": "都市",
    "cyber_city": "サイバー都市",
    "cathedral": "大聖堂",
    "mountain": "山",
    "sea": "海",
    "wave": "波",
    "tree": "樹木",
    "clouds": "雲",
    "sakura": "桜",
    "garden": "庭園",
    "wildflowers": "野花",
    "rose": "バラ",
    "mandala": "曼荼羅",
    "focus_lines": "集中線",
    "speed_lines": "スピード線",
    "hatching": "ハッチング",
    "glow": "発光",
    "magic_aura": "魔法オーラ",
    "magic_circle": "魔法陣",
}


def _format_plan_quality_summary(plan: DrawingPlan) -> str:
    """適用判断に必要な品質・意味不足を一行へまとめる。"""

    report = evaluate_plan_quality(plan)
    score = round(report.score * 100)
    fidelity = round(report.semantic_fidelity_score * 100)
    missing = tuple(report.missing_required_elements)
    if missing:
        labels = "、".join(_ELEMENT_LABELS.get(element, element) for element in missing)
        return f"⚠ 品質診断 {score}%｜意味充足 {fidelity}%｜不足: {labels}｜本数を増やして再生成してください"
    if report.issues:
        return f"△ 品質診断 {score}%｜意味充足 {fidelity}%｜{report.issues[0]}"

    details: list[str] = []
    scene_spec = plan.metadata.get("scene_spec", {})
    subjects = scene_spec.get("subjects", ()) if isinstance(scene_spec, dict) else ()
    if isinstance(subjects, (list, tuple)) and subjects:
        details.append(f"主役コントラスト {report.subject_background_contrast:.2f}")
    if report.effect_subject_intrusion_ratio > 0.0:
        details.append(f"効果侵入 {round(report.effect_subject_intrusion_ratio * 100)}%")
    suffix = "｜" + "｜".join(details) if details else ""
    return f"✓ 品質診断 {score}%｜意味充足 {fidelity}%{suffix}"


def _is_plan_goal_reached(plan: DrawingPlan) -> bool:
    """モデル自己申告だけでなく、独立した品質検査にも合格した場合だけ完成とする。"""
    explicit_goal = bool(plan.goal_reached or plan.metadata.get("goal_reached", False) is True)
    if not explicit_goal or plan.completion_score < 0.85 or not plan.strokes:
        return False
    report = evaluate_plan_quality(plan)
    return report.score >= 0.70 and report.out_of_bounds_points == 0


if TYPE_CHECKING:

    class DockWidget(QWidget):
        def setWindowTitle(self, title: str | None) -> None: ...
        def setWidget(self, widget: Any) -> None: ...
        def canvasChanged(self, canvas: Any) -> None: ...

    class Krita:
        @staticmethod
        def instance() -> Any: ...

else:
    try:
        from krita import DockWidget, Krita
    except ImportError:

        class DockWidget(QWidget):  # type: ignore[no-redef]
            def __init__(self, *args: Any, **kwargs: Any) -> None:
                with contextlib.suppress(Exception):
                    super().__init__(*args, **kwargs)

            def setWindowTitle(self, title: str | None) -> None:
                with contextlib.suppress(Exception):
                    super().setWindowTitle(title)

            def setWidget(self, widget: Any) -> None:
                pass

            def canvasChanged(self, canvas: Any) -> None:
                pass

        class Krita:  # type: ignore[no-redef]
            @staticmethod
            def instance() -> Any:
                return None


def _get_attr(obj: Any, name: str, default: Any = None) -> Any:
    """安全にインスタンス属性を取得 (未初期化QtモックでのRuntimeErrorを回避)。"""
    try:
        if hasattr(obj, "__dict__") and name in obj.__dict__:
            return obj.__dict__[name]
        return getattr(obj, name, default)
    except Exception:
        return default


def _message_box_button(name: str) -> Any:
    """Qt5/Qt6 の QMessageBox 標準ボタンを同じ方法で取得する。"""
    scoped = getattr(QMessageBox, "StandardButton", None)
    scoped_value = getattr(scoped, name, None)
    if scoped_value is not None:
        return scoped_value
    return getattr(QMessageBox, name, None)


def _confirm(parent: Any, title: str, message: str) -> bool:
    """明示的に Yes が選択された場合だけ破壊的操作を許可する。"""
    yes = _message_box_button("Yes")
    no = _message_box_button("No")
    if yes is None or no is None:
        return False
    answer = QMessageBox.question(parent, title, message, yes | no, no)
    return bool(answer == yes)


def _safe_endpoint_label(url: str) -> str:
    """資格情報を表示せず、デバッグ用の接続先だけを返す。"""
    try:
        parsed = urlsplit(url.strip())
        if parsed.scheme not in {"http", "https"} or not parsed.hostname:
            return "[invalid URL]"
        host = f"[{parsed.hostname}]" if ":" in parsed.hostname else parsed.hostname
        port = f":{parsed.port}" if parsed.port is not None else ""
        return f"{parsed.scheme.lower()}://{host}{port}"
    except (TypeError, ValueError):
        return "[invalid URL]"


def _safe_get_save_filename(parent: Any, title: str, default_dir: str, filter_str: str) -> tuple[str, str]:
    """Pyrefly / Qt5 / Qt6 互換の安全なファイル保存ダイアログ呼び出し。"""
    func = getattr(QFileDialog, "getSaveFileName", None)
    if func is not None and callable(func):
        res = func(parent, title, default_dir, filter_str)
        if isinstance(res, tuple) and len(res) >= 2:
            return str(res[0]), str(res[1])
        if isinstance(res, str):
            return res, ""
    return "", ""


def _safe_get_open_filename(parent: Any, title: str, default_dir: str, filter_str: str) -> tuple[str, str]:
    """Pyrefly / Qt5 / Qt6 互換の安全なファイル読込ダイアログ呼び出し。"""
    func = getattr(QFileDialog, "getOpenFileName", None)
    if func is not None and callable(func):
        res = func(parent, title, default_dir, filter_str)
        if isinstance(res, tuple) and len(res) >= 2:
            return str(res[0]), str(res[1])
        if isinstance(res, str):
            return res, ""
    return "", ""


class PreviewWidget(QWidget):
    """描画計画のストロークをリアルタイムでベクタープレビューするミニキャンバス。
    マウスホイールでズーム、ドラッグでパン移動、ダブルクリックでリセット可能。
    """

    def __init__(self, parent: Any | None = None) -> None:
        super().__init__(parent)
        self._plan: DrawingPlan | None = None
        self._accumulated_strokes: list[Stroke] = []
        self._canvas_width: float = 1000.0
        self._canvas_height: float = 1000.0
        self._size_multiplier: float = 1.0
        self._opacity_multiplier: float = 1.0
        self._layer_mode: str = "multi_layer"
        self._zoom_factor: float = 1.0
        self._pan_offset_x: float = 0.0
        self._pan_offset_y: float = 0.0
        self._dragging: bool = False
        self._last_mouse_pos: tuple[float, float] = (0.0, 0.0)
        self._layer_filter: set[str] | None = None
        self.setMinimumHeight(160)
        self.setMaximumHeight(220)
        if Qt is not None and hasattr(Qt, "OpenHandCursor") and hasattr(self, "setCursor"):
            with contextlib.suppress(Exception):
                self.setCursor(Qt.OpenHandCursor)
        if hasattr(self, "setToolTip"):
            self.setToolTip(
                "ストロークプレビュー: マウスホイールでズーム、ドラッグでパン移動、ダブルクリックでリセット"
            )

    def reset_view(self) -> None:
        """ズームとパン位置を初期状態にリセットする。"""
        self._zoom_factor = 1.0
        self._pan_offset_x = 0.0
        self._pan_offset_y = 0.0
        self.update()

    def set_layer_filter(self, allowed_layers: set[str] | None) -> None:
        """表示対象とするレイヤー名のセットを設定する（Noneで全レイヤー表示）。"""
        self._layer_filter = set(allowed_layers) if allowed_layers is not None else None
        self.update()

    def clear_plan(self) -> None:
        """プレビュー表示と累積ストロークを初期化する。"""
        self._plan = None
        self._accumulated_strokes.clear()
        self.reset_view()
        self.update()

    def set_plan(
        self,
        plan: DrawingPlan | None,
        size_multiplier: float = 1.0,
        opacity_multiplier: float = 1.0,
        accumulate: bool = False,
        layer_mode: str = "multi_layer",
    ) -> None:
        self._plan = plan
        self._size_multiplier = float(size_multiplier)
        self._opacity_multiplier = float(opacity_multiplier)
        self._layer_mode = (
            layer_mode if layer_mode in {"multi_layer", "single_layer", "active_layer"} else "multi_layer"
        )
        if plan is None:
            self._accumulated_strokes.clear()
        elif accumulate:
            self._accumulated_strokes.extend(plan.strokes)
        else:
            self._accumulated_strokes = list(plan.strokes)

        if plan is not None:
            if plan.canvas_width is not None and plan.canvas_width > 0:
                self._canvas_width = float(plan.canvas_width)
            if plan.canvas_height is not None and plan.canvas_height > 0:
                self._canvas_height = float(plan.canvas_height)
        self.update()

    def update_multipliers(self, size_multiplier: float, opacity_multiplier: float) -> None:
        self._size_multiplier = float(size_multiplier)
        self._opacity_multiplier = float(opacity_multiplier)
        self.update()

    def set_canvas_size(self, width: float, height: float) -> None:
        """Kritaアクティブキャンバスの実際の解像度をプレビューへ同期する。"""
        if width > 0 and height > 0:
            self._canvas_width = float(width)
            self._canvas_height = float(height)
            self.update()

    def paint_to_painter(self, painter: Any, width: float, height: float) -> None:
        """指定された QPainter インスタンスへストロークを描画する（テストおよびオフスクリーン出力共用）。"""
        if hasattr(painter, "setRenderHint"):
            with contextlib.suppress(Exception):
                painter.setRenderHint(antialiasing_render_hint())

        w = float(width)
        h = float(height)

        painter.fillRect(0, 0, int(w), int(h), QColor("#1e1e24"))

        strokes: Sequence[Stroke] = self._accumulated_strokes
        if not strokes and (self._plan is None or not self._plan.strokes):
            painter.setPen(QColor("#777788"))
            painter.drawText(int(w * 0.2), int(h * 0.5), "ストローク プレビュー")
            return

        if not strokes and self._plan is not None:
            strokes = self._plan.strokes

        max_x = max((p.x for s in strokes for p in s.points), default=w)
        max_y = max((p.y for s in strokes for p in s.points), default=h)
        canvas_w = (
            (self._plan.canvas_width if self._plan and self._plan.canvas_width else None) or self._canvas_width or max_x
        )
        canvas_h = (
            (self._plan.canvas_height if self._plan and self._plan.canvas_height else None)
            or self._canvas_height
            or max_y
        )
        canvas_w = max(1.0, float(canvas_w))
        canvas_h = max(1.0, float(canvas_h))

        layer_filter = _get_attr(self, "_layer_filter", None)
        if layer_filter is not None:
            strokes = [s for s in strokes if s.layer_name in layer_filter]

        zoom_factor = float(_get_attr(self, "_zoom_factor", 1.0))
        pan_x = float(_get_attr(self, "_pan_offset_x", 0.0))
        pan_y = float(_get_attr(self, "_pan_offset_y", 0.0))

        scale = min(w / canvas_w, h / canvas_h) * 0.92 * zoom_factor
        cw_px = canvas_w * scale
        ch_px = canvas_h * scale
        ox = (w - cw_px) * 0.5 + pan_x
        oy = (h - ch_px) * 0.5 + pan_y

        # 1. 白地キャンバス用紙領域の描画（Krita の白地キャンバス再現）
        rx, ry, rw, rh = int(ox), int(oy), int(cw_px), int(ch_px)
        painter.fillRect(rx, ry, rw, rh, QColor("#ffffff"))

        # 2. キャンバス用紙境界線
        pen_border = QPen(QColor("#555566"), 1.0)
        painter.setPen(pen_border)
        if hasattr(painter, "drawRect"):
            painter.drawRect(rx, ry, rw, rh)

        # 3. キャンバス用紙領域への厳格なクリッピング（用紙枠外へのはみ出し防止）
        has_clipping = hasattr(painter, "setClipRect")
        if has_clipping:
            with contextlib.suppress(Exception):
                painter.save()
                painter.setClipRect(rx, ry, rw, rh)

        # 4. ストロークの精密描画。multi layer はレイヤー別の透明バッファへ描き、
        # 消しゴムをそのレイヤーだけに適用してから Krita と同じ順序で合成する。
        cap_round = round_cap_style()
        join_round = round_join_style()
        mode_source_over = composition_mode_source_over(QPainter)
        mode_destination_out = composition_mode_destination_out(QPainter)
        mode_multiply = composition_mode_multiply(QPainter)
        mode_plus = composition_mode_plus(QPainter)

        sorted_strokes = (
            sorted(strokes, key=lambda stroke: LAYER_RENDER_ORDER.get(stroke.layer_name, 35))
            if self._layer_mode == "multi_layer"
            else list(strokes)
        )

        def layer_blend_mode(layer_name: str) -> Any:
            if self._layer_mode != "multi_layer":
                return mode_source_over
            lowered = layer_name.lower()
            if "shading" in lowered or "shadow" in lowered:
                return mode_multiply
            if "highlight" in lowered or "fx" in lowered or "glow" in lowered:
                return mode_plus
            return mode_source_over

        def draw_stroke(target: Any, stroke: Stroke, *, isolated_layer: bool = True) -> None:
            points = stroke.points
            if not points:
                return
            base_size = max(0.5, float(stroke.size_px) * self._size_multiplier)
            pen_width = max(0.35, base_size * scale)
            rgb_color, color_alpha = split_color_alpha(stroke.color)
            color = QColor(rgb_color)
            effective_opacity = max(
                0.0,
                min(1.0, float(stroke.opacity) * self._opacity_multiplier * color_alpha),
            )
            if hasattr(color, "setAlphaF"):
                color.setAlphaF(effective_opacity)
            if hasattr(target, "setCompositionMode"):
                with contextlib.suppress(Exception):
                    if stroke.is_eraser:
                        target.setCompositionMode(mode_destination_out)
                    elif isolated_layer:
                        target.setCompositionMode(mode_source_over)

            pen = QPen(color, pen_width)
            if hasattr(pen, "setCapStyle"):
                pen.setCapStyle(cap_round)
            if hasattr(pen, "setJoinStyle"):
                pen.setJoinStyle(join_round)

            is_dot = len(points) == 1 or (
                len(points) == 2 and points[0].x == points[1].x and points[0].y == points[1].y
            )
            if is_dot:
                point = points[0]
                px = ox + point.x * scale
                py = oy + point.y * scale
                effective_width = max(0.35, pen_width * max(0.2, point.pressure))
                radius = effective_width * 0.5
                target.setPen(pen)
                if hasattr(target, "drawEllipse"):
                    if hasattr(target, "setBrush") and QBrush is not None and callable(QBrush):
                        target.setBrush(QBrush(color))
                    if QPointF is not None and callable(QPointF):
                        target.drawEllipse(QPointF(px, py), radius, radius)
                    else:
                        target.drawEllipse(int(px - radius), int(py - radius), int(radius * 2), int(radius * 2))
                else:
                    target.drawLine(int(px), int(py), int(px), int(py))
                return

            for first, second in zip(points, points[1:], strict=False):
                average_pressure = (first.pressure + second.pressure) * 0.5
                effective_width = max(0.35, pen_width * average_pressure)
                if hasattr(pen, "setWidthF"):
                    pen.setWidthF(effective_width)
                elif hasattr(pen, "setWidth"):
                    pen.setWidth(max(1, int(round(effective_width))))
                target.setPen(pen)
                target.drawLine(
                    int(round(ox + first.x * scale)),
                    int(round(oy + first.y * scale)),
                    int(round(ox + second.x * scale)),
                    int(round(oy + second.y * scale)),
                )

        can_buffer_layers = (
            QImage is not None
            and callable(QImage)
            and QPainter is not None
            and callable(QPainter)
            and hasattr(painter, "drawImage")
        )
        if can_buffer_layers:
            grouped: dict[str, list[Stroke]] = {}
            for stroke in sorted_strokes:
                group_name = stroke.layer_name if self._layer_mode == "multi_layer" else "__single_layer__"
                grouped.setdefault(group_name, []).append(stroke)
            for group_name, layer_strokes in grouped.items():
                image: Any = QImage(max(1, int(round(w))), max(1, int(round(h))), argb32_image_format(QImage))
                image.fill(0)
                layer_painter: Any = QPainter(image)
                try:
                    if hasattr(layer_painter, "setRenderHint"):
                        with contextlib.suppress(Exception):
                            layer_painter.setRenderHint(antialiasing_render_hint())
                    if hasattr(layer_painter, "setClipRect"):
                        with contextlib.suppress(Exception):
                            layer_painter.setClipRect(rx, ry, rw, rh)
                    for stroke in layer_strokes:
                        draw_stroke(layer_painter, stroke)
                finally:
                    layer_painter.end()
                if hasattr(painter, "setCompositionMode"):
                    painter.setCompositionMode(layer_blend_mode(group_name))
                painter.drawImage(0, 0, image)
        else:
            # 軽量テスト painter など、オフスクリーン画像を扱えない実装の互換経路。
            for stroke in sorted_strokes:
                if hasattr(painter, "setCompositionMode") and not stroke.is_eraser:
                    painter.setCompositionMode(layer_blend_mode(stroke.layer_name))
                draw_stroke(painter, stroke, isolated_layer=False)

        if has_clipping:
            with contextlib.suppress(Exception):
                painter.restore()

        if hasattr(painter, "setCompositionMode"):
            with contextlib.suppress(Exception):
                painter.setCompositionMode(mode_source_over)

        # 5. 情報バッジ（GUI有効時のみストローク数・レイヤー数・ズーム倍率を控えめに表示）
        if (
            strokes
            and hasattr(painter, "drawText")
            and QApplication is not None
            and hasattr(QApplication, "instance")
            and QApplication.instance() is not None
        ):
            with contextlib.suppress(Exception):
                unique_layers = len({s.layer_name for s in strokes})
                zoom_str = f" · {zoom_factor:.1f}x" if abs(zoom_factor - 1.0) > 0.05 else ""
                pan_str = " · 移動中" if abs(pan_x) > 1.0 or abs(pan_y) > 1.0 else ""
                badge_text = f"{len(strokes)} 本 ({unique_layers} 層){zoom_str}{pan_str}"
                painter.setPen(QColor("#9999aa"))
                painter.drawText(8, int(h - 8), badge_text)

    def paintEvent(self, event: Any) -> None:  # noqa: N802
        if QPainter is None or QColor is None or QPen is None or not callable(QPainter):
            return

        w = float(self.width()) if hasattr(self, "width") else 200.0
        h = float(self.height()) if hasattr(self, "height") else 160.0
        try:
            painter: Any = QPainter(self)
        except Exception:
            return

        try:
            self.paint_to_painter(painter, w, h)
        finally:
            painter.end()

    def set_zoom_factor(self, factor: float) -> None:
        """ズーム倍率を設定する（0.3x〜5.0x）。"""
        self._zoom_factor = max(0.3, min(5.0, float(factor)))
        self.update()

    def set_pan_offset(self, offset_x: float, offset_y: float) -> None:
        """パン移動オフセットを設定する。"""
        self._pan_offset_x = float(offset_x)
        self._pan_offset_y = float(offset_y)
        self.update()

    def mousePressEvent(self, event: Any) -> None:  # noqa: N802
        btn = getattr(event, "button", lambda: None)()
        is_left = btn == 1 or (Qt is not None and btn == getattr(Qt, "LeftButton", 1))
        if is_left:
            self._dragging = True
            pos = getattr(event, "pos", lambda: None)()
            if pos is not None and hasattr(pos, "x") and hasattr(pos, "y"):
                self._last_mouse_pos = (float(pos.x()), float(pos.y()))
            if Qt is not None and hasattr(Qt, "ClosedHandCursor") and hasattr(self, "setCursor"):
                with contextlib.suppress(Exception):
                    self.setCursor(Qt.ClosedHandCursor)
        if hasattr(event, "accept"):
            event.accept()

    def mouseMoveEvent(self, event: Any) -> None:  # noqa: N802
        if getattr(self, "_dragging", False):
            pos = getattr(event, "pos", lambda: None)()
            if pos is not None and hasattr(pos, "x") and hasattr(pos, "y"):
                cur_x, cur_y = float(pos.x()), float(pos.y())
                dx = cur_x - self._last_mouse_pos[0]
                dy = cur_y - self._last_mouse_pos[1]
                self._last_mouse_pos = (cur_x, cur_y)
                self.set_pan_offset(self._pan_offset_x + dx, self._pan_offset_y + dy)
        if hasattr(event, "accept"):
            event.accept()

    def mouseReleaseEvent(self, event: Any) -> None:  # noqa: N802
        self._dragging = False
        if Qt is not None and hasattr(Qt, "OpenHandCursor") and hasattr(self, "setCursor"):
            with contextlib.suppress(Exception):
                self.setCursor(Qt.OpenHandCursor)
        if hasattr(event, "accept"):
            event.accept()

    def mouseDoubleClickEvent(self, event: Any) -> None:  # noqa: N802
        self.reset_view()
        if hasattr(event, "accept"):
            event.accept()

    def wheelEvent(self, event: Any) -> None:  # noqa: N802
        delta = 0
        if hasattr(event, "angleDelta"):
            ad = event.angleDelta()
            if hasattr(ad, "y"):
                delta = ad.y()
        if delta == 0 and hasattr(event, "delta"):
            delta = event.delta()
        if delta > 0:
            self.set_zoom_factor(self._zoom_factor * 1.15)
        elif delta < 0:
            self.set_zoom_factor(self._zoom_factor / 1.15)
        if hasattr(event, "accept"):
            event.accept()


class PlanWorker(QObject):
    """スレッドセーフな自律ビジョン改善ループおよびバックグラウンド計画生成ワーカー。"""

    plan_ready = pyqtSignal(object)
    iteration_progress = pyqtSignal(int, int, str)
    plan_failed = pyqtSignal(str)
    debug_log = pyqtSignal(str)
    finished = pyqtSignal()

    def __init__(
        self,
        planner: PlannerPort,
        prompt: str,
        seed: int,
        count: int | None = None,
        width: float = 1000.0,
        height: float = 1000.0,
        image_data: bytes | None = None,
        max_iterations: int = 1,
        palette_name: str = "auto",
        brush_profile: str = "auto",
        edge_threshold: float = 0.18,
        shading_density: str = "medium",
        enable_flats: bool = True,
        color_mode: str = "original",
        auto_count: bool = False,
        goal_mode: bool = False,
        parent: Any | None = None,
        canvas_port: Any | None = None,
        document: Any | None = None,
    ) -> None:
        super().__init__(parent)
        effective_max_iterations = 10 if goal_mode else max_iterations
        if (
            isinstance(effective_max_iterations, bool)
            or not isinstance(effective_max_iterations, int)
            or not 1 <= effective_max_iterations <= 10
        ):
            raise ValueError("max_iterations は 1 から 10 の整数である必要があります")
        self.planner = planner
        self.prompt = prompt
        self.seed = seed
        self.count = count
        self.width = width
        self.height = height
        self.image_data = image_data
        self.max_iterations = effective_max_iterations
        self.palette_name = palette_name
        self.brush_profile = brush_profile
        self.edge_threshold = edge_threshold
        self.shading_density = shading_density
        self.enable_flats = enable_flats
        self.color_mode = color_mode
        self.auto_count = auto_count
        self.goal_mode = goal_mode
        self._state_lock = threading.Lock()
        self._is_cancelled = False
        self._render_done_event = threading.Event()
        self._next_canvas_image: bytes | None = None
        self._render_error: str | None = None
        self._thread: threading.Thread | None = None
        self._is_running = False
        self.completed_successfully = False

        if hasattr(self.planner, "log_callback"):
            cast(Any, self.planner).log_callback = self._emit_debug_log

    def _emit_debug_log(self, message: str) -> None:
        self.debug_log.emit(message)

    def provide_canvas_capture(self, capture_bytes: bytes | None) -> None:
        """メインスレッドから取得された安全なキャンバスキャプチャを受け取り、次イテレーションを再開する。"""
        self._next_canvas_image = capture_bytes
        self._render_done_event.set()

    def notify_render_done(self) -> None:
        """メインスレッドでの描画完了を受け取り、次イテレーションの進行を再開する。"""
        self._render_done_event.set()

    def notify_render_failed(self, message: str) -> None:
        """メインスレッドの描画失敗をワーカーへ伝え、反復を失敗終了させる。"""
        self._render_error = message.strip() or "描画処理に失敗しました"
        self._render_done_event.set()

    def cancel(self) -> None:
        with self._state_lock:
            self._is_cancelled = True
            # A stop request wins over a completion observed by the worker thread.
            self.completed_successfully = False
        self._render_done_event.set()
        self.debug_log.emit("[ワーカー] キャンセル要求を受信しました")

    def is_cancelled(self) -> bool:
        with self._state_lock:
            return self._is_cancelled

    def _mark_completed_successfully(self) -> bool:
        """Atomically mark success only while cancellation has not been requested."""
        with self._state_lock:
            if self._is_cancelled:
                return False
            self.completed_successfully = True
            return True

    def isRunning(self) -> bool:  # noqa: N802
        return self._is_running or (self._thread is not None and self._thread.is_alive())

    def wait(self, timeout_ms: int = 1_500) -> bool:
        thread = self._thread
        if thread is None:
            return True
        thread.join(max(0, timeout_ms) / 1000.0)
        return not thread.is_alive()

    def start(self) -> None:
        if self.isRunning():
            return
        with self._state_lock:
            self.completed_successfully = False
        self._is_running = True
        self._thread = threading.Thread(target=self._run_wrapper, daemon=True)
        self._thread.start()

    def _run_wrapper(self) -> None:
        try:
            self.run()
        finally:
            self._is_running = False
            self.finished.emit()

    def run(self) -> None:
        try:
            count_label = "Auto (品質予算・AI自律)" if self.auto_count or self.count is None else str(self.count)
            goal_label = f", GoalMode: {self.goal_mode}" if self.goal_mode else ""
            self.debug_log.emit(
                f"[ワーカー開始] Total Iterations: {self.max_iterations}, Strokes: {count_label}{goal_label}, Target Size: {self.width:.0f}x{self.height:.0f}, Palette: {self.palette_name}, Profile: {self.brush_profile}"
            )

            session_stroke_count = 0
            session_plans: list[DrawingPlan] = []
            for iter_idx in range(1, self.max_iterations + 1):
                if self.is_cancelled():
                    self.debug_log.emit("[ワーカー] 処理が中断されました")
                    return

                mode_text = " (Goal判定中)" if self.goal_mode else ""
                msg = f"ステップ {iter_idx}/{self.max_iterations}{mode_text}: 計画を生成中..."
                self.iteration_progress.emit(iter_idx, self.max_iterations, msg)
                self.debug_log.emit(f"[ステップ {iter_idx}/{self.max_iterations}] 計画生成処理を開始")

                canvas_img: bytes | None = self._next_canvas_image if iter_idx > 1 else None

                current_plan = self.planner.plan(
                    prompt=self.prompt,
                    seed=self.seed,
                    count=self.count,
                    width=self.width,
                    height=self.height,
                    image_data=self.image_data,
                    canvas_image=canvas_img,
                    iteration=iter_idx,
                    max_iterations=self.max_iterations,
                    palette_name=self.palette_name,
                    brush_profile=self.brush_profile,
                    edge_threshold=self.edge_threshold,
                    shading_density=self.shading_density,
                    enable_flats=self.enable_flats,
                    color_mode=self.color_mode,
                    auto_count=self.auto_count,
                    goal_mode=self.goal_mode,
                    cancelled=self.is_cancelled,
                )

                remaining_steps = self.max_iterations - iter_idx + 1
                remaining_session_budget = MAX_PLAN_STROKES - session_stroke_count
                iteration_budget = max(1, remaining_session_budget // remaining_steps)
                if len(current_plan.strokes) > iteration_budget:
                    source_stroke_count = len(current_plan.strokes)
                    sampled_strokes = tuple(sample_strokes_by_priority(list(current_plan.strokes), iteration_budget))
                    budget_metadata = dict(current_plan.metadata)
                    budget_metadata["session_budget"] = {
                        "maximum_strokes": MAX_PLAN_STROKES,
                        "iteration_budget": iteration_budget,
                        "source_stroke_count": source_stroke_count,
                    }
                    current_plan = replace(
                        current_plan,
                        strokes=sampled_strokes,
                        metadata=budget_metadata,
                    )
                    self.debug_log.emit(
                        f"[セッション品質予算] ステップ {iter_idx} を {len(sampled_strokes)}/{source_stroke_count} 本へ調整しました"
                    )
                session_stroke_count += len(current_plan.strokes)

                # Goal判定は仕上げ差分だけでなく、それまでに描画した全反復の累積品質で行う。
                # 現ステップへ結果を記録して、UI側の最終保存判定にも同じ結論を渡す。
                session_goal_met = False
                if self.goal_mode:
                    session_plans.append(current_plan)
                    try:
                        cumulative_goal_plan = combine_drawing_plans(session_plans, auto_rescale=True)
                        session_goal_met = _is_plan_goal_reached(cumulative_goal_plan)
                    except Exception as exc:
                        self.debug_log.emit(
                            f"[Goal判定警告] 累積計画の統合に失敗したため現ステップのみで判定します: {exc}"
                        )
                        session_goal_met = _is_plan_goal_reached(current_plan)
                    if session_goal_met:
                        goal_metadata = dict(current_plan.metadata)
                        goal_metadata["session_goal_reached"] = True
                        goal_metadata["session_stroke_count"] = session_stroke_count
                        current_plan = replace(current_plan, metadata=goal_metadata)
                        session_plans[-1] = current_plan

                if self.is_cancelled():
                    self.debug_log.emit("[ワーカー] 描画計画受領後にキャンセルを確認しました")
                    return

                self.debug_log.emit(
                    f"[ステップ {iter_idx}] 計画生成完了。メインスレッドへ描画を要求します (ストローク数: {len(current_plan.strokes)}, 完成度: {current_plan.completion_score * 100:.0f}%, Goal達成: {current_plan.goal_reached})"
                )

                self._render_done_event.clear()
                self._next_canvas_image = None
                self._render_error = None
                self.plan_ready.emit(current_plan)

                # メインスレッドでの描画 & キャプチャ完了を待機
                render_wait_started = time.monotonic()
                while not self._render_done_event.wait(timeout=0.05):
                    if self.is_cancelled():
                        self.debug_log.emit("[ワーカー] 描画待機中にキャンセルを確認しました")
                        return
                    if time.monotonic() - render_wait_started > RENDER_WAIT_TIMEOUT_SECONDS:
                        raise TimeoutError("Krita の描画完了通知がタイムアウトしました")

                if self._render_error is not None:
                    raise RuntimeError(self._render_error)

                # Goal モード時の目標達成判定による早期自律完了
                if session_goal_met and iter_idx >= 1:
                    if not self._mark_completed_successfully():
                        return
                    self.iteration_progress.emit(
                        iter_idx,
                        iter_idx,
                        f"目標達成！(完成度: {current_plan.completion_score * 100:.0f}%) 全ステップの描画が完了しました",
                    )
                    self.debug_log.emit(
                        f"[Goal達成] ステップ {iter_idx} で目標達成判定を受信しました (完成度: {current_plan.completion_score * 100:.0f}%)"
                    )
                    break

            if not self.completed_successfully and self._mark_completed_successfully():
                self.iteration_progress.emit(self.max_iterations, self.max_iterations, "全ステップの描画が完了しました")
                self.debug_log.emit("[ワーカー完了] 全ての処理が正常に完了しました")

        except Exception as exc:
            tb = traceback.format_exc()
            self.debug_log.emit(f"[例外発生] {exc}\nスタックトレース:\n{tb}")
            if not self.is_cancelled():
                self.plan_failed.emit(str(exc))


class ApiConnectionWorker(QObject):
    """API 接続テストをUIスレッド外で実行する軽量ワーカー。"""

    succeeded = pyqtSignal(str)
    failed = pyqtSignal(str)
    debug_log = pyqtSignal(str)
    finished = pyqtSignal()

    def __init__(self, planner: OpenAICompatiblePlanner, parent: Any | None = None) -> None:
        super().__init__(parent)
        self.planner = planner
        self._thread: threading.Thread | None = None
        self._is_running = False
        self._cancelled = False
        self.planner.log_callback = self.debug_log.emit

    def start(self) -> None:
        if self._is_running:
            return
        self._is_running = True
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def isRunning(self) -> bool:  # noqa: N802
        return self._is_running or (self._thread is not None and self._thread.is_alive())

    def cancel(self) -> None:
        self._cancelled = True

    def wait(self, timeout_ms: int = 1_500) -> bool:
        thread = self._thread
        if thread is None:
            return True
        thread.join(max(0, timeout_ms) / 1000.0)
        return not thread.is_alive()

    def _run(self) -> None:
        try:
            result = self.planner.test_connection()
            if not self._cancelled:
                self.succeeded.emit(result)
        except Exception as exc:
            if not self._cancelled:
                self.failed.emit(str(exc) or exc.__class__.__name__)
        finally:
            self._is_running = False
            self.finished.emit()


class AIStrokePainterDocker(DockWidget):
    """AI Stroke Painter 製品版 Krita Docker パネル。"""

    PRESETS: list[tuple[str, str, str, int, str, float, int]] = [
        ("👤 美少女アニメ顔", "anime girl portrait, delicate eyes, flowing hair", "anime", 40, "gpen", 1.0, 100),
        ("👤 少年ヒーロー", "anime boy hero with spiky hair and confident smile", "anime", 35, "gpen", 1.0, 100),
        ("🌿 幻想的な山と桜", "fantasy sakura landscape with mountains and clouds", "nature", 30, "brush", 1.2, 90),
        ("🌊 浮世絵風の大波", "hokusai great wave with foam and ripples", "nature", 30, "gpen", 1.0, 100),
        (
            "🌸 透明水彩の野花",
            "delicate watercolor wildflower garden with soft petals",
            "watercolor",
            35,
            "brush",
            1.1,
            85,
        ),
        ("💥 迫力の集中線", "intense manga focus radial speed lines", "monochrome", 45, "marupen", 1.0, 100),
        (
            "🧙 魔法陣エフェクト",
            "intense magical circle with runes and radiant rays",
            "cyberpunk",
            35,
            "marupen",
            0.9,
            100,
        ),
        ("🐱 優雅な猫", "cute cat face with whiskers and emerald eyes", "nature", 30, "marupen", 1.0, 100),
        ("🏛️ サイバーパンク都市", "cyberpunk city skyline with neon buildings", "cyberpunk", 40, "marker", 1.0, 100),
        ("🌀 神聖幾何学マンダラ", "sacred geometry kaleidoscope mandala", "cyberpunk", 40, "marupen", 0.8, 100),
        ("🌲 和風水墨画の松", "japanese sumi-e ink wash pine tree on mountain cliff", "sepia", 35, "brush", 1.3, 85),
        ("🌹 バラの花束", "blooming rose with stem and organic leaves", "pastel", 30, "gpen", 1.0, 100),
    ]

    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("AI Stroke Painter Pro")
        self.planner = RuleBasedPlanner()
        self.canvas_port = KritaCanvasAdapter()
        self._cancel: bool = False
        self._worker: PlanWorker | None = None
        self._connection_worker: ApiConnectionWorker | None = None
        self._active_doc: Any | None = None
        self._active_view: Any | None = None
        self._canvas_session_open = False
        self._run_render_options: dict[str, Any] | None = None
        self._canvas: Any | None = None
        self._image_bytes: bytes | None = None
        self._last_plan: DrawingPlan | None = None
        self._session_plans: list[DrawingPlan] = []
        self._pending_plan: DrawingPlan | None = None
        self._applying_pending = False
        self._closing = False

        # --- 4タブ構成のドックコンテナ ---
        self.tabs = QTabWidget()

        # ==========================================
        # タブ 1: 🎨 生成・描画 (Main)
        # ==========================================
        tab_main = QWidget()
        tab_main_layout = QVBoxLayout(tab_main)

        # 1. プリセットクイック選択 & カスタムプリセット管理
        preset_box = QGroupBox("クイック・プリセット & カスタム保存")
        preset_layout = QVBoxLayout(preset_box)

        preset_row1 = QHBoxLayout()
        self.preset_combo = QComboBox()
        self._populate_presets()
        preset_apply_btn = QPushButton("適用")
        preset_apply_btn.clicked.connect(self._apply_preset)
        preset_row1.addWidget(self.preset_combo)
        preset_row1.addWidget(preset_apply_btn)
        preset_layout.addLayout(preset_row1)

        preset_row2 = QHBoxLayout()
        self.save_preset_btn = QPushButton("💾 保存...")
        self.save_preset_btn.clicked.connect(self._save_custom_preset)
        self.del_preset_btn = QPushButton("🗑️ 削除")
        self.del_preset_btn.clicked.connect(self._delete_custom_preset)
        preset_row2.addWidget(self.save_preset_btn)
        preset_row2.addWidget(self.del_preset_btn)
        preset_layout.addLayout(preset_row2)

        preset_row3 = QHBoxLayout()
        self.export_preset_btn = QPushButton("📤 出力...")
        self.export_preset_btn.clicked.connect(self._export_presets)
        self.import_preset_btn = QPushButton("📥 読込...")
        self.import_preset_btn.clicked.connect(self._import_presets)
        preset_row3.addWidget(self.export_preset_btn)
        preset_row3.addWidget(self.import_preset_btn)
        preset_layout.addLayout(preset_row3)
        tab_main_layout.addWidget(preset_box)

        # 2. プロンプト入力欄 & 履歴 & クイックタグ
        prompt_box = QGroupBox("描画指示 (Prompt)")
        prompt_layout = QVBoxLayout(prompt_box)

        prompt_hdr = QHBoxLayout()
        prompt_hdr.addWidget(QLabel("プロンプト:"))
        self.prompt_history_combo = QComboBox()
        self._populate_prompt_history()
        self.prompt_history_combo.currentIndexChanged.connect(self._on_prompt_history_selected)
        prompt_hdr.addWidget(self.prompt_history_combo)

        self.clear_prompt_btn = QPushButton("✖ クリア")
        self.clear_prompt_btn.setToolTip("プロンプト入力欄をクリアします")
        self.clear_prompt_btn.clicked.connect(self._clear_prompt)
        prompt_hdr.addWidget(self.clear_prompt_btn)
        prompt_layout.addLayout(prompt_hdr)

        self.prompt = QPlainTextEdit("anime girl portrait, delicate eyes, flowing hair")
        self.prompt.setMaximumHeight(65)
        prompt_layout.addWidget(self.prompt)

        # クイックタグ行
        quick_tags_layout = QHBoxLayout()
        quick_tags_layout.addWidget(QLabel("タグ追加:"))
        quick_tags: list[tuple[str, str]] = [
            ("アニメ調", "anime style, vibrant colors"),
            ("繊細な線画", "delicate clean lineart, fine details"),
            ("水彩風", "soft watercolor illustration, subtle gradients"),
            ("サイバー", "cyberpunk neon lights, high contrast"),
            ("集中線", "manga focus speed lines"),
            ("幾何学", "sacred geometry intricate patterns"),
        ]
        for tag_label, tag_val in quick_tags:
            tag_btn = QPushButton(f"+ {tag_label}")
            tag_btn.setToolTip(f"プロンプトに '{tag_val}' を追加します")
            tag_btn.clicked.connect(lambda _chk=False, val=tag_val: self._add_prompt_tag(val))
            quick_tags_layout.addWidget(tag_btn)
        prompt_layout.addLayout(quick_tags_layout)
        tab_main_layout.addWidget(prompt_box)

        # 3. 生成パラメータ & スタイル (Seed, 本数, パレット, ブラシスタイル, 段階的改善)
        gen_box = QGroupBox("生成パラメータ & スタイル")
        gen_layout = QVBoxLayout(gen_box)

        params_row1 = QHBoxLayout()
        params_row1.addWidget(QLabel("Seed"))
        self.seed = QSpinBox()
        self.seed.setRange(0, 2147483647)
        self.seed.setValue(42)
        self.seed.setToolTip("プロシージャル生成時に使用する乱数シードです (OpenAI互換モデル時は無効)")
        params_row1.addWidget(self.seed)

        self.auto_seed = QCheckBox("Auto")
        self.auto_seed.setToolTip("描画実行時にランダムなシード値を自動生成して適用します (OpenAI互換モデル時は無効)")
        self.auto_seed.toggled.connect(self._update_seed_controls_state)
        params_row1.addWidget(self.auto_seed)

        params_row1.addWidget(QLabel("本数"))
        self.count = QSpinBox()
        self.count.setRange(1, 2000)
        self.count.setValue(35)
        params_row1.addWidget(self.count)

        self.auto_count = QCheckBox("Auto (品質予算)")
        self.auto_count.setToolTip("AIが品質と処理時間の予算内で必要なストローク本数を自律的に決定します")
        self.auto_count.toggled.connect(lambda chk: self.count.setEnabled(not chk))
        params_row1.addWidget(self.auto_count)
        gen_layout.addLayout(params_row1)

        style_row = QHBoxLayout()
        style_row.addWidget(QLabel("パレット"))
        self.palette_combo = QComboBox()
        self.palette_combo.addItem("自動 (Auto)", "auto")
        self.palette_combo.addItem("アニメカラー", "anime")
        self.palette_combo.addItem("モノクロ線画", "monochrome")
        self.palette_combo.addItem("サイバーパンク", "cyberpunk")
        self.palette_combo.addItem("自然アースカラー", "nature")
        self.palette_combo.addItem("パステル・ゆめかわ", "pastel")
        self.palette_combo.addItem("透明水彩", "watercolor")
        self.palette_combo.addItem("80s レトロポップ", "retro_pop")
        self.palette_combo.addItem("ダークファンタジー", "dark_fantasy")
        self.palette_combo.addItem("クラシックセピア", "sepia")
        self.palette_combo.addItem("ボタニカル・植物", "botanical")
        self.palette_combo.addItem("水墨画 (墨・朱印)", "sumie")
        self.palette_combo.addItem("サイバーゴールド", "cyber_gold")
        style_row.addWidget(self.palette_combo)

        style_row.addWidget(QLabel("タッチ"))
        self.brush_profile = QComboBox()
        self.brush_profile.addItem("自動 (Auto)", "auto")
        self.brush_profile.addItem("Gペン (筆圧強)", "gpen")
        self.brush_profile.addItem("丸ペン (均一線)", "marupen")
        self.brush_profile.addItem("毛筆・水彩", "brush")
        self.brush_profile.addItem("マーカー", "marker")
        self.brush_profile.addItem("鉛筆・デッサン", "pencil")
        self.brush_profile.addItem("透明水彩タッチ", "watercolor")
        self.brush_profile.addItem("エアブラシ", "airbrush")
        style_row.addWidget(self.brush_profile)
        gen_layout.addLayout(style_row)

        refine_layout = QHBoxLayout()
        self.auto_refine = QCheckBox("段階的ステップ描画 (Auto-Refine)")
        self.auto_refine.setToolTip(
            "下塗り→陰影→線画→ハイライトを段階的に描き、各中間ステップのキャンバス画像をAPIへ送信します。"
        )
        self.auto_refine.setChecked(False)
        refine_layout.addWidget(self.auto_refine)

        self.goal_mode = QCheckBox("目標達成まで継続 (Goal Mode)")
        self.goal_mode.setToolTip(
            "AIが目標達成・完成（goal_reached）を判定するまでステップ描画を自動継続します（安全上限10回 / いつでも停止可能）"
        )
        self.goal_mode.setChecked(False)
        refine_layout.addWidget(self.goal_mode)

        refine_layout.addWidget(QLabel("反復数"))
        self.iterations = QSpinBox()
        self.iterations.setRange(1, 10)
        self.iterations.setValue(3)
        refine_layout.addWidget(self.iterations)
        gen_layout.addLayout(refine_layout)
        tab_main_layout.addWidget(gen_box)

        # 4. ベクタープレビュー & プレビューコントロール
        preview_box = QGroupBox("ストローク・プレビュー")
        preview_box_layout = QVBoxLayout(preview_box)

        preview_bar = QHBoxLayout()
        self.preview_layer_combo = QComboBox()
        self.preview_layer_combo.addItem("全レイヤー表示", "all")
        self.preview_layer_combo.addItem("Lineart (線画のみ)", "lineart")
        self.preview_layer_combo.addItem("Flats (下塗りのみ)", "flats")
        self.preview_layer_combo.addItem("Shading (陰影のみ)", "shading")
        self.preview_layer_combo.currentIndexChanged.connect(self._on_preview_layer_filter_changed)
        preview_bar.addWidget(self.preview_layer_combo)

        self.preview_zoom_in_btn = QPushButton("＋")
        if hasattr(self.preview_zoom_in_btn, "setMaximumWidth"):
            self.preview_zoom_in_btn.setMaximumWidth(28)
        self.preview_zoom_in_btn.clicked.connect(self._preview_zoom_in)
        self.preview_zoom_out_btn = QPushButton("－")
        if hasattr(self.preview_zoom_out_btn, "setMaximumWidth"):
            self.preview_zoom_out_btn.setMaximumWidth(28)
        self.preview_zoom_out_btn.clicked.connect(self._preview_zoom_out)
        self.preview_reset_btn = QPushButton("リセット")
        self.preview_reset_btn.clicked.connect(self._preview_reset)
        preview_bar.addWidget(self.preview_zoom_in_btn)
        preview_bar.addWidget(self.preview_zoom_out_btn)
        preview_bar.addWidget(self.preview_reset_btn)
        preview_box_layout.addLayout(preview_bar)

        self.preview = PreviewWidget(self)
        preview_box_layout.addWidget(self.preview)
        self.quality_summary_label = QLabel("品質診断: プレビュー生成後に表示します")
        self.quality_summary_label.setWordWrap(True)
        preview_box_layout.addWidget(self.quality_summary_label)
        tab_main_layout.addWidget(preview_box)
        tab_main_layout.addStretch(1)

        scroll_main = QScrollArea(self)
        scroll_main.setWidgetResizable(True)
        scroll_main.setWidget(tab_main)
        self.tabs.addTab(scroll_main, "🎨 生成・描画")

        # ==========================================
        # タブ 2: 🖼️ 参照画像 (Image-to-Stroke)
        # ==========================================
        tab_image = QWidget()
        tab_image_layout = QVBoxLayout(tab_image)

        image_box = QGroupBox("参照画像 & 画像解析設定 (Image-to-Stroke)")
        image_layout = QVBoxLayout(image_box)

        img_btn_row = QHBoxLayout()
        self.load_image_btn = QPushButton("画像を選択...")
        self.clear_image_btn = QPushButton("クリア")
        self.clear_image_btn.setEnabled(False)
        self.image_status_label = QLabel("画像なし")
        img_btn_row.addWidget(self.load_image_btn)
        img_btn_row.addWidget(self.clear_image_btn)
        img_btn_row.addWidget(self.image_status_label)
        self.load_image_btn.clicked.connect(self._select_reference_image)
        self.clear_image_btn.clicked.connect(self._clear_reference_image)
        image_layout.addLayout(img_btn_row)
        self.image_privacy_label = QLabel(
            "LLM / Vision モードでは、参照画像を最大1024pxへ縮小し、位置・作者等のメタデータを除去して外部APIへ送信します。"
        )
        self.image_privacy_label.setWordWrap(True)
        image_layout.addWidget(self.image_privacy_label)

        img_params_layout = QFormLayout()
        self.edge_threshold = QDoubleSpinBox()
        self.edge_threshold.setRange(0.02, 0.50)
        self.edge_threshold.setSingleStep(0.02)
        self.edge_threshold.setDecimals(2)
        self.edge_threshold.setValue(0.18)
        self.edge_threshold.setToolTip("エッジ抽出感度 (値が小さいほど微細な線・テクスチャを抽出)")
        img_params_layout.addRow("エッジ感度", self.edge_threshold)

        self.shading_density = QComboBox()
        self.shading_density.addItem("標準 (Medium)", "medium")
        self.shading_density.addItem("高密度 (High)", "high")
        self.shading_density.addItem("低密度 (Low)", "low")
        self.shading_density.addItem("オフ / なし (Off)", "off")
        img_params_layout.addRow("陰影ハッチング", self.shading_density)

        img_opt_row = QHBoxLayout()
        self.enable_flats = QCheckBox("下塗り描画")
        self.enable_flats.setChecked(True)
        self.enable_flats.setToolTip("元画像のカラーブロックによる下塗りストロークを生成します")
        img_opt_row.addWidget(self.enable_flats)

        self.image_color_mode = QComboBox()
        self.image_color_mode.addItem("元画像カラー", "original")
        self.image_color_mode.addItem("選択パレット適用", "palette")
        self.image_color_mode.setToolTip("画像の色をそのまま使うか、指定パレットの色に近似マッピングするかを選択します")
        img_opt_row.addWidget(self.image_color_mode)
        img_params_layout.addRow("カラーモード", img_opt_row)

        image_layout.addLayout(img_params_layout)
        tab_image_layout.addWidget(image_box)
        tab_image_layout.addStretch(1)

        scroll_image = QScrollArea(self)
        scroll_image.setWidgetResizable(True)
        scroll_image.setWidget(tab_image)
        self.tabs.addTab(scroll_image, "🖼️ 参照画像")

        # ==========================================
        # タブ 3: 🤖 AI設定 (AI Planner)
        # ==========================================
        tab_ai = QWidget()
        tab_ai_layout = QVBoxLayout(tab_ai)

        engine_box = QGroupBox("Planner エンジン")
        engine_layout = QVBoxLayout(engine_box)
        self.planner_mode = QComboBox()
        self.planner_mode.addItem("プロシージャル (オフライン 高品質)", "offline")
        self.planner_mode.addItem("OpenAI 互換 LLM / Vision", "openai_compatible")
        self.planner_mode.addItem("🎨 AI 画像生成 -> ストローク (T2I 高品質)", "t2i_stroke")
        engine_layout.addWidget(self.planner_mode)
        tab_ai_layout.addWidget(engine_box)

        # ----------------------------------------------------
        # AI 画像生成 (Text-to-Image) 詳細設定
        # ----------------------------------------------------
        self.t2i_settings = QGroupBox("🎨 AI 画像生成 (Text-to-Image) 設定")
        t2i_layout = QVBoxLayout(self.t2i_settings)
        t2i_form = QFormLayout()

        self.t2i_provider = QComboBox()
        self.t2i_provider.addItem("OpenAI (DALL-E 3 / DALL-E 2)", "openai")
        self.t2i_provider.addItem("Stable Diffusion WebUI (Forge/A1111)", "sd_webui")
        self.t2i_provider.addItem("カスタム HTTP エンドポイント", "custom_http")
        t2i_form.addRow("Provider", self.t2i_provider)

        self.t2i_endpoint = QLineEdit("https://api.openai.com/v1/images/generations")
        self.t2i_endpoint.setPlaceholderText("例: https://api.openai.com/v1/images/generations")
        t2i_form.addRow("エンドポイント", self.t2i_endpoint)

        self.t2i_model = QLineEdit("dall-e-3")
        self.t2i_model.setPlaceholderText("例: dall-e-3, dall-e-2, flux-schnell")
        t2i_form.addRow("Model", self.t2i_model)

        self.t2i_api_key = QLineEdit()
        self.t2i_api_key.setEchoMode(password_echo_mode())
        self.t2i_api_key.setPlaceholderText("空欄なら OPENAI_API_KEY")
        t2i_form.addRow("API Key", self.t2i_api_key)

        self.t2i_size = QComboBox()
        self.t2i_size.addItem("自動 (キャンバス縦横比に合わせる)", "auto")
        self.t2i_size.addItem("1024 x 1024 (正方形 1:1)", "1024x1024")
        self.t2i_size.addItem("1792 x 1024 (横長 16:9)", "1792x1024")
        self.t2i_size.addItem("1024 x 1792 (縦長 9:16)", "1024x1792")
        self.t2i_size.addItem("512 x 512 (SD 1.5 互換)", "512x512")
        t2i_form.addRow("生成サイズ", self.t2i_size)

        self.t2i_negative_prompt = QLineEdit()
        self.t2i_negative_prompt.setPlaceholderText("例: low quality, worst quality, deformed, blurry")
        self.t2i_negative_prompt.setToolTip("SD WebUI などネガティブプロンプト対応エンジンに送信する除外キーワード")
        t2i_form.addRow("ネガティブ指示", self.t2i_negative_prompt)

        t2i_layout.addLayout(t2i_form)
        tab_ai_layout.addWidget(self.t2i_settings)

        self.llm_settings = QGroupBox("OpenAI 互換 LLM / Vision 詳細設定")
        llm_layout = QVBoxLayout(self.llm_settings)

        # クイックプロファイル選択
        profile_box = QGroupBox("クイック設定プロファイル")
        profile_row = QHBoxLayout(profile_box)
        self.profile_openai_btn = QPushButton("OpenAI (gpt-4o)")
        self.profile_openai_btn.clicked.connect(lambda: self._apply_llm_profile("openai"))
        self.profile_ollama_btn = QPushButton("Ollama (ローカル)")
        self.profile_ollama_btn.clicked.connect(lambda: self._apply_llm_profile("ollama"))
        self.profile_lmstudio_btn = QPushButton("LM Studio")
        self.profile_lmstudio_btn.clicked.connect(lambda: self._apply_llm_profile("lmstudio"))
        self.profile_deepseek_btn = QPushButton("DeepSeek")
        self.profile_deepseek_btn.clicked.connect(lambda: self._apply_llm_profile("deepseek"))
        profile_row.addWidget(self.profile_openai_btn)
        profile_row.addWidget(self.profile_ollama_btn)
        profile_row.addWidget(self.profile_lmstudio_btn)
        profile_row.addWidget(self.profile_deepseek_btn)
        llm_layout.addWidget(profile_box)

        llm_form = QFormLayout()
        self.base_url = QLineEdit("https://api.openai.com/v1")
        llm_form.addRow("Base URL", self.base_url)
        self.model = QLineEdit("gpt-4o")
        self.model.setPlaceholderText("例: gpt-4o, o3-mini, deepseek-r1, qwq-32b")
        llm_form.addRow("Model", self.model)
        self.api_key = QLineEdit()
        self.api_key.setEchoMode(password_echo_mode())
        self.api_key.setPlaceholderText("空欄なら OPENAI_API_KEY")
        llm_form.addRow("API Key", self.api_key)

        self.timeout_sec = QSpinBox()
        self.timeout_sec.setRange(10, 600)
        self.timeout_sec.setValue(120)
        self.timeout_sec.setSuffix(" 秒")
        llm_form.addRow("タイムアウト", self.timeout_sec)

        self.max_tokens = QSpinBox()
        self.max_tokens.setRange(512, 131072)
        self.max_tokens.setSingleStep(1024)
        self.max_tokens.setValue(16384)
        self.max_tokens.setSuffix(" tokens")
        self.max_tokens.setToolTip(
            "思考モデル (o1, o3, DeepSeek R1, Gemini Thinking 等) では思考推論トークンと出力 JSON の両方を消費するため 16384〜32768 以上を推奨します"
        )
        llm_form.addRow("Max Tokens", self.max_tokens)

        self.reasoning_effort = QComboBox()
        self.reasoning_effort.addItem("低 (Low: 高速・思考トークン節約)", "low")
        self.reasoning_effort.addItem("中 (Medium: バランス)", "medium")
        self.reasoning_effort.addItem("高 (High: 熟考・複雑な構図)", "high")
        self.reasoning_effort.addItem("オフ / 指定なし (None)", "none")
        self.reasoning_effort.setToolTip("思考モデル（o1/o3/o4/R1等）の思考強度 (reasoning_effort) を設定します")
        llm_form.addRow("Reasoning Effort", self.reasoning_effort)

        sampling_row = QHBoxLayout()
        self.temperature = QDoubleSpinBox()
        self.temperature.setRange(0.0, 2.0)
        self.temperature.setSingleStep(0.1)
        self.temperature.setDecimals(2)
        self.temperature.setValue(0.70)
        self.temperature.setToolTip("生成のランダム性・多様性 (思考モデル時は安全にスキップ)")
        sampling_row.addWidget(QLabel("Temp:"))
        sampling_row.addWidget(self.temperature)

        self.top_p = QDoubleSpinBox()
        self.top_p.setRange(0.1, 1.0)
        self.top_p.setSingleStep(0.05)
        self.top_p.setDecimals(2)
        self.top_p.setValue(1.0)
        sampling_row.addWidget(QLabel("Top P:"))
        sampling_row.addWidget(self.top_p)
        llm_form.addRow("サンプリング", sampling_row)

        self.vision_res = QComboBox()
        self.vision_res.addItem("512 x 512 (標準・高速)", 512)
        self.vision_res.addItem("768 x 768 (高精細)", 768)
        self.vision_res.addItem("1024 x 1024 (超高解像度)", 1024)
        self.vision_res.addItem("256 x 256 (低消費・最速)", 256)
        self.vision_res.setToolTip("自律ビジョン改善ループ (Auto-Refine) 時にAIへ送信するキャンバス解像度")
        llm_form.addRow("Vision 解像度", self.vision_res)

        self.custom_instructions = QLineEdit()
        self.custom_instructions.setPlaceholderText("追加指示（例: 繊細な細線で描いてください）")
        self.custom_instructions.setToolTip("システムプロンプトに追加されるユーザー独自の描画指示")
        llm_form.addRow("追加カスタム指示", self.custom_instructions)

        self.autonomy_mode = QComboBox()
        self.autonomy_mode.addItem("クリエイティブ (高自由度・構図自律決定)", "creative")
        self.autonomy_mode.addItem("バランス (プロンプト重視・柔軟作画)", "balanced")
        self.autonomy_mode.addItem("テンプレート準拠 (固定構図・固定比率)", "template")
        self.autonomy_mode.setToolTip(
            "LLMによるイラスト作画の自律度。クリエイティブでは構図やポーズ、直接プリミティブ作画を最大限に解放します"
        )
        llm_form.addRow("作画の自律度", self.autonomy_mode)

        self.fallback_to_procedural = QCheckBox("API失敗時にオフライン生成へフォールバック")
        self.fallback_to_procedural.setChecked(False)
        self.fallback_to_procedural.setToolTip("有効時のみ、LLM生成に失敗した場合にプロシージャル描画へ切り替えます")
        llm_form.addRow("障害時の動作", self.fallback_to_procedural)

        self.test_conn_btn = QPushButton("API 接続テスト")
        self.test_conn_btn.clicked.connect(self._test_api_connection)
        llm_form.addRow("", self.test_conn_btn)
        llm_layout.addLayout(llm_form)

        tab_ai_layout.addWidget(self.llm_settings)
        tab_ai_layout.addStretch(1)

        scroll_ai = QScrollArea(self)
        scroll_ai.setWidgetResizable(True)
        scroll_ai.setWidget(tab_ai)
        self.tabs.addTab(scroll_ai, "🤖 AI設定")

        # ==========================================
        # タブ 4: ⚙️ レイヤー・詳細 (Settings & Debug)
        # ==========================================
        tab_settings = QWidget()
        tab_settings_layout = QVBoxLayout(tab_settings)

        # ブラシ・描画 & レイヤー設定
        brush_box = QGroupBox("🖌️ ブラシ・描画 & レイヤー設定")
        brush_form = QFormLayout(brush_box)

        scale_row = QHBoxLayout()
        self.brush_size_multiplier = QDoubleSpinBox()
        self.brush_size_multiplier.setRange(0.1, 5.0)
        self.brush_size_multiplier.setSingleStep(0.1)
        self.brush_size_multiplier.setDecimals(2)
        self.brush_size_multiplier.setValue(1.0)
        self.brush_size_multiplier.setSuffix(" x")
        self.brush_size_multiplier.setToolTip("ストロークの太さを一括スケーリングします")
        scale_row.addWidget(QLabel("太さ倍率:"))
        scale_row.addWidget(self.brush_size_multiplier)

        self.opacity_multiplier = QSpinBox()
        self.opacity_multiplier.setRange(10, 100)
        self.opacity_multiplier.setSingleStep(5)
        self.opacity_multiplier.setValue(100)
        self.opacity_multiplier.setSuffix(" %")
        self.opacity_multiplier.setToolTip("ストローク全体の不透明度を一括スケーリングします")
        scale_row.addWidget(QLabel("不透明度:"))
        scale_row.addWidget(self.opacity_multiplier)
        brush_form.addRow("描画スケーリング", scale_row)

        layer_row = QHBoxLayout()
        self.layer_mode = QComboBox()
        self.layer_mode.addItem("マルチレイヤー分割 (Draft/Flats/Lineart/etc)", "multi_layer")
        self.layer_mode.addItem("現在のアクティブレイヤーに直接描画", "active_layer")
        self.layer_mode.addItem("単一の新規レイヤーにまとめて描画", "single_layer")
        self.layer_mode.setToolTip(
            "マルチレイヤーが推奨です。アクティブレイヤー直接描画は安全な取消のため画素スナップショットを取得します。"
        )
        layer_row.addWidget(self.layer_mode)
        brush_form.addRow("レイヤー出力", layer_row)

        config_row = QHBoxLayout()
        self.layer_prefix = QLineEdit("AI Artwork")
        self.layer_prefix.setPlaceholderText("レイヤー名 / グループ名")
        config_row.addWidget(QLabel("名前:"))
        config_row.addWidget(self.layer_prefix)

        self.event_interval = QSpinBox()
        self.event_interval.setRange(5, 100)
        self.event_interval.setValue(30)
        self.event_interval.setToolTip(
            "何線分ごとに画面イベントを処理するか (値が小さいほどリアルタイム、大きいほど高速)"
        )
        config_row.addWidget(QLabel("画面更新間隔（線分）:"))
        config_row.addWidget(self.event_interval)
        brush_form.addRow("キャンバス設定", config_row)
        tab_settings_layout.addWidget(brush_box)

        # 保存 & 動作オプション
        save_box = QGroupBox("保存 & 動作オプション")
        save_box_layout = QVBoxLayout(save_box)
        save_layout = QHBoxLayout()
        self.save_json = QCheckBox("計画 JSON 保存")
        self.save_json.setChecked(True)
        self.save_svg_chk = QCheckBox("SVG 保存")
        self.save_svg_chk.setChecked(True)
        save_layout.addWidget(self.save_json)
        save_layout.addWidget(self.save_svg_chk)
        save_box_layout.addLayout(save_layout)

        self.confirm_before_apply = QCheckBox("適用前にプレビューを確認")
        self.confirm_before_apply.setChecked(True)
        self.confirm_before_apply.setToolTip("生成計画を確認してから「キャンバスへ適用」を押す安全モード")
        save_box_layout.addWidget(self.confirm_before_apply)
        tab_settings_layout.addWidget(save_box)

        # デバッグ & 設定リセット
        debug_toggle_layout = QHBoxLayout()
        self.debug_mode_chk = QCheckBox("🐞 デバッグモード (詳細ログを表示)")
        self.debug_mode_chk.setChecked(False)
        debug_toggle_layout.addWidget(self.debug_mode_chk)

        self.reset_defaults_btn = QPushButton("🔄 初期設定に戻す")
        self.reset_defaults_btn.clicked.connect(self._reset_to_defaults)
        debug_toggle_layout.addWidget(self.reset_defaults_btn)
        tab_settings_layout.addLayout(debug_toggle_layout)

        # デバッグログパネル
        self.debug_box = QGroupBox("デバッグログ (リアルタイム通信・処理ログ)")
        debug_box_layout = QVBoxLayout(self.debug_box)
        self.debug_log_edit = QPlainTextEdit()
        self.debug_log_edit.setReadOnly(True)
        self.debug_log_edit.setMaximumHeight(140)
        if hasattr(self.debug_log_edit, "document"):
            log_document = self.debug_log_edit.document()
            if log_document is not None and hasattr(log_document, "setMaximumBlockCount"):
                log_document.setMaximumBlockCount(5_000)
        debug_box_layout.addWidget(self.debug_log_edit)

        debug_btn_layout = QHBoxLayout()
        self.copy_log_btn = QPushButton("📋 ログをコピー")
        self.clear_log_btn = QPushButton("🗑️ クリア")
        self.save_log_btn = QPushButton("💾 ログを保存...")
        debug_btn_layout.addWidget(self.copy_log_btn)
        debug_btn_layout.addWidget(self.clear_log_btn)
        debug_btn_layout.addWidget(self.save_log_btn)
        debug_box_layout.addLayout(debug_btn_layout)

        self.copy_log_btn.clicked.connect(self._copy_debug_log)
        self.clear_log_btn.clicked.connect(self._clear_debug_log)
        self.save_log_btn.clicked.connect(self._save_debug_log)
        self.debug_mode_chk.toggled.connect(self._toggle_debug_panel)

        self.debug_box.setVisible(False)
        tab_settings_layout.addWidget(self.debug_box)
        tab_settings_layout.addStretch(1)

        scroll_settings = QScrollArea(self)
        scroll_settings.setWidgetResizable(True)
        scroll_settings.setWidget(tab_settings)
        self.tabs.addTab(scroll_settings, "⚙️ レイヤー・詳細")

        # ==========================================
        # 下部固定アクションバー (Execution Controls)
        # ==========================================
        self.run_btn = QPushButton("プレビュー生成")
        self.run_btn.setShortcut("Ctrl+Return")
        self.run_btn.setToolTip("描画計画を生成またはキャンバスへ直接描画します (Ctrl+Enter)")
        self.apply_btn = QPushButton("キャンバスへ適用")
        self.apply_btn.setEnabled(False)
        self.apply_btn.setShortcut("Ctrl+Shift+Return")
        self.apply_btn.setToolTip("プレビュー確認済みの計画をキャンバスへ適用します (Ctrl+Shift+Enter)")
        self.stop_btn = QPushButton("停止")
        self.stop_btn.setEnabled(False)
        self.stop_btn.setShortcut("Escape")
        self.stop_btn.setToolTip("実行中の処理を停止します (Esc)")
        btn_layout = QHBoxLayout()
        btn_layout.addWidget(self.run_btn)
        btn_layout.addWidget(self.apply_btn)
        btn_layout.addWidget(self.stop_btn)

        # プログレスバー & ステータス
        self.progress = QProgressBar()
        self.progress.setRange(0, 1)
        self.progress.setValue(0)
        self.status = QLabel("待機中: プロンプトまたはプリセットを選んで描画を開始してください")
        self.status.setWordWrap(True)

        dock_container = QWidget(self)
        dock_layout = QVBoxLayout(dock_container)
        dock_layout.addWidget(self.tabs)
        dock_layout.addLayout(btn_layout)
        dock_layout.addWidget(self.progress)
        dock_layout.addWidget(self.status)
        self.setWidget(dock_container)

        # 設定の復元
        self._load_settings()

        # イベント接続
        self.run_btn.clicked.connect(self.run)
        self.apply_btn.clicked.connect(self._apply_pending_plan)
        self.stop_btn.clicked.connect(self.cancel)
        self.confirm_before_apply.toggled.connect(self._update_action_buttons_state)
        self.planner_mode.currentIndexChanged.connect(self._update_planner_settings_state)
        self.brush_size_multiplier.valueChanged.connect(self._update_preview_multipliers)
        self.opacity_multiplier.valueChanged.connect(self._update_preview_multipliers)
        self._update_planner_settings_state()
        self._update_action_buttons_state()
        self._setup_accessibility()

    def _on_preview_layer_filter_changed(self, _index: int = 0) -> None:
        combo = _get_attr(self, "preview_layer_combo")
        preview = _get_attr(self, "preview")
        if combo is None or preview is None or not hasattr(preview, "set_layer_filter"):
            return
        val = combo.currentData() if hasattr(combo, "currentData") else None
        if not val or val == "all":
            preview.set_layer_filter(None)
        elif val == "lineart":
            preview.set_layer_filter({"Lineart"})
        elif val == "flats":
            preview.set_layer_filter({"Flats"})
        elif val == "shading":
            preview.set_layer_filter({"Shading"})

    def _preview_zoom_in(self) -> None:
        preview = _get_attr(self, "preview")
        if preview is not None and hasattr(preview, "set_zoom_factor"):
            cur = float(_get_attr(preview, "_zoom_factor", 1.0))
            preview.set_zoom_factor(cur * 1.25)

    def _preview_zoom_out(self) -> None:
        preview = _get_attr(self, "preview")
        if preview is not None and hasattr(preview, "set_zoom_factor"):
            cur = float(_get_attr(preview, "_zoom_factor", 1.0))
            preview.set_zoom_factor(cur / 1.25)

    def _preview_reset(self) -> None:
        preview = _get_attr(self, "preview")
        if preview is not None and hasattr(preview, "reset_view"):
            preview.reset_view()

    def _clear_prompt(self) -> None:
        """プロンプト入力欄を初期化・クリアする。"""
        prompt_w = _get_attr(self, "prompt")
        if prompt_w is not None and hasattr(prompt_w, "clear"):
            prompt_w.clear()

    def _populate_prompt_history(self) -> None:
        """QSettings から直近のプロンプト履歴を読み込みコンボボックスを初期化する。"""
        combo = _get_attr(self, "prompt_history_combo")
        if combo is None or not hasattr(combo, "clear"):
            return
        combo.clear()
        combo.addItem("📋 履歴から選択...", "")
        if callable(QSettings):
            with contextlib.suppress(Exception):
                settings: Any = QSettings("AIStrokePainter", "DockerSettings")
                raw = settings.value("prompt_history_json")
                if raw:
                    history = json.loads(str(raw))
                    if isinstance(history, list):
                        for item in history:
                            if isinstance(item, str) and item.strip():
                                short_label = item[:35] + ("..." if len(item) > 35 else "")
                                combo.addItem(f"🕒 {short_label}", item)

    def _save_prompt_to_history(self, prompt_text: str) -> None:
        """成功または実行されたプロンプトを履歴リストの先頭に保存する（最大10件）。"""
        text = prompt_text.strip()
        if not text or not callable(QSettings):
            return
        with contextlib.suppress(Exception):
            settings: Any = QSettings("AIStrokePainter", "DockerSettings")
            raw = settings.value("prompt_history_json")
            loaded: Any = json.loads(str(raw)) if raw else []
            history: list[str] = [str(x) for x in loaded] if isinstance(loaded, list) else []
            if text in history:
                history.remove(text)
            history.insert(0, text)
            history = history[:10]
            settings.setValue("prompt_history_json", json.dumps(history, ensure_ascii=False))
            if hasattr(settings, "sync"):
                settings.sync()
            self._populate_prompt_history()

    def _on_prompt_history_selected(self, index: int) -> None:
        """履歴コンボボックスからプロンプトが選択されたら入力欄へ展開する。"""
        combo = _get_attr(self, "prompt_history_combo")
        if combo is None or index <= 0 or not hasattr(combo, "itemData"):
            return
        data = combo.itemData(index)
        if data and isinstance(data, str):
            prompt_w = _get_attr(self, "prompt")
            if prompt_w is not None and hasattr(prompt_w, "setPlainText"):
                prompt_w.setPlainText(data)

    def _add_prompt_tag(self, tag: str) -> None:
        """プロンプト入力欄の末尾に指定されたタグキーワードを追加する。"""
        prompt_w = _get_attr(self, "prompt")
        if prompt_w is None or not hasattr(prompt_w, "toPlainText") or not hasattr(prompt_w, "setPlainText"):
            return
        current = prompt_w.toPlainText().strip()
        if current:
            if tag not in current:
                prompt_w.setPlainText(f"{current}, {tag}")
        else:
            prompt_w.setPlainText(tag)

    def _apply_llm_profile(self, profile_key: str) -> None:
        """各LLMプロバイダ向けの推奨プリセットを一括適用する。"""
        profiles: dict[str, dict[str, Any]] = {
            "openai": {
                "base_url": "https://api.openai.com/v1",
                "model": "gpt-4o",
                "max_tokens": 16384,
                "reasoning_effort": "medium",
                "temperature": 0.70,
            },
            "ollama": {
                "base_url": "http://127.0.0.1:11434/v1",
                "model": "llama3.2-vision",
                "max_tokens": 8192,
                "reasoning_effort": "none",
                "temperature": 0.70,
            },
            "lmstudio": {
                "base_url": "http://127.0.0.1:1234/v1",
                "model": "qwen2.5-coder-7b-instruct",
                "max_tokens": 8192,
                "reasoning_effort": "none",
                "temperature": 0.70,
            },
            "deepseek": {
                "base_url": "https://api.deepseek.com/v1",
                "model": "deepseek-chat",
                "max_tokens": 8192,
                "reasoning_effort": "none",
                "temperature": 0.70,
            },
        }
        cfg = profiles.get(profile_key)
        if not cfg:
            return
        if hasattr(self, "planner_mode") and hasattr(self.planner_mode, "count"):
            for i in range(self.planner_mode.count()):
                if self.planner_mode.itemData(i) == "openai_compatible":
                    self.planner_mode.setCurrentIndex(i)
                    break
        if hasattr(self, "base_url") and hasattr(self.base_url, "setText"):
            self.base_url.setText(cfg["base_url"])
        if hasattr(self, "model") and hasattr(self.model, "setText"):
            self.model.setText(cfg["model"])
        if hasattr(self, "max_tokens") and hasattr(self.max_tokens, "setValue"):
            self.max_tokens.setValue(cfg["max_tokens"])
        if hasattr(self, "temperature") and hasattr(self.temperature, "setValue"):
            self.temperature.setValue(cfg["temperature"])
        if hasattr(self, "reasoning_effort") and hasattr(self.reasoning_effort, "count"):
            for i in range(self.reasoning_effort.count()):
                if self.reasoning_effort.itemData(i) == cfg["reasoning_effort"]:
                    self.reasoning_effort.setCurrentIndex(i)
                    break
        self._log_debug(f"[LLMプロファイル適用] '{profile_key}' の推奨設定を適用しました")

    def canvasChanged(self, canvas: Any) -> None:  # noqa: N802
        """Kritaからキャンバス切り替えイベント通知を受け取る (DockWidgetの必須抽象メソッド)。"""
        self._canvas = canvas

    def closeEvent(self, event: Any) -> None:  # noqa: N802
        """設定を保存し、バックグラウンド処理と描画セッションを安全に終了する。"""
        self._closing = True
        self._save_settings()
        workers_stopped = True
        for worker_name in ("_worker", "_connection_worker"):
            worker = _get_attr(self, worker_name)
            if worker is None:
                continue
            cancel = getattr(worker, "cancel", None)
            if callable(cancel):
                try:
                    cancel()
                except Exception:
                    workers_stopped = False
            wait_result: Any = None
            wait = getattr(worker, "wait", None)
            if callable(wait):
                try:
                    wait_result = wait(1_500)
                except Exception:
                    workers_stopped = False
            is_running = getattr(worker, "isRunning", None)
            if callable(is_running):
                try:
                    if bool(is_running()):
                        workers_stopped = False
                except Exception:
                    workers_stopped = False
            elif wait_result is False:
                workers_stopped = False

        if not workers_stopped:
            self._closing = False
            status = _get_attr(self, "status")
            if status is not None and hasattr(status, "setText"):
                status.setText("バックグラウンド処理の終了を待っています。完了後にもう一度閉じてください。")
            ignore = getattr(event, "ignore", None)
            if callable(ignore):
                ignore()
            return

        if not self._finish_canvas_session(False):
            self._closing = False
            ignore = getattr(event, "ignore", None)
            if callable(ignore):
                ignore()
            return
        with contextlib.suppress(Exception):
            super().closeEvent(event)

    def _update_preview_multipliers(self, *_args: Any) -> None:
        prev = _get_attr(self, "preview")
        if prev is not None and hasattr(prev, "update_multipliers"):
            bs_widget = _get_attr(self, "brush_size_multiplier")
            op_widget = _get_attr(self, "opacity_multiplier")
            size_val = bs_widget.value() if bs_widget is not None and hasattr(bs_widget, "value") else 1.0
            op_val = (op_widget.value() / 100.0) if op_widget is not None and hasattr(op_widget, "value") else 1.0
            prev.update_multipliers(
                size_multiplier=size_val,
                opacity_multiplier=op_val,
            )

    def _populate_presets(self, selected_title: str | None = None) -> None:
        """ビルトインプリセットと QSettings 保存済みカスタムプリセットをコンボボックスに読み込む。"""
        combo = _get_attr(self, "preset_combo")
        if combo is None or not hasattr(combo, "clear"):
            return
        combo.clear()
        for item in self.PRESETS:
            title = item[0]
            combo.addItem(title, item)

        # カスタムプリセットの読み込み
        if callable(QSettings):
            with contextlib.suppress(Exception):
                settings: Any = QSettings("AIStrokePainter", "CustomPresets")
                raw_json = settings.value("presets_json")
                if raw_json:
                    custom_map: dict[str, Any] = json.loads(str(raw_json))
                    for name, data in custom_map.items():
                        combo.addItem(f"⭐ [カスタム] {name}", data)

        if selected_title:
            for i in range(combo.count()):
                if combo.itemText(i) == selected_title:
                    combo.setCurrentIndex(i)
                    break

    def _save_custom_preset(self) -> None:
        """現在のプロンプト・パレット・本数・ブラシ設定などを名前を付けてカスタムプリセットに保存する。"""
        preset_name, ok = QInputDialog.getText(self, "プリセット保存", "保存するプリセット名を入力してください:")
        if not ok or not preset_name or not preset_name.strip():
            return

        name = preset_name.strip()
        if name.startswith("⭐ [カスタム] "):
            name = name[len("⭐ [カスタム] ") :].strip()
        if not name or len(name) > 100 or any(ord(char) < 32 for char in name):
            QMessageBox.warning(self, "プリセット保存", "プリセット名は制御文字を含まない100文字以下にしてください。")
            return
        prompt_w = _get_attr(self, "prompt")
        pal_w = _get_attr(self, "palette_combo")
        count_w = _get_attr(self, "count")
        prof_w = _get_attr(self, "brush_profile")
        bs_w = _get_attr(self, "brush_size_multiplier")
        op_w = _get_attr(self, "opacity_multiplier")
        auto_count_w = _get_attr(self, "auto_count")

        data = {
            "prompt": prompt_w.toPlainText() if prompt_w is not None and hasattr(prompt_w, "toPlainText") else "",
            "palette": pal_w.currentData() if pal_w is not None and hasattr(pal_w, "currentData") else "auto",
            "count": count_w.value() if count_w is not None and hasattr(count_w, "value") else 35,
            "brush_profile": prof_w.currentData() if prof_w is not None and hasattr(prof_w, "currentData") else "auto",
            "brush_size": bs_w.value() if bs_w is not None and hasattr(bs_w, "value") else 1.0,
            "opacity": op_w.value() if op_w is not None and hasattr(op_w, "value") else 100,
            "auto_count": bool(auto_count_w.isChecked())
            if auto_count_w is not None and hasattr(auto_count_w, "isChecked")
            else False,
            "custom": True,
        }

        if callable(QSettings):
            try:
                settings: Any = QSettings("AIStrokePainter", "CustomPresets")
                raw_json = settings.value("presets_json")
                loaded = json.loads(str(raw_json)) if raw_json else {}
                if not isinstance(loaded, dict):
                    raise ValueError("保存済みプリセットの形式が不正です")
                custom_map: dict[str, Any] = loaded
                if name in custom_map and not _confirm(
                    self, "プリセット上書き確認", f"カスタムプリセット '{name}' を上書きしますか？"
                ):
                    return
                custom_map[name] = data
                settings.setValue("presets_json", json.dumps(custom_map, ensure_ascii=False))
                if hasattr(settings, "sync"):
                    settings.sync()
            except Exception as exc:
                self._log_debug(f"[プリセット保存失敗] {exc}")
                QMessageBox.critical(self, "プリセット保存", f"カスタムプリセットを保存できませんでした: {exc}")
                return
        else:
            QMessageBox.critical(self, "プリセット保存", "この環境では設定ストレージを利用できません。")
            return

        self._populate_presets(selected_title=f"⭐ [カスタム] {name}")
        self._log_debug(f"[プリセット保存] カスタムプリセット '{name}' を保存しました")
        QMessageBox.information(self, "プリセット保存", f"カスタムプリセット '{name}' を保存しました。")

    def _delete_custom_preset(self) -> None:
        """選択中のカスタムプリセットを削除する。"""
        combo = _get_attr(self, "preset_combo")
        if combo is None or not hasattr(combo, "currentText"):
            return
        current_text = combo.currentText()
        if not current_text.startswith("⭐ [カスタム] "):
            QMessageBox.information(self, "プリセット削除", "ビルトインプリセットは削除できません。")
            return

        name = current_text
        if name.startswith("⭐ [カスタム] "):
            name = name[len("⭐ [カスタム] ") :].strip()
        if not _confirm(self, "プリセット削除確認", f"カスタムプリセット '{name}' を削除しますか？"):
            return
        if callable(QSettings):
            try:
                settings: Any = QSettings("AIStrokePainter", "CustomPresets")
                raw_json = settings.value("presets_json")
                if raw_json:
                    loaded = json.loads(str(raw_json))
                    if not isinstance(loaded, dict):
                        raise ValueError("保存済みプリセットの形式が不正です")
                    custom_map: dict[str, Any] = loaded
                    custom_map.pop(name, None)
                    settings.setValue("presets_json", json.dumps(custom_map, ensure_ascii=False))
                    if hasattr(settings, "sync"):
                        settings.sync()
            except Exception as exc:
                self._log_debug(f"[プリセット削除失敗] {exc}")
                QMessageBox.critical(self, "プリセット削除", f"カスタムプリセットを削除できませんでした: {exc}")
                return
        else:
            QMessageBox.critical(self, "プリセット削除", "この環境では設定ストレージを利用できません。")
            return

        self._populate_presets()
        self._log_debug(f"[プリセット削除] カスタムプリセット '{name}' を削除しました")
        QMessageBox.information(self, "プリセット削除", f"カスタムプリセット '{name}' を削除しました。")

    def _export_presets(self) -> None:
        """保存済みカスタムプリセットを JSON ファイルとしてエクスポートする。"""
        if not callable(QSettings) or not callable(QFileDialog):
            QMessageBox.critical(self, "プリセット出力", "この環境ではファイル出力機能を利用できません。")
            return
        try:
            settings: Any = QSettings("AIStrokePainter", "CustomPresets")
            raw_json = settings.value("presets_json")
            if not raw_json:
                QMessageBox.information(self, "プリセット出力", "エクスポート可能なカスタムプリセットがありません。")
                return
            loaded = json.loads(str(raw_json))
            if not isinstance(loaded, dict) or not loaded:
                QMessageBox.information(self, "プリセット出力", "エクスポート可能なカスタムプリセットがありません。")
                return
        except Exception as exc:
            self._log_debug(f"[プリセット出力失敗] {exc}")
            QMessageBox.critical(self, "プリセット出力", f"プリセットの読み出しに失敗しました: {exc}")
            return

        file_path, _ = _safe_get_save_filename(
            self, "カスタムプリセットを保存", "custom_presets.json", "JSON Files (*.json)"
        )
        if not file_path:
            return
        if not file_path.lower().endswith(".json"):
            file_path += ".json"
        try:
            with open(file_path, "w", encoding="utf-8") as f:
                json.dump(loaded, f, ensure_ascii=False, indent=2)
            self._log_debug(f"[プリセット出力成功] {Path(file_path).name}")
            QMessageBox.information(self, "プリセット出力", f"カスタムプリセットを出力しました:\n{file_path}")
        except Exception as exc:
            self._log_debug(f"[プリセット出力ファイル保存失敗] {exc}")
            QMessageBox.critical(self, "プリセット出力", f"ファイルの保存に失敗しました: {exc}")

    def _import_presets(self) -> None:
        """JSON ファイルからカスタムプリセットを読み込んでインポート・マージする。"""
        if not callable(QSettings):
            QMessageBox.critical(self, "プリセット読込", "この環境ではファイル読込機能を利用できません。")
            return
        file_path, _ = _safe_get_open_filename(self, "カスタムプリセットを読込", "", "JSON Files (*.json)")
        if not file_path:
            return
        try:
            source_path = Path(file_path)
            if source_path.stat().st_size > 10 * 1024 * 1024:
                raise ValueError("プリセットファイルのサイズが上限(10MB)を超えています")
            with open(file_path, encoding="utf-8") as f:
                imported_data = json.load(f)
            if not isinstance(imported_data, dict):
                raise ValueError("JSONのルートがオブジェクト(辞書)ではありません")
            # プリセット構造の検証
            valid_presets: dict[str, Any] = {}
            for k, v in imported_data.items():
                if not isinstance(k, str) or not isinstance(v, dict):
                    continue
                k_clean = k.strip()
                if k_clean.startswith("⭐ [カスタム] "):
                    k_clean = k_clean[len("⭐ [カスタム] ") :].strip()
                if not k_clean or len(k_clean) > 100 or any(ord(c) < 32 for c in k_clean):
                    continue
                if "prompt" not in v or not isinstance(v.get("prompt"), str):
                    continue
                valid_presets[k_clean] = v
                if len(valid_presets) >= 100:
                    break
            if not valid_presets:
                raise ValueError(
                    "有効なプリセットデータが見つかりませんでした（各プリセットは100文字以内の名前とpromptを含む必要があります）"
                )

            settings: Any = QSettings("AIStrokePainter", "CustomPresets")
            raw_json = settings.value("presets_json")
            existing = json.loads(str(raw_json)) if raw_json else {}
            if not isinstance(existing, dict):
                existing = {}
            existing.update(valid_presets)
            settings.setValue("presets_json", json.dumps(existing, ensure_ascii=False))
            if hasattr(settings, "sync"):
                settings.sync()
            self._populate_presets()
            self._log_debug(f"[プリセット読込成功] {len(valid_presets)} 個のプリセットをマージしました")
            QMessageBox.information(
                self, "プリセット読込", f"{len(valid_presets)} 個のカスタムプリセットを読み込みました。"
            )
        except Exception as exc:
            self._log_debug(f"[プリセット読込失敗] {exc}")
            QMessageBox.critical(self, "プリセット読込", f"プリセットの読み込みに失敗しました: {exc}")

    def _reset_to_defaults(self) -> None:
        """全設定値を標準デフォルト値にリセットする。"""
        if not _confirm(self, "設定リセット", "すべての設定を初期値に戻しますか？"):
            return
        w = _get_attr(self, "planner_mode")
        if w is not None and hasattr(w, "setCurrentIndex"):
            w.setCurrentIndex(0)
        w = _get_attr(self, "api_key")
        if w is not None and hasattr(w, "clear"):
            w.clear()
        w = _get_attr(self, "prompt")
        if w is not None and hasattr(w, "setPlainText"):
            w.setPlainText("anime girl portrait, delicate eyes, flowing hair")
        w = _get_attr(self, "seed")
        if w is not None and hasattr(w, "setValue"):
            w.setValue(42)
        w = _get_attr(self, "count")
        if w is not None and hasattr(w, "setValue"):
            w.setValue(35)
        w = _get_attr(self, "palette_combo")
        if w is not None and hasattr(w, "setCurrentIndex"):
            w.setCurrentIndex(0)
        w = _get_attr(self, "brush_profile")
        if w is not None and hasattr(w, "setCurrentIndex"):
            w.setCurrentIndex(0)
        w = _get_attr(self, "auto_refine")
        if w is not None and hasattr(w, "setChecked"):
            w.setChecked(False)
        w = _get_attr(self, "iterations")
        if w is not None and hasattr(w, "setValue"):
            w.setValue(3)
        w = _get_attr(self, "brush_size_multiplier")
        if w is not None and hasattr(w, "setValue"):
            w.setValue(1.0)
        w = _get_attr(self, "opacity_multiplier")
        if w is not None and hasattr(w, "setValue"):
            w.setValue(100)
        w = _get_attr(self, "layer_mode")
        if w is not None and hasattr(w, "setCurrentIndex"):
            w.setCurrentIndex(0)
        w = _get_attr(self, "layer_prefix")
        if w is not None and hasattr(w, "setText"):
            w.setText("AI Artwork")
        w = _get_attr(self, "event_interval")
        if w is not None and hasattr(w, "setValue"):
            w.setValue(30)
        w = _get_attr(self, "edge_threshold")
        if w is not None and hasattr(w, "setValue"):
            w.setValue(0.18)
        w = _get_attr(self, "shading_density")
        if w is not None and hasattr(w, "setCurrentIndex"):
            w.setCurrentIndex(0)
        w = _get_attr(self, "enable_flats")
        if w is not None and hasattr(w, "setChecked"):
            w.setChecked(True)
        w = _get_attr(self, "image_color_mode")
        if w is not None and hasattr(w, "setCurrentIndex"):
            w.setCurrentIndex(0)
        w = _get_attr(self, "auto_seed")
        if w is not None and hasattr(w, "setChecked"):
            w.setChecked(False)
        w = _get_attr(self, "auto_count")
        if w is not None and hasattr(w, "setChecked"):
            w.setChecked(False)
        w = _get_attr(self, "goal_mode")
        if w is not None and hasattr(w, "setChecked"):
            w.setChecked(False)
        w = _get_attr(self, "base_url")
        if w is not None and hasattr(w, "setText"):
            w.setText("https://api.openai.com/v1")
        w = _get_attr(self, "model")
        if w is not None and hasattr(w, "setText"):
            w.setText("gpt-4o")
        w = _get_attr(self, "timeout_sec")
        if w is not None and hasattr(w, "setValue"):
            w.setValue(120)
        w = _get_attr(self, "max_tokens")
        if w is not None and hasattr(w, "setValue"):
            w.setValue(16384)
        w = _get_attr(self, "reasoning_effort")
        if w is not None and hasattr(w, "setCurrentIndex"):
            w.setCurrentIndex(0)
        w = _get_attr(self, "temperature")
        if w is not None and hasattr(w, "setValue"):
            w.setValue(0.70)
        w = _get_attr(self, "top_p")
        if w is not None and hasattr(w, "setValue"):
            w.setValue(1.0)
        w = _get_attr(self, "vision_res")
        if w is not None and hasattr(w, "setCurrentIndex"):
            w.setCurrentIndex(0)
        w = _get_attr(self, "custom_instructions")
        if w is not None and hasattr(w, "setText"):
            w.setText("")
        w = _get_attr(self, "autonomy_mode")
        if w is not None and hasattr(w, "setCurrentIndex"):
            w.setCurrentIndex(0)
        w = _get_attr(self, "t2i_provider")
        if w is not None and hasattr(w, "setCurrentIndex"):
            w.setCurrentIndex(0)
        w = _get_attr(self, "t2i_endpoint")
        if w is not None and hasattr(w, "setText"):
            w.setText("https://api.openai.com/v1/images/generations")
        w = _get_attr(self, "t2i_model")
        if w is not None and hasattr(w, "setText"):
            w.setText("dall-e-3")
        w = _get_attr(self, "t2i_api_key")
        if w is not None and hasattr(w, "clear"):
            w.clear()
        w = _get_attr(self, "t2i_size")
        if w is not None and hasattr(w, "setCurrentIndex"):
            w.setCurrentIndex(0)
        w = _get_attr(self, "t2i_negative_prompt")
        if w is not None and hasattr(w, "setText"):
            w.setText("")
        w = _get_attr(self, "fallback_to_procedural")
        if w is not None and hasattr(w, "setChecked"):
            w.setChecked(False)
        w = _get_attr(self, "save_json")
        if w is not None and hasattr(w, "setChecked"):
            w.setChecked(True)
        w = _get_attr(self, "save_svg_chk")
        if w is not None and hasattr(w, "setChecked"):
            w.setChecked(True)
        w = _get_attr(self, "confirm_before_apply")
        if w is not None and hasattr(w, "setChecked"):
            w.setChecked(True)
        w = _get_attr(self, "debug_mode_chk")
        if w is not None and hasattr(w, "setChecked"):
            w.setChecked(False)
        self._save_settings()
        self._update_action_buttons_state()
        self._log_debug("[設定リセット] 全設定を初期値に戻しました")

    def _load_settings(self) -> None:
        """QSettings から前回の UI 設定値を自動復元する。"""
        if QSettings is None or not callable(QSettings):
            return
        try:
            settings: Any = QSettings("AIStrokePainter", "DockerSettings")
        except Exception as exc:
            self._log_debug(f"[設定復元警告] QSettings を開けませんでした: {exc}")
            return

        def restore(key: str, apply_value: Any) -> None:
            try:
                value = settings.value(key)
                if value is not None:
                    apply_value(value)
            except Exception as exc:
                # 破損した1項目だけを既定値のまま残し、後続項目の復元は継続する。
                self._log_debug(f"[設定復元警告] {key} を復元できないため既定値を使用します: {exc}")

        def set_text(name: str, value: Any, *, plain: bool = False) -> None:
            widget = _get_attr(self, name)
            if widget is None:
                return
            if plain and hasattr(widget, "setPlainText"):
                widget.setPlainText(str(value))
            elif hasattr(widget, "setText"):
                widget.setText(str(value))

        def set_number(name: str, value: Any, converter: Any) -> None:
            widget = _get_attr(self, name)
            if widget is not None and hasattr(widget, "setValue"):
                widget.setValue(converter(value))

        def set_checked(name: str, value: Any) -> None:
            widget = _get_attr(self, name)
            if widget is not None and hasattr(widget, "setChecked"):
                widget.setChecked(str(value).lower() in ("true", "1"))

        def set_combo(name: str, value: Any, converter: Any = str) -> None:
            widget = _get_attr(self, name)
            if widget is None or not hasattr(widget, "count") or not hasattr(widget, "itemData"):
                return
            expected = converter(value)
            for index in range(widget.count()):
                if converter(widget.itemData(index)) == expected:
                    widget.setCurrentIndex(index)
                    return

        restore("planner_mode", lambda value: set_combo("planner_mode", value))
        restore("t2i_provider", lambda value: set_combo("t2i_provider", value))
        restore("t2i_endpoint", lambda value: set_text("t2i_endpoint", value))
        restore("t2i_model", lambda value: set_text("t2i_model", value))
        restore("t2i_size", lambda value: set_combo("t2i_size", value))
        restore("t2i_negative_prompt", lambda value: set_text("t2i_negative_prompt", value))
        restore("base_url", lambda value: set_text("base_url", value))
        restore("model", lambda value: set_text("model", value))
        restore("timeout_sec", lambda value: set_number("timeout_sec", value, int))
        restore("max_tokens", lambda value: set_number("max_tokens", value, int))
        restore("reasoning_effort", lambda value: set_combo("reasoning_effort", value))
        restore("temperature", lambda value: set_number("temperature", value, float))
        restore("top_p", lambda value: set_number("top_p", value, float))
        restore("vision_res", lambda value: set_combo("vision_res", value, int))
        restore("custom_instructions", lambda value: set_text("custom_instructions", value))
        restore("autonomy_mode", lambda value: set_combo("autonomy_mode", value))
        restore("fallback_to_procedural", lambda value: set_checked("fallback_to_procedural", value))
        restore("prompt", lambda value: set_text("prompt", value, plain=True))
        restore("seed", lambda value: set_number("seed", value, int))
        restore("auto_seed", lambda value: set_checked("auto_seed", value))
        restore("count", lambda value: set_number("count", value, int))
        restore("auto_count", lambda value: set_checked("auto_count", value))
        restore("palette", lambda value: set_combo("palette_combo", value))
        restore("brush_profile", lambda value: set_combo("brush_profile", value))
        restore("iterations", lambda value: set_number("iterations", value, int))
        restore("auto_refine", lambda value: set_checked("auto_refine", value))
        restore("goal_mode", lambda value: set_checked("goal_mode", value))
        restore("brush_size_multiplier", lambda value: set_number("brush_size_multiplier", value, float))
        restore("opacity_multiplier", lambda value: set_number("opacity_multiplier", value, int))
        restore("layer_mode", lambda value: set_combo("layer_mode", value))
        restore("layer_prefix", lambda value: set_text("layer_prefix", value))
        restore("event_interval", lambda value: set_number("event_interval", value, int))
        restore("edge_threshold", lambda value: set_number("edge_threshold", value, float))
        restore("shading_density", lambda value: set_combo("shading_density", value))
        restore("enable_flats", lambda value: set_checked("enable_flats", value))
        restore("image_color_mode", lambda value: set_combo("image_color_mode", value))
        restore("save_json", lambda value: set_checked("save_json", value))
        restore("save_svg", lambda value: set_checked("save_svg_chk", value))
        restore("confirm_before_apply", lambda value: set_checked("confirm_before_apply", value))
        restore("debug_mode", lambda value: set_checked("debug_mode_chk", value))

    def _save_settings(self) -> None:
        """現在の UI 設定値を QSettings に保存する。"""
        if QSettings is None or not callable(QSettings):
            return
        with contextlib.suppress(Exception):
            settings: Any = QSettings("AIStrokePainter", "DockerSettings")
            w = _get_attr(self, "planner_mode")
            if w is not None and hasattr(w, "currentData"):
                settings.setValue("planner_mode", w.currentData() or "offline")
            w = _get_attr(self, "t2i_provider")
            if w is not None and hasattr(w, "currentData"):
                settings.setValue("t2i_provider", w.currentData() or "openai")
            w = _get_attr(self, "t2i_endpoint")
            if w is not None and hasattr(w, "text"):
                settings.setValue("t2i_endpoint", w.text())
            w = _get_attr(self, "t2i_model")
            if w is not None and hasattr(w, "text"):
                settings.setValue("t2i_model", w.text())
            w = _get_attr(self, "t2i_size")
            if w is not None and hasattr(w, "currentData"):
                settings.setValue("t2i_size", w.currentData() or "auto")
            w = _get_attr(self, "t2i_negative_prompt")
            if w is not None and hasattr(w, "text"):
                settings.setValue("t2i_negative_prompt", w.text())
            w = _get_attr(self, "base_url")
            if w is not None and hasattr(w, "text"):
                settings.setValue("base_url", w.text())
            w = _get_attr(self, "model")
            if w is not None and hasattr(w, "text"):
                settings.setValue("model", w.text())
            w = _get_attr(self, "timeout_sec")
            if w is not None and hasattr(w, "value"):
                settings.setValue("timeout_sec", w.value())
            w = _get_attr(self, "max_tokens")
            if w is not None and hasattr(w, "value"):
                settings.setValue("max_tokens", w.value())
            w = _get_attr(self, "reasoning_effort")
            if w is not None and hasattr(w, "currentData"):
                settings.setValue("reasoning_effort", w.currentData() or "low")
            w = _get_attr(self, "temperature")
            if w is not None and hasattr(w, "value"):
                settings.setValue("temperature", w.value())
            w = _get_attr(self, "top_p")
            if w is not None and hasattr(w, "value"):
                settings.setValue("top_p", w.value())
            w = _get_attr(self, "vision_res")
            if w is not None and hasattr(w, "currentData"):
                settings.setValue("vision_res", w.currentData() or 512)
            w = _get_attr(self, "custom_instructions")
            if w is not None and hasattr(w, "text"):
                settings.setValue("custom_instructions", w.text())
            w = _get_attr(self, "autonomy_mode")
            if w is not None and hasattr(w, "currentData"):
                settings.setValue("autonomy_mode", w.currentData() or "creative")
            w = _get_attr(self, "fallback_to_procedural")
            if w is not None and hasattr(w, "isChecked"):
                settings.setValue("fallback_to_procedural", w.isChecked())
            w = _get_attr(self, "prompt")
            if w is not None and hasattr(w, "toPlainText"):
                settings.setValue("prompt", w.toPlainText())
            w = _get_attr(self, "seed")
            if w is not None and hasattr(w, "value"):
                settings.setValue("seed", w.value())
            w = _get_attr(self, "auto_seed")
            if w is not None and hasattr(w, "isChecked"):
                settings.setValue("auto_seed", w.isChecked())
            w = _get_attr(self, "count")
            if w is not None and hasattr(w, "value"):
                settings.setValue("count", w.value())
            w = _get_attr(self, "auto_count")
            if w is not None and hasattr(w, "isChecked"):
                settings.setValue("auto_count", w.isChecked())
            w = _get_attr(self, "palette_combo")
            if w is not None and hasattr(w, "currentData"):
                settings.setValue("palette", w.currentData() or "auto")
            w = _get_attr(self, "brush_profile")
            if w is not None and hasattr(w, "currentData"):
                settings.setValue("brush_profile", w.currentData() or "auto")
            w = _get_attr(self, "iterations")
            if w is not None and hasattr(w, "value"):
                settings.setValue("iterations", w.value())
            w = _get_attr(self, "auto_refine")
            if w is not None and hasattr(w, "isChecked"):
                settings.setValue("auto_refine", w.isChecked())
            w = _get_attr(self, "goal_mode")
            if w is not None and hasattr(w, "isChecked"):
                settings.setValue("goal_mode", w.isChecked())
            w = _get_attr(self, "brush_size_multiplier")
            if w is not None and hasattr(w, "value"):
                settings.setValue("brush_size_multiplier", w.value())
            w = _get_attr(self, "opacity_multiplier")
            if w is not None and hasattr(w, "value"):
                settings.setValue("opacity_multiplier", w.value())
            w = _get_attr(self, "layer_mode")
            if w is not None and hasattr(w, "currentData"):
                settings.setValue("layer_mode", w.currentData() or "multi_layer")
            w = _get_attr(self, "layer_prefix")
            if w is not None and hasattr(w, "text"):
                settings.setValue("layer_prefix", w.text())
            w = _get_attr(self, "event_interval")
            if w is not None and hasattr(w, "value"):
                settings.setValue("event_interval", w.value())
            w = _get_attr(self, "edge_threshold")
            if w is not None and hasattr(w, "value"):
                settings.setValue("edge_threshold", w.value())
            w = _get_attr(self, "shading_density")
            if w is not None and hasattr(w, "currentData"):
                settings.setValue("shading_density", w.currentData() or "medium")
            w = _get_attr(self, "enable_flats")
            if w is not None and hasattr(w, "isChecked"):
                settings.setValue("enable_flats", w.isChecked())
            w = _get_attr(self, "image_color_mode")
            if w is not None and hasattr(w, "currentData"):
                settings.setValue("image_color_mode", w.currentData() or "original")
            w = _get_attr(self, "save_json")
            if w is not None and hasattr(w, "isChecked"):
                settings.setValue("save_json", w.isChecked())
            w = _get_attr(self, "save_svg_chk")
            if w is not None and hasattr(w, "isChecked"):
                settings.setValue("save_svg", w.isChecked())
            w = _get_attr(self, "confirm_before_apply")
            if w is not None and hasattr(w, "isChecked"):
                settings.setValue("confirm_before_apply", w.isChecked())
            w = _get_attr(self, "debug_mode_chk")
            if w is not None and hasattr(w, "isChecked"):
                settings.setValue("debug_mode", w.isChecked())
            if hasattr(settings, "sync"):
                settings.sync()

    def _toggle_debug_panel(self, checked: bool) -> None:
        box = _get_attr(self, "debug_box")
        if box is not None and hasattr(box, "setVisible"):
            box.setVisible(checked)
        edit = _get_attr(self, "debug_log_edit")
        if checked and edit is not None and hasattr(edit, "toPlainText") and not edit.toPlainText():
            self._log_debug("デバッグモードが有効化されました。")

    def _log_debug(self, message: str) -> None:
        edit = _get_attr(self, "debug_log_edit")
        if edit is not None and hasattr(edit, "appendPlainText"):
            edit.appendPlainText(message)

    def _copy_debug_log(self) -> None:
        edit = _get_attr(self, "debug_log_edit")
        if edit is None or not hasattr(edit, "toPlainText"):
            return
        text = edit.toPlainText()
        if text:
            clipboard = QApplication.clipboard()
            if clipboard is not None:
                clipboard.setText(text)
                st = _get_attr(self, "status")
                if st is not None and hasattr(st, "setText"):
                    st.setText("デバッグログをクリップボードにコピーしました")

    def _clear_debug_log(self) -> None:
        edit = _get_attr(self, "debug_log_edit")
        if edit is not None and hasattr(edit, "clear"):
            edit.clear()

    def _save_debug_log(self) -> None:
        edit = _get_attr(self, "debug_log_edit")
        if edit is None or not hasattr(edit, "toPlainText"):
            return
        text = edit.toPlainText()
        if not text:
            QMessageBox.information(self, "ログ保存", "保存するログがありません。")
            return
        file_path, _ = _safe_get_save_filename(
            self, "デバッグログを保存", "ai_stroke_painter_debug.log", "テキストログ (*.log *.txt)"
        )
        if file_path:
            try:
                Path(file_path).write_text(text, encoding="utf-8")
                QMessageBox.information(self, "ログ保存", f"ログを保存しました:\n{file_path}")
            except Exception as exc:
                QMessageBox.critical(self, "エラー", f"ログの保存に失敗しました: {exc}")

    def _apply_preset(self) -> None:
        combo = _get_attr(self, "preset_combo")
        if combo is None or not hasattr(combo, "currentData"):
            return
        data = combo.currentData()
        if not data:
            return

        prompt_w = _get_attr(self, "prompt")
        count_w = _get_attr(self, "count")
        pal_w = _get_attr(self, "palette_combo")
        prof_w = _get_attr(self, "brush_profile")
        bs_w = _get_attr(self, "brush_size_multiplier")
        op_w = _get_attr(self, "opacity_multiplier")
        auto_count_w = _get_attr(self, "auto_count")

        if isinstance(data, dict):
            # カスタムプリセット
            if prompt_w is not None and hasattr(prompt_w, "setPlainText"):
                prompt_w.setPlainText(data.get("prompt", ""))
            if count_w is not None and hasattr(count_w, "setValue"):
                count_w.setValue(int(data.get("count", 35)))
            if pal_w is not None and hasattr(pal_w, "count") and hasattr(pal_w, "itemData"):
                palette = data.get("palette", "anime")
                for i in range(pal_w.count()):
                    if pal_w.itemData(i) == palette:
                        pal_w.setCurrentIndex(i)
                        break
            if prof_w is not None and hasattr(prof_w, "count") and hasattr(prof_w, "itemData"):
                prof = data.get("brush_profile", "auto")
                for i in range(prof_w.count()):
                    if prof_w.itemData(i) == prof:
                        prof_w.setCurrentIndex(i)
                        break
            if "brush_size" in data and bs_w is not None and hasattr(bs_w, "setValue"):
                bs_w.setValue(float(data["brush_size"]))
            if "opacity" in data and op_w is not None and hasattr(op_w, "setValue"):
                op_w.setValue(int(data["opacity"]))
            if auto_count_w is not None and hasattr(auto_count_w, "setChecked"):
                auto_count_w.setChecked(bool(data.get("auto_count", False)))
        elif isinstance(data, (list, tuple)):
            # ビルトインプリセット
            prompt_text = data[1]
            palette = data[2]
            count = data[3]
            if prompt_w is not None and hasattr(prompt_w, "setPlainText"):
                prompt_w.setPlainText(prompt_text)
            if count_w is not None and hasattr(count_w, "setValue"):
                count_w.setValue(count)
            if auto_count_w is not None and hasattr(auto_count_w, "setChecked"):
                # 内蔵プリセットは完成品質を優先し、少数の走査線だけを残す手動間引きを避ける。
                auto_count_w.setChecked(True)
            if pal_w is not None and hasattr(pal_w, "count") and hasattr(pal_w, "itemData"):
                for i in range(pal_w.count()):
                    if pal_w.itemData(i) == palette:
                        pal_w.setCurrentIndex(i)
                        break
            if len(data) >= 5 and prof_w is not None and hasattr(prof_w, "count") and hasattr(prof_w, "itemData"):
                prof = data[4]
                for i in range(prof_w.count()):
                    if prof_w.itemData(i) == prof:
                        prof_w.setCurrentIndex(i)
                        break
            if len(data) >= 6 and bs_w is not None and hasattr(bs_w, "setValue"):
                bs_w.setValue(float(data[5]))
            if len(data) >= 7 and op_w is not None and hasattr(op_w, "setValue"):
                op_w.setValue(int(data[6]))

        self._update_preview_multipliers()
        self._log_debug(f"[プリセット適用] {combo.currentText()}")

    def _select_reference_image(self) -> None:
        file_path, _ = _safe_get_open_filename(
            self, "参照画像を開く", "", "画像ファイル (*.png *.jpg *.jpeg *.webp *.bmp)"
        )
        if file_path and Path(file_path).is_file():
            try:
                image_path = Path(file_path)
                with image_path.open("rb") as image_handle:
                    image_data = image_handle.read(MAX_REFERENCE_IMAGE_BYTES + 1)
                if len(image_data) > MAX_REFERENCE_IMAGE_BYTES:
                    raise ValueError(f"参照画像は {MAX_REFERENCE_IMAGE_BYTES // (1024 * 1024)}MB 以下にしてください")
                self._image_bytes = image_data
                dimensions = _image_dimensions_from_header(image_data)
                dimension_text = f" — {dimensions[0]}×{dimensions[1]}px" if dimensions is not None else ""
                lbl = _get_attr(self, "image_status_label")
                if lbl is not None and hasattr(lbl, "setText"):
                    lbl.setText(f"{image_path.name}{dimension_text}")
                    if hasattr(lbl, "setToolTip"):
                        lbl.setToolTip(
                            "オフラインモードでは端末内だけで解析します。LLMモードでは縮小・メタデータ除去後に送信します。"
                        )
                btn = _get_attr(self, "clear_image_btn")
                if btn is not None and hasattr(btn, "setEnabled"):
                    btn.setEnabled(True)
                self._log_debug(
                    f"[参照画像読込] {dimension_text.lstrip(' —') or '寸法不明'} ({len(self._image_bytes)} bytes)"
                )
            except Exception as exc:
                QMessageBox.critical(self, "エラー", f"画像を読み込めませんでした: {exc}")

    def _clear_reference_image(self) -> None:
        self._image_bytes = None
        lbl = _get_attr(self, "image_status_label")
        if lbl is not None and hasattr(lbl, "setText"):
            lbl.setText("画像なし")
        btn = _get_attr(self, "clear_image_btn")
        if btn is not None and hasattr(btn, "setEnabled"):
            btn.setEnabled(False)
        self._log_debug("[参照画像クリア]")

    def _test_api_connection(self) -> None:
        existing_worker = _get_attr(self, "_connection_worker")
        if existing_worker is not None and existing_worker.isRunning():
            return
        self._log_debug("[API 接続テスト開始]")
        try:
            b_w = _get_attr(self, "base_url")
            m_w = _get_attr(self, "model")
            k_w = _get_attr(self, "api_key")
            t_w = _get_attr(self, "timeout_sec")
            tok_w = _get_attr(self, "max_tokens")
            eff_w = _get_attr(self, "reasoning_effort")
            temp_w = _get_attr(self, "temperature")
            top_w = _get_attr(self, "top_p")
            cust_w = _get_attr(self, "custom_instructions")
            auto_w = _get_attr(self, "autonomy_mode")
            vres_w = _get_attr(self, "vision_res")

            base_url_val = b_w.text() if b_w is not None and hasattr(b_w, "text") else ""
            model_val = m_w.text() if m_w is not None and hasattr(m_w, "text") else ""
            key_val = k_w.text().strip() if k_w is not None and hasattr(k_w, "text") else ""
            timeout_val = float(t_w.value()) if t_w is not None and hasattr(t_w, "value") else 120.0
            max_tokens_val = tok_w.value() if tok_w is not None and hasattr(tok_w, "value") else 16384
            effort_val = eff_w.currentData() if eff_w is not None and hasattr(eff_w, "currentData") else "low"
            temp_val = float(temp_w.value()) if temp_w is not None and hasattr(temp_w, "value") else 0.7
            top_p_val = float(top_w.value()) if top_w is not None and hasattr(top_w, "value") else 1.0
            custom_val = cust_w.text().strip() if cust_w is not None and hasattr(cust_w, "text") else ""
            autonomy_val = (
                str(auto_w.currentData() or "creative")
                if auto_w is not None and hasattr(auto_w, "currentData")
                else "creative"
            )
            vres_val = (
                int(vres_w.currentData() or 512) if vres_w is not None and hasattr(vres_w, "currentData") else 512
            )

            planner = OpenAICompatiblePlanner(
                OpenAICompatibleSettings(
                    base_url=base_url_val,
                    model=model_val,
                    api_key=key_val or os.environ.get("OPENAI_API_KEY", ""),
                    timeout_seconds=min(15.0, timeout_val),
                    max_tokens=max_tokens_val,
                    reasoning_effort=effort_val or "low",
                    temperature=temp_val,
                    top_p=top_p_val,
                    custom_system_prompt=custom_val,
                    autonomy_mode=autonomy_val,
                    vision_resolution=vres_val,
                ),
            )
            worker = ApiConnectionWorker(planner)
            self._connection_worker = worker
            worker.debug_log.connect(self._log_debug)
            worker.succeeded.connect(self._on_connection_test_succeeded)
            worker.failed.connect(self._on_connection_test_failed)
            worker.finished.connect(self._on_connection_test_finished)
            btn = _get_attr(self, "test_conn_btn")
            if btn is not None and hasattr(btn, "setEnabled"):
                btn.setEnabled(False)
            worker.start()
        except Exception as exc:
            self._log_debug(f"[API 接続テスト失敗] {exc}")
            QMessageBox.critical(self, "接続テスト失敗", str(exc))

    def _on_connection_test_succeeded(self, message: str) -> None:
        if bool(_get_attr(self, "_closing", False)):
            return
        QMessageBox.information(self, "API 接続テスト", message)

    def _on_connection_test_failed(self, message: str) -> None:
        if bool(_get_attr(self, "_closing", False)):
            return
        self._log_debug(f"[API 接続テスト失敗] {message}")
        QMessageBox.critical(self, "接続テスト失敗", message)

    def _on_connection_test_finished(self) -> None:
        if bool(_get_attr(self, "_closing", False)):
            return
        btn = _get_attr(self, "test_conn_btn")
        if btn is not None and hasattr(btn, "setEnabled"):
            btn.setEnabled(True)
        self._connection_worker = None

    def _update_seed_controls_state(self, *_args: Any) -> None:
        is_openai = self._is_openai_compatible_mode()
        auto_seed_w = _get_attr(self, "auto_seed")
        seed_w = _get_attr(self, "seed")
        if is_openai:
            if auto_seed_w is not None and hasattr(auto_seed_w, "setEnabled"):
                auto_seed_w.setEnabled(False)
            if seed_w is not None and hasattr(seed_w, "setEnabled"):
                seed_w.setEnabled(False)
        else:
            if auto_seed_w is not None and hasattr(auto_seed_w, "setEnabled"):
                auto_seed_w.setEnabled(True)
            auto_chk = (
                bool(auto_seed_w.isChecked())
                if auto_seed_w is not None and hasattr(auto_seed_w, "isChecked")
                else False
            )
            if seed_w is not None and hasattr(seed_w, "setEnabled"):
                seed_w.setEnabled(not auto_chk)

    def _update_planner_settings_state(self, *_args: Any) -> None:
        is_openai = self._is_openai_compatible_mode()
        is_t2i = self._is_t2i_mode()

        t2i_box = _get_attr(self, "t2i_settings")
        if t2i_box is not None and hasattr(t2i_box, "setEnabled"):
            t2i_box.setEnabled(is_t2i)

        llm_box = _get_attr(self, "llm_settings")
        if llm_box is not None and hasattr(llm_box, "setEnabled"):
            llm_box.setEnabled(is_openai)

        ref = _get_attr(self, "auto_refine")
        if ref is not None and hasattr(ref, "setEnabled"):
            ref.setEnabled(is_openai)
            if not is_openai and hasattr(ref, "setChecked"):
                ref.setChecked(False)

        goal = _get_attr(self, "goal_mode")
        if goal is not None and hasattr(goal, "setEnabled"):
            goal.setEnabled(is_openai)
            if not is_openai and hasattr(goal, "setChecked"):
                goal.setChecked(False)

        iters = _get_attr(self, "iterations")
        if iters is not None and hasattr(iters, "setEnabled"):
            iters.setEnabled(is_openai)
        self._update_seed_controls_state()

    def _setup_accessibility(self) -> None:
        """スクリーンリーダーおよびアクセシビリティ支援技術向けに各UI要素の名称と説明を設定する。"""
        # タブメニュー
        tabs_w = _get_attr(self, "tabs")
        if tabs_w is not None and hasattr(tabs_w, "setAccessibleName"):
            tabs_w.setAccessibleName("機能設定タブ")
        # プリセット群
        p_combo = _get_attr(self, "preset_combo")
        if p_combo is not None and hasattr(p_combo, "setAccessibleName"):
            p_combo.setAccessibleName("プリセット選択")
            if hasattr(p_combo, "setAccessibleDescription"):
                p_combo.setAccessibleDescription("作成済みのビルトインまたはカスタムプリセットを選択します")
        save_p = _get_attr(self, "save_preset_btn")
        if save_p is not None and hasattr(save_p, "setAccessibleName"):
            save_p.setAccessibleName("カスタムプリセット保存")
        del_p = _get_attr(self, "del_preset_btn")
        if del_p is not None and hasattr(del_p, "setAccessibleName"):
            del_p.setAccessibleName("カスタムプリセット削除")
        exp_p = _get_attr(self, "export_preset_btn")
        if exp_p is not None and hasattr(exp_p, "setAccessibleName"):
            exp_p.setAccessibleName("プリセットエクスポート")
        imp_p = _get_attr(self, "import_preset_btn")
        if imp_p is not None and hasattr(imp_p, "setAccessibleName"):
            imp_p.setAccessibleName("プリセットインポート")
        # プロンプト群
        ph_combo = _get_attr(self, "prompt_history_combo")
        if ph_combo is not None and hasattr(ph_combo, "setAccessibleName"):
            ph_combo.setAccessibleName("プロンプト履歴")
        cp_btn = _get_attr(self, "clear_prompt_btn")
        if cp_btn is not None and hasattr(cp_btn, "setAccessibleName"):
            cp_btn.setAccessibleName("プロンプトクリア")
        prompt_w = _get_attr(self, "prompt")
        if prompt_w is not None and hasattr(prompt_w, "setAccessibleName"):
            prompt_w.setAccessibleName("描画指示プロンプト入力欄")
        # パラメータ群
        seed_w = _get_attr(self, "seed")
        if seed_w is not None and hasattr(seed_w, "setAccessibleName"):
            seed_w.setAccessibleName("乱数シード")
        aseed_w = _get_attr(self, "auto_seed")
        if aseed_w is not None and hasattr(aseed_w, "setAccessibleName"):
            aseed_w.setAccessibleName("乱数シード自動生成")
        count_w = _get_attr(self, "count")
        if count_w is not None and hasattr(count_w, "setAccessibleName"):
            count_w.setAccessibleName("ストローク本数")
        acount_w = _get_attr(self, "auto_count")
        if acount_w is not None and hasattr(acount_w, "setAccessibleName"):
            acount_w.setAccessibleName("ストローク本数品質予算自動化")
        pal_w = _get_attr(self, "palette_combo")
        if pal_w is not None and hasattr(pal_w, "setAccessibleName"):
            pal_w.setAccessibleName("カラーパレット")
        prof_w = _get_attr(self, "brush_profile")
        if prof_w is not None and hasattr(prof_w, "setAccessibleName"):
            prof_w.setAccessibleName("ブラシタッチプロファイル")
        ref_w = _get_attr(self, "auto_refine")
        if ref_w is not None and hasattr(ref_w, "setAccessibleName"):
            ref_w.setAccessibleName("段階的ステップ描画")
        goal_w = _get_attr(self, "goal_mode")
        if goal_w is not None and hasattr(goal_w, "setAccessibleName"):
            goal_w.setAccessibleName("目標達成まで継続")
        iter_w = _get_attr(self, "iterations")
        if iter_w is not None and hasattr(iter_w, "setAccessibleName"):
            iter_w.setAccessibleName("反復回数")
        # プレビュー群
        pl_combo = _get_attr(self, "preview_layer_combo")
        if pl_combo is not None and hasattr(pl_combo, "setAccessibleName"):
            pl_combo.setAccessibleName("プレビュー表示レイヤー切り替え")
        pzi_btn = _get_attr(self, "preview_zoom_in_btn")
        if pzi_btn is not None and hasattr(pzi_btn, "setAccessibleName"):
            pzi_btn.setAccessibleName("プレビュー拡大")
        pzo_btn = _get_attr(self, "preview_zoom_out_btn")
        if pzo_btn is not None and hasattr(pzo_btn, "setAccessibleName"):
            pzo_btn.setAccessibleName("プレビュー縮小")
        prz_btn = _get_attr(self, "preview_reset_btn")
        if prz_btn is not None and hasattr(prz_btn, "setAccessibleName"):
            prz_btn.setAccessibleName("プレビュー表示リセット")
        prev_w = _get_attr(self, "preview")
        if prev_w is not None and hasattr(prev_w, "setAccessibleName"):
            prev_w.setAccessibleName("ストロークベクタープレビュー")
            if hasattr(prev_w, "setAccessibleDescription"):
                prev_w.setAccessibleDescription(
                    "生成されたストロークのベクタープレビュー。マウスホイールでズーム、ドラッグで移動、ダブルクリックでリセット"
                )
        # 画像変換タブ
        load_b = _get_attr(self, "load_image_btn")
        if load_b is not None and hasattr(load_b, "setAccessibleName"):
            load_b.setAccessibleName("参照画像読み込み")
        clear_img_b = _get_attr(self, "clear_image_btn")
        if clear_img_b is not None and hasattr(clear_img_b, "setAccessibleName"):
            clear_img_b.setAccessibleName("参照画像クリア")
        for attr, name in (
            ("edge_threshold", "エッジ検出感度"),
            ("shading_density", "陰影ハッチング密度"),
            ("enable_flats", "下塗り描画"),
            ("image_color_mode", "画像カラーモード"),
        ):
            w = _get_attr(self, attr)
            if w is not None and hasattr(w, "setAccessibleName"):
                w.setAccessibleName(name)
        # AI設定タブ
        for attr, name in (
            ("planner_mode", "描画エンジン切り替え"),
            ("test_conn_btn", "API接続テスト"),
            ("base_url", "APIエンドポイントURL"),
            ("model", "AIモデル識別子"),
            ("api_key", "APIキー"),
            ("profile_openai_btn", "OpenAI公式プロファイル適用"),
            ("profile_ollama_btn", "Ollamaローカルプロファイル適用"),
            ("profile_lmstudio_btn", "LM Studioローカルプロファイル適用"),
            ("profile_deepseek_btn", "DeepSeekプロファイル適用"),
        ):
            w = _get_attr(self, attr)
            if w is not None and hasattr(w, "setAccessibleName"):
                w.setAccessibleName(name)
        # レイヤー・詳細設定タブ
        for attr, name in (
            ("brush_size_multiplier", "ブラシ太さ倍率"),
            ("opacity_multiplier", "不透明度倍率"),
            ("layer_mode", "レイヤー出力モード"),
            ("layer_prefix", "レイヤー名プレフィックス"),
            ("event_interval", "画面描画更新間隔"),
            ("save_json", "計画JSON保存"),
            ("save_svg_chk", "ベクターSVG保存"),
            ("confirm_before_apply", "適用前プレビュー確認"),
            ("debug_mode_chk", "デバッグログ表示切り替え"),
            ("reset_defaults_btn", "全設定初期化"),
            ("copy_log_btn", "デバッグログコピー"),
            ("clear_log_btn", "デバッグログクリア"),
            ("save_log_btn", "デバッグログファイル保存"),
        ):
            w = _get_attr(self, attr)
            if w is not None and hasattr(w, "setAccessibleName"):
                w.setAccessibleName(name)
        # 下部実行コントロール
        run_b = _get_attr(self, "run_btn")
        if run_b is not None and hasattr(run_b, "setAccessibleName"):
            run_b.setAccessibleName("プレビュー生成または描画実行")
        apply_b = _get_attr(self, "apply_btn")
        if apply_b is not None and hasattr(apply_b, "setAccessibleName"):
            apply_b.setAccessibleName("キャンバスへ適用")
        stop_b = _get_attr(self, "stop_btn")
        if stop_b is not None and hasattr(stop_b, "setAccessibleName"):
            stop_b.setAccessibleName("描画処理停止")
        prog_w = _get_attr(self, "progress")
        if prog_w is not None and hasattr(prog_w, "setAccessibleName"):
            prog_w.setAccessibleName("描画進捗率")
        stat_w = _get_attr(self, "status")
        if stat_w is not None and hasattr(stat_w, "setAccessibleName"):
            stat_w.setAccessibleName("ステータス表示")

    def _update_action_buttons_state(self, *_args: Any) -> None:
        """適用前確認チェックボックスの状態に合わせてボタン文言と適用ボタン状態を同期する。"""
        confirm_w = _get_attr(self, "confirm_before_apply")
        is_confirm = bool(confirm_w.isChecked()) if confirm_w is not None and hasattr(confirm_w, "isChecked") else True
        run_b = _get_attr(self, "run_btn")
        if run_b is not None and hasattr(run_b, "setText"):
            run_b.setText("プレビュー生成" if is_confirm else "キャンバスに描画")
        apply_b = _get_attr(self, "apply_btn")
        if apply_b is not None and hasattr(apply_b, "setEnabled"):
            if not is_confirm:
                apply_b.setEnabled(False)
            else:
                apply_b.setEnabled(self._pending_plan is not None)

    def _is_t2i_mode(self) -> bool:
        mode_w = _get_attr(self, "planner_mode")
        if mode_w is None:
            return False
        mode_data = mode_w.currentData() if hasattr(mode_w, "currentData") else None
        mode_text = mode_w.currentText() if hasattr(mode_w, "currentText") else ""
        return mode_data == "t2i_stroke" or "画像生成" in mode_text

    def _is_openai_compatible_mode(self) -> bool:
        mode_w = _get_attr(self, "planner_mode")
        if mode_w is None:
            return False
        mode_data = mode_w.currentData() if hasattr(mode_w, "currentData") else None
        mode_text = mode_w.currentText() if hasattr(mode_w, "currentText") else ""
        return mode_data == "openai_compatible" or "OpenAI" in mode_text

    def _planner(self) -> PlannerPort:
        is_t2i = self._is_t2i_mode()
        is_openai = self._is_openai_compatible_mode()

        if is_t2i:
            prov_w = _get_attr(self, "t2i_provider")
            ep_w = _get_attr(self, "t2i_endpoint")
            m_w = _get_attr(self, "t2i_model")
            k_w = _get_attr(self, "t2i_api_key")
            sz_w = _get_attr(self, "t2i_size")
            neg_w = _get_attr(self, "t2i_negative_prompt")

            prov_val = (
                (prov_w.currentData() or "openai")
                if prov_w is not None and hasattr(prov_w, "currentData")
                else "openai"
            )
            ep_val = (
                ep_w.text().strip()
                if ep_w is not None and hasattr(ep_w, "text")
                else "https://api.openai.com/v1/images/generations"
            )
            m_val = m_w.text().strip() if m_w is not None and hasattr(m_w, "text") else "dall-e-3"
            k_val = k_w.text().strip() if k_w is not None and hasattr(k_w, "text") else ""
            sz_val = (sz_w.currentData() or "auto") if sz_w is not None and hasattr(sz_w, "currentData") else "auto"
            neg_val = neg_w.text().strip() if neg_w is not None and hasattr(neg_w, "text") else ""

            self._log_debug(
                f"[エンジン選択] AI 画像生成 (Text-to-Image -> Stroke) [Provider: {prov_val}, Endpoint: {_safe_endpoint_label(ep_val)}, Model: {m_val}, Size: {sz_val}]"
            )
            return ImageGenerationPlanner(
                ImageGeneratorSettings(
                    provider=prov_val,
                    endpoint_url=ep_val,
                    api_key=k_val or os.environ.get("OPENAI_API_KEY", ""),
                    model=m_val,
                    size=sz_val,
                    negative_prompt=neg_val,
                ),
                log_callback=self._log_debug,
            )

        if not is_openai:
            self._log_debug("[エンジン選択] プロシージャル (オフライン)")
            p = _get_attr(self, "planner")
            return p if isinstance(p, PlannerPort) else RuleBasedPlanner()

        b_w = _get_attr(self, "base_url")
        m_w = _get_attr(self, "model")
        k_w = _get_attr(self, "api_key")
        t_w = _get_attr(self, "timeout_sec")
        tok_w = _get_attr(self, "max_tokens")
        eff_w = _get_attr(self, "reasoning_effort")
        temp_w = _get_attr(self, "temperature")
        top_w = _get_attr(self, "top_p")
        cust_w = _get_attr(self, "custom_instructions")
        auto_w = _get_attr(self, "autonomy_mode")
        vres_w = _get_attr(self, "vision_res")
        fallback_w = _get_attr(self, "fallback_to_procedural")

        base_url_val = b_w.text() if b_w is not None and hasattr(b_w, "text") else "https://api.openai.com/v1"
        model_val = m_w.text() if m_w is not None and hasattr(m_w, "text") else "gpt-4o"
        key_val = k_w.text().strip() if k_w is not None and hasattr(k_w, "text") else ""
        timeout_val = float(t_w.value()) if t_w is not None and hasattr(t_w, "value") else 120.0
        max_tokens_val = tok_w.value() if tok_w is not None and hasattr(tok_w, "value") else 16384
        effort_val = (eff_w.currentData() or "low") if eff_w is not None and hasattr(eff_w, "currentData") else "low"
        temp_val = float(temp_w.value()) if temp_w is not None and hasattr(temp_w, "value") else 0.70
        top_p_val = float(top_w.value()) if top_w is not None and hasattr(top_w, "value") else 1.0
        custom_val = cust_w.text().strip() if cust_w is not None and hasattr(cust_w, "text") else ""
        autonomy_val = (
            str(auto_w.currentData() or "creative")
            if auto_w is not None and hasattr(auto_w, "currentData")
            else "creative"
        )
        vres = int(vres_w.currentData() or 512) if vres_w is not None and hasattr(vres_w, "currentData") else 512

        self._log_debug(
            f"[エンジン選択] OpenAI 互換 API (Base URL: {_safe_endpoint_label(base_url_val)}, Model: {model_val}, Autonomy: {autonomy_val}, MaxTokens: {max_tokens_val}, ReasoningEffort: {effort_val}, Temp: {temp_val:.2f}, TopP: {top_p_val:.2f}, VisionRes: {vres})"
        )
        return OpenAICompatiblePlanner(
            OpenAICompatibleSettings(
                base_url=base_url_val,
                model=model_val,
                api_key=key_val or os.environ.get("OPENAI_API_KEY", ""),
                timeout_seconds=timeout_val,
                max_tokens=max_tokens_val,
                reasoning_effort=effort_val,
                temperature=temp_val,
                top_p=top_p_val,
                custom_system_prompt=custom_val,
                autonomy_mode=autonomy_val,
                vision_resolution=vres,
                fallback_to_procedural=(
                    fallback_w.isChecked() if fallback_w is not None and hasattr(fallback_w, "isChecked") else False
                ),
            ),
            log_callback=self._log_debug,
        )

    def is_cancelled(self) -> bool:
        try:
            return bool(getattr(self, "_cancel", False))
        except Exception:
            return False

    def _set_generation_controls_enabled(self, enabled: bool) -> None:
        """実行中に変更しても現在の worker へ反映されない設定をロックする。"""
        names = (
            "preset_combo",
            "save_preset_btn",
            "del_preset_btn",
            "export_preset_btn",
            "import_preset_btn",
            "prompt",
            "prompt_history_combo",
            "clear_prompt_btn",
            "load_image_btn",
            "clear_image_btn",
            "planner_mode",
            "profile_openai_btn",
            "profile_ollama_btn",
            "profile_lmstudio_btn",
            "profile_deepseek_btn",
            "seed",
            "auto_seed",
            "count",
            "auto_count",
            "palette_combo",
            "brush_profile",
            "iterations",
            "auto_refine",
            "goal_mode",
            "brush_size_multiplier",
            "opacity_multiplier",
            "layer_mode",
            "layer_prefix",
            "event_interval",
            "edge_threshold",
            "shading_density",
            "enable_flats",
            "image_color_mode",
            "t2i_settings",
            "llm_settings",
            "autonomy_mode",
            "save_json",
            "save_svg_chk",
            "confirm_before_apply",
            "reset_defaults_btn",
            "test_conn_btn",
        )
        for name in names:
            widget = _get_attr(self, name)
            if widget is not None and hasattr(widget, "setEnabled"):
                widget.setEnabled(enabled)
        if enabled:
            self._update_planner_settings_state()
            count_toggle = _get_attr(self, "auto_count")
            count_widget = _get_attr(self, "count")
            if (
                count_toggle is not None
                and hasattr(count_toggle, "isChecked")
                and count_toggle.isChecked()
                and count_widget is not None
                and hasattr(count_widget, "setEnabled")
            ):
                count_widget.setEnabled(False)

    def cancel(self) -> None:
        self._cancel = True
        apply_button = _get_attr(self, "apply_btn")
        if apply_button is not None and hasattr(apply_button, "setEnabled"):
            apply_button.setEnabled(False)
        worker = _get_attr(self, "_worker")
        if worker is not None and hasattr(worker, "cancel"):
            worker.cancel()
        st = _get_attr(self, "status")
        if st is not None and hasattr(st, "setText"):
            st.setText("停止要求を受け付けました。現在の処理完了後に停止します。")
        self._log_debug("[UI] 停止ボタンが押下されました")

    def run(self) -> None:
        app = Krita.instance()
        document: Any | None = app.activeDocument() if app is not None else None
        if document is None:
            QMessageBox.warning(
                self, "AI Stroke Painter", "先にドキュメントを開いてください。描画先キャンバスがありません。"
            )
            return

        window = getattr(app, "activeWindow", lambda: None)() if app is not None else None
        active_view = getattr(window, "activeView", lambda: None)() if window is not None else None
        if active_view is None:
            QMessageBox.warning(self, "AI Stroke Painter", "描画対象のアクティブビューを取得できませんでした。")
            return
        view_document = getattr(active_view, "document", lambda: document)()
        if view_document is not None and view_document != document:
            QMessageBox.warning(self, "AI Stroke Painter", "アクティブビューと描画対象ドキュメントが一致しません。")
            return

        doc_width: float = 1000.0
        doc_height: float = 1000.0
        if hasattr(document, "width") and hasattr(document, "height"):
            try:
                w_val: Any = document.width() if callable(document.width) else document.width
                h_val: Any = document.height() if callable(document.height) else document.height
                doc_width = float(w_val)
                doc_height = float(h_val)
            except Exception:
                pass

        self._save_settings()
        prompt_w = _get_attr(self, "prompt")
        if prompt_w is not None and hasattr(prompt_w, "toPlainText"):
            self._save_prompt_to_history(prompt_w.toPlainText())
        self._active_doc = document
        self._active_view = active_view
        self._cancel = False
        self._session_plans = []
        self._last_plan = None
        self._pending_plan = None
        self._applying_pending = False
        run_b = _get_attr(self, "run_btn")
        if run_b is not None and hasattr(run_b, "setEnabled"):
            run_b.setEnabled(False)
        stop_b = _get_attr(self, "stop_btn")
        if stop_b is not None and hasattr(stop_b, "setEnabled"):
            stop_b.setEnabled(True)
        prog = _get_attr(self, "progress")
        if prog is not None and hasattr(prog, "setRange"):
            prog.setRange(0, 0)
        self._set_generation_controls_enabled(False)

        iter_w = _get_attr(self, "iterations")
        ref_w = _get_attr(self, "auto_refine")
        goal_w = _get_attr(self, "goal_mode")
        auto_seed_w = _get_attr(self, "auto_seed")
        auto_count_w = _get_attr(self, "auto_count")
        pal_w = _get_attr(self, "palette_combo")
        prof_w = _get_attr(self, "brush_profile")
        bs_w = _get_attr(self, "brush_size_multiplier")
        op_w = _get_attr(self, "opacity_multiplier")
        lm_w = _get_attr(self, "layer_mode")
        lp_w = _get_attr(self, "layer_prefix")
        ei_w = _get_attr(self, "event_interval")
        sj_w = _get_attr(self, "save_json")
        ss_w = _get_attr(self, "save_svg_chk")

        is_openai = self._is_openai_compatible_mode()
        auto_seed = (
            bool(auto_seed_w.isChecked())
            if auto_seed_w is not None and hasattr(auto_seed_w, "isChecked") and not is_openai
            else False
        )
        auto_count = (
            bool(auto_count_w.isChecked()) if auto_count_w is not None and hasattr(auto_count_w, "isChecked") else False
        )
        goal_mode = (
            bool(goal_w.isChecked()) if goal_w is not None and hasattr(goal_w, "isChecked") and is_openai else False
        )

        max_iters = (
            10
            if goal_mode
            else (iter_w.value() if iter_w is not None and ref_w is not None and is_openai and ref_w.isChecked() else 1)
        )
        palette = (pal_w.currentData() or "auto") if pal_w is not None and hasattr(pal_w, "currentData") else "auto"
        profile = (prof_w.currentData() or "auto") if prof_w is not None and hasattr(prof_w, "currentData") else "auto"
        size_val = bs_w.value() if bs_w is not None and hasattr(bs_w, "value") else 1.0
        op_val = op_w.value() if op_w is not None and hasattr(op_w, "value") else 100
        self._run_render_options = {
            "size_multiplier": float(size_val),
            "opacity_multiplier": float(op_val) / 100.0,
            "layer_mode": (
                str(lm_w.currentData() or "multi_layer")
                if lm_w is not None and hasattr(lm_w, "currentData")
                else "multi_layer"
            ),
            "layer_prefix": (
                str(lp_w.text().strip() or "AI Artwork") if lp_w is not None and hasattr(lp_w, "text") else "AI Artwork"
            ),
            "event_interval": int(ei_w.value()) if ei_w is not None and hasattr(ei_w, "value") else 30,
            "save_json": bool(sj_w.isChecked()) if sj_w is not None and hasattr(sj_w, "isChecked") else False,
            "save_svg": bool(ss_w.isChecked()) if ss_w is not None and hasattr(ss_w, "isChecked") else False,
        }

        now_str = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        self._log_debug(f"\n========== 描画タスク開始 [{now_str}] ==========")
        mode_w = _get_attr(self, "planner_mode")
        mode_str = mode_w.currentText() if mode_w is not None and hasattr(mode_w, "currentText") else ""
        count_mode_str = (
            "Auto (品質予算)"
            if auto_count
            else str(_get_attr(self, "count").value() if _get_attr(self, "count") else 35)
        )
        goal_mode_str = ", Goal Mode: ON" if goal_mode else ""
        self._log_debug(
            f"選択モード: {mode_str}, 反復数: {max_iters}{goal_mode_str}, ストローク本数: {count_mode_str}, パレット: {palette}, プロファイル: {profile}, 太さ倍率: {size_val}x, 不透明度: {op_val}%"
        )
        if self._image_bytes and mode_w is not None and mode_w.currentData() == "openai_compatible":
            self._log_debug("[プライバシー] 参照画像は縮小・私的メタデータ除去後に設定先APIへ送信されます")
        st = _get_attr(self, "status")
        if st is not None and hasattr(st, "setText"):
            st.setText("描画計画を生成中… 停止できます。")

        prev_w_initial = _get_attr(self, "preview")
        if prev_w_initial is not None:
            if hasattr(prev_w_initial, "set_canvas_size"):
                prev_w_initial.set_canvas_size(doc_width, doc_height)
            if hasattr(prev_w_initial, "clear_plan"):
                prev_w_initial.clear_plan()
        quality_label = _get_attr(self, "quality_summary_label")
        if quality_label is not None and hasattr(quality_label, "setText"):
            quality_label.setText("品質診断: 計画を解析中…")

        try:
            planner = self._planner()
            prompt_w = _get_attr(self, "prompt")
            seed_w = _get_attr(self, "seed")
            count_w = _get_attr(self, "count")
            ethresh_w = _get_attr(self, "edge_threshold")
            sdens_w = _get_attr(self, "shading_density")
            flats_w = _get_attr(self, "enable_flats")
            cmode_w = _get_attr(self, "image_color_mode")

            prompt_val = (
                prompt_w.toPlainText().strip() if prompt_w is not None and hasattr(prompt_w, "toPlainText") else ""
            )

            # Auto Seed: 実行時にランダムシードを生成してスピンボックスに反映
            if auto_seed:
                seed_val = random.randint(0, 2147483647)
                if seed_w is not None and hasattr(seed_w, "setValue"):
                    seed_w.setValue(seed_val)
                self._log_debug(f"[Auto Seed] ランダムシード {seed_val} を生成しました")
            else:
                seed_val = seed_w.value() if seed_w is not None and hasattr(seed_w, "value") else 42

            count_val = (
                None if auto_count else (count_w.value() if count_w is not None and hasattr(count_w, "value") else 35)
            )
            e_thresh = float(ethresh_w.value()) if ethresh_w is not None and hasattr(ethresh_w, "value") else 0.18
            s_dens = (
                str(sdens_w.currentData() or "medium")
                if sdens_w is not None and hasattr(sdens_w, "currentData")
                else "medium"
            )
            flats_val = flats_w.isChecked() if flats_w is not None and hasattr(flats_w, "isChecked") else True
            c_mode = (
                str(cmode_w.currentData() or "original")
                if cmode_w is not None and hasattr(cmode_w, "currentData")
                else "original"
            )

            worker = PlanWorker(
                planner=planner,
                prompt=prompt_val,
                seed=seed_val,
                count=count_val,
                width=doc_width,
                height=doc_height,
                image_data=self._image_bytes,
                max_iterations=max_iters,
                palette_name=palette,
                brush_profile=profile,
                edge_threshold=e_thresh,
                shading_density=s_dens,
                enable_flats=flats_val,
                color_mode=c_mode,
                auto_count=auto_count,
                goal_mode=goal_mode,
            )
            self._worker = worker
            worker.debug_log.connect(self._log_debug)
            worker.plan_ready.connect(self._on_plan_ready)
            worker.iteration_progress.connect(self._on_iteration_progress)
            worker.plan_failed.connect(self._on_plan_failed)
            worker.finished.connect(self._on_worker_finished)
            worker.start()
        except Exception as exc:
            self._log_debug(f"[タスク起動例外] {exc}\n{traceback.format_exc()}")
            st = _get_attr(self, "status")
            if st is not None and hasattr(st, "setText"):
                st.setText(f"エラー: {exc}")
            QMessageBox.critical(self, "AI Stroke Painter", str(exc))
            self._reset_run_state()

    def _on_iteration_progress(self, current: int, total: int, msg: str) -> None:
        if bool(_get_attr(self, "_closing", False)):
            return
        st = _get_attr(self, "status")
        if st is not None and hasattr(st, "setText"):
            st.setText(msg)
        progress = _get_attr(self, "progress")
        if progress is not None and hasattr(progress, "setRange") and hasattr(progress, "setValue"):
            progress.setRange(0, max(1, total))
            progress.setValue(max(0, current - 1))

    def _on_plan_ready(self, plan: DrawingPlan) -> None:
        if bool(_get_attr(self, "_closing", False)):
            return
        active_doc = _get_attr(self, "_active_doc")
        document = active_doc or (Krita.instance().activeDocument() if Krita.instance() is not None else None)

        doc_w: float = float(plan.canvas_width or 1000.0)
        doc_h: float = float(plan.canvas_height or 1000.0)
        if document is not None and hasattr(document, "width") and hasattr(document, "height"):
            try:
                w_val: Any = document.width() if callable(document.width) else document.width
                h_val: Any = document.height() if callable(document.height) else document.height
                doc_w = float(w_val)
                doc_h = float(h_val)
            except Exception:
                pass

        # キャンバス解像度と計画寸法の整合（乖離防止）
        if (
            plan.canvas_width is not None
            and plan.canvas_height is not None
            and (
                not math.isclose(plan.canvas_width, doc_w, rel_tol=0.01)
                or not math.isclose(plan.canvas_height, doc_h, rel_tol=0.01)
            )
        ):
            plan = plan.scale_to(doc_w, doc_h, fit_mode="scale")
        elif plan.canvas_width is None or plan.canvas_height is None:
            plan = replace(plan, canvas_width=doc_w, canvas_height=doc_h)

        resuming_pending = (
            bool(_get_attr(self, "_applying_pending", False)) and _get_attr(self, "_pending_plan") is plan
        )
        if resuming_pending:
            cumulative_plan = _get_attr(self, "_last_plan")
            if not isinstance(cumulative_plan, DrawingPlan):
                resuming_pending = False
        if not resuming_pending:
            session_plans = list(_get_attr(self, "_session_plans", []))
            session_plans.append(plan)
            try:
                cumulative_plan = combine_drawing_plans(session_plans, auto_rescale=True)
            except Exception as exc:
                message = f"反復計画を統合できませんでした: {exc}"
                self._log_debug(f"[累積計画エラー] {message}")
                st_w = _get_attr(self, "status")
                if st_w is not None and hasattr(st_w, "setText"):
                    st_w.setText(message)
                worker = _get_attr(self, "_worker")
                if worker is not None and hasattr(worker, "notify_render_failed"):
                    worker.notify_render_failed(message)
                return
            self._session_plans = session_plans
            self._last_plan = cumulative_plan
        bs_w = _get_attr(self, "brush_size_multiplier")
        op_w = _get_attr(self, "opacity_multiplier")
        prev_w = _get_attr(self, "preview")
        lm_w = _get_attr(self, "layer_mode")
        lp_w = _get_attr(self, "layer_prefix")
        ei_w = _get_attr(self, "event_interval")
        sj_w = _get_attr(self, "save_json")
        ss_w = _get_attr(self, "save_svg_chk")
        cp = _get_attr(self, "canvas_port")
        if cp is None:
            cp = KritaCanvasAdapter()

        size_mult = float(bs_w.value()) if bs_w is not None and hasattr(bs_w, "value") else 1.0
        op_mult = (float(op_w.value()) / 100.0) if op_w is not None and hasattr(op_w, "value") else 1.0
        render_options = _get_attr(self, "_run_render_options") or {}
        size_mult = float(render_options.get("size_multiplier", size_mult))
        op_mult = float(render_options.get("opacity_multiplier", op_mult))
        preview_layer_mode = str(
            render_options.get(
                "layer_mode",
                lm_w.currentData() if lm_w is not None and hasattr(lm_w, "currentData") else "multi_layer",
            )
            or "multi_layer"
        )

        if not resuming_pending and prev_w is not None and hasattr(prev_w, "set_plan"):
            try:
                if hasattr(prev_w, "set_canvas_size"):
                    prev_w.set_canvas_size(doc_w, doc_h)
                prev_w.set_plan(
                    plan,
                    size_multiplier=size_mult,
                    opacity_multiplier=op_mult,
                    accumulate=True,
                    layer_mode=preview_layer_mode,
                )
            except TypeError:
                try:
                    prev_w.set_plan(plan, size_multiplier=size_mult, opacity_multiplier=op_mult)
                except TypeError:
                    try:
                        prev_w.set_plan(plan)
                    except Exception as exc:
                        self._log_debug(f"[プレビュー更新失敗] {exc}")
            except Exception as exc:
                self._log_debug(f"[プレビュー更新失敗] {exc}")

        quality_label = _get_attr(self, "quality_summary_label")
        if quality_label is not None and hasattr(quality_label, "setText"):
            try:
                quality_label.setText(_format_plan_quality_summary(plan))
            except Exception as exc:
                quality_label.setText("品質診断を計算できませんでした")
                self._log_debug(f"[品質診断失敗] {exc}")

        confirm_w = _get_attr(self, "confirm_before_apply")
        confirmation_required = bool(
            confirm_w is not None and hasattr(confirm_w, "isChecked") and confirm_w.isChecked()
        )
        if confirmation_required and not resuming_pending:
            self._pending_plan = plan
            apply_button = _get_attr(self, "apply_btn")
            if apply_button is not None and hasattr(apply_button, "setEnabled"):
                apply_button.setEnabled(True)
            st_w = _get_attr(self, "status")
            if st_w is not None and hasattr(st_w, "setText"):
                st_w.setText(f"ステップ {plan.iteration} のプレビューを確認し、「キャンバスへ適用」を押してください")
            self._log_debug(f"[適用待機] ステップ {plan.iteration} の計画をプレビューで確認できます")
            return
        if resuming_pending:
            self._pending_plan = None
            self._applying_pending = False
            apply_button = _get_attr(self, "apply_btn")
            if apply_button is not None and hasattr(apply_button, "setEnabled"):
                apply_button.setEnabled(False)

        render_error: str | None = None
        try:
            if document is None:
                st_w = _get_attr(self, "status")
                if st_w is not None and hasattr(st_w, "setText"):
                    st_w.setText("ドキュメントが閉じられたため描画を中断しました")
                self._log_debug("[描画中断] アクティブなドキュメントがありません")
                self._cancel = True
                worker = _get_attr(self, "_worker")
                if worker is not None and hasattr(worker, "cancel"):
                    worker.cancel()
                return

            if self.is_cancelled():
                return

            if (
                not bool(_get_attr(self, "_canvas_session_open", False))
                and cp is not None
                and hasattr(cp, "begin_render_session")
            ):
                cp.begin_render_session(document)
                self._canvas_session_open = True

            paths: list[str] = []
            export_errors: list[str] = []
            save_json_enabled = bool(
                render_options.get("save_json", sj_w is not None and hasattr(sj_w, "isChecked") and sj_w.isChecked())
            )
            save_svg_enabled = bool(
                render_options.get("save_svg", ss_w is not None and hasattr(ss_w, "isChecked") and ss_w.isChecked())
            )
            l_mode = (
                str(render_options["layer_mode"])
                if "layer_mode" in render_options
                else (
                    str(lm_w.currentData() or "multi_layer")
                    if lm_w is not None and hasattr(lm_w, "currentData")
                    else "multi_layer"
                )
            )
            l_prefix = (
                str(render_options["layer_prefix"])
                if "layer_prefix" in render_options
                else (
                    str(lp_w.text().strip() or "AI Artwork")
                    if lp_w is not None and hasattr(lp_w, "text")
                    else "AI Artwork"
                )
            )
            e_interval = int(
                render_options.get(
                    "event_interval", int(ei_w.value()) if ei_w is not None and hasattr(ei_w, "value") else 30
                )
            )

            self._log_debug(
                f"[描画レンダリング開始] ストローク本数={len(plan.strokes)}, レイヤーモード={l_mode}, プレフィックス={l_prefix}, 太さ={size_mult}x, 不透明度={op_mult * 100:.0f}%"
            )
            rendered = cp.render(
                document,
                plan,
                self.is_cancelled,
                brush_size_multiplier=size_mult,
                opacity_multiplier=op_mult,
                layer_mode=l_mode,
                layer_prefix=l_prefix,
                event_interval=e_interval,
                view=_get_attr(self, "_active_view"),
            )
            render_worker = _get_attr(self, "_worker")
            is_goal_completion = bool(getattr(render_worker, "goal_mode", False)) and (
                plan.metadata.get("session_goal_reached", False) is True or _is_plan_goal_reached(plan)
            )
            is_final_iteration = (
                plan.iteration >= int(getattr(render_worker, "max_iterations", plan.iteration)) or is_goal_completion
            )
            if not self.is_cancelled() and is_final_iteration:
                export_plan = materialize_render_options(
                    cumulative_plan,
                    size_multiplier=size_mult,
                    opacity_multiplier=op_mult,
                    layer_mode=l_mode,
                )
                if save_json_enabled:
                    try:
                        saved_json_path = save_plan(export_plan)
                    except Exception as exc:
                        export_errors.append(f"JSON: {exc}")
                        self._log_debug(f"[JSON保存失敗] {exc}\n{traceback.format_exc()}")
                    else:
                        paths.append(str(saved_json_path))
                        self._log_debug(f"[JSON保存] {Path(saved_json_path).name}")
                if save_svg_enabled:
                    try:
                        saved_svg_path = save_svg(export_plan)
                    except Exception as exc:
                        export_errors.append(f"SVG: {exc}")
                        self._log_debug(f"[SVG保存失敗] {exc}\n{traceback.format_exc()}")
                    else:
                        paths.append(str(saved_svg_path))
                        self._log_debug(f"[SVG保存] {Path(saved_svg_path).name}")
            suffix = f" ({', '.join(paths)})" if paths else ""
            warning_suffix = f" / 保存警告: {'; '.join(export_errors)}" if export_errors else ""
            st_w = _get_attr(self, "status")
            if self.is_cancelled():
                if st_w is not None and hasattr(st_w, "setText"):
                    st_w.setText(f"{rendered}本を描画して停止しました{suffix}{warning_suffix}")
                self._log_debug(f"[描画停止] {rendered} 本を描画後に停止")
            else:
                if st_w is not None and hasattr(st_w, "setText"):
                    if is_final_iteration:
                        st_w.setText(
                            f"描画完了: 累積{len(cumulative_plan.strokes)}本を生成しました{suffix}{warning_suffix}"
                        )
                    else:
                        st_w.setText(
                            f"ステップ {plan.iteration} を適用済み: {rendered}本、累積{len(cumulative_plan.strokes)}本"
                        )
                self._log_debug(
                    f"[描画適用] ステップ {plan.iteration}: {rendered} 本、累積 {len(cumulative_plan.strokes)} 本をキャンバスに描画しました"
                )
        except Exception as exc:
            render_error = str(exc) or exc.__class__.__name__
            self._log_debug(f"[描画レンダリング例外] {exc}\n{traceback.format_exc()}")
            st_w = _get_attr(self, "status")
            if st_w is not None and hasattr(st_w, "setText"):
                st_w.setText(f"描画エラー: {exc}")
        finally:
            worker = _get_attr(self, "_worker")
            if worker is not None:
                if render_error is not None and hasattr(worker, "notify_render_failed"):
                    worker.notify_render_failed(render_error)
                elif (
                    document is not None
                    and not self.is_cancelled()
                    and hasattr(worker, "provide_canvas_capture")
                    and getattr(worker, "max_iterations", 1) > plan.iteration
                ):
                    vres_w = _get_attr(self, "vision_res")
                    vres = (
                        int(vres_w.currentData() or 512)
                        if vres_w is not None and hasattr(vres_w, "currentData")
                        else 512
                    )
                    self._log_debug(
                        f"[自動改善] 現在のキャンバスを視覚評価するためキャプチャします (解像度: {vres}x{vres})..."
                    )
                    try:
                        cap_img = cp.capture_canvas(document, vres, vres)
                    except Exception as exc:
                        cap_img = b""
                        self._log_debug(f"[自動改善] キャンバス取得エラー: {exc}")
                    if cap_img:
                        self._log_debug(f"[自動改善] キャプチャ完了 ({len(cap_img)} bytes)。次ステップへ送信します")
                    else:
                        self._log_debug(
                            "[自動改善] キャンバス取得に失敗したため、次ステップはテキスト情報のみで続行します"
                        )
                    worker.provide_canvas_capture(cap_img)
                elif hasattr(worker, "notify_render_done"):
                    worker.notify_render_done()
            if worker is None or not (hasattr(worker, "isRunning") and worker.isRunning()):
                self._reset_run_state()

    def _apply_pending_plan(self) -> None:
        plan = _get_attr(self, "_pending_plan")
        if not isinstance(plan, DrawingPlan) or self.is_cancelled():
            return
        self._applying_pending = True
        apply_button = _get_attr(self, "apply_btn")
        if apply_button is not None and hasattr(apply_button, "setEnabled"):
            apply_button.setEnabled(False)
        self._on_plan_ready(plan)

    def _on_plan_failed(self, error_msg: str) -> None:
        if bool(_get_attr(self, "_closing", False)):
            return
        self._worker = None
        self._log_debug(f"[計画生成失敗] {error_msg}")
        if not self.is_cancelled():
            chk = _get_attr(self, "debug_mode_chk")
            st = _get_attr(self, "status")
            if chk is not None and hasattr(chk, "isChecked") and chk.isChecked():
                if st is not None and hasattr(st, "setText"):
                    st.setText(f"エラー: {error_msg} (詳細はデバッグログ参照)")
            elif st is not None and hasattr(st, "setText"):
                st.setText(f"エラー: {error_msg}")
            QMessageBox.critical(self, "AI Stroke Painter エラー", error_msg)
        self._reset_run_state()

    def _on_worker_finished(self) -> None:
        if bool(_get_attr(self, "_closing", False)):
            return
        worker = _get_attr(self, "_worker")
        worker_is_cancelled = False
        if worker is not None:
            cancelled_getter = getattr(worker, "is_cancelled", None)
            if callable(cancelled_getter):
                with contextlib.suppress(Exception):
                    worker_is_cancelled = bool(cancelled_getter())
        succeeded = bool(
            worker is not None
            and getattr(worker, "completed_successfully", False)
            and not worker_is_cancelled
            and not self.is_cancelled()
        )
        self._reset_run_state(commit_session=succeeded)

    def _finish_canvas_session(self, commit: bool) -> bool:
        if not bool(_get_attr(self, "_canvas_session_open", False)):
            return True
        canvas_port = _get_attr(self, "canvas_port")
        try:
            if canvas_port is not None and hasattr(canvas_port, "end_render_session"):
                canvas_port.end_render_session(commit=commit)
            self._log_debug(
                "[描画セッション] 生成結果を確定しました"
                if commit
                else "[描画セッション] 生成結果をロールバックしました"
            )
        except Exception as exc:
            self._log_debug(f"[描画セッション終了失敗] {exc}")
            action = "確定" if commit else "ロールバック"
            message = f"描画セッションの{action}に失敗しました。キャンバスを確認し、必要に応じて手動で元に戻してください。\n\n{exc}"
            st = _get_attr(self, "status")
            if st is not None and hasattr(st, "setText"):
                st.setText(f"エラー: 描画セッションの{action}に失敗しました。キャンバスを確認してください。")
            if QMessageBox is not None and hasattr(QMessageBox, "critical"):
                QMessageBox.critical(self, "AI Stroke Painter 描画セッションエラー", message)
            return False
        finally:
            self._canvas_session_open = False
        return True

    def _reset_run_state(self, commit_session: bool = False) -> None:
        self._finish_canvas_session(commit_session)
        if not commit_session:
            prev_w = _get_attr(self, "preview")
            if prev_w is not None and hasattr(prev_w, "clear_plan"):
                with contextlib.suppress(Exception):
                    prev_w.clear_plan()
            if self.is_cancelled():
                st_w = _get_attr(self, "status")
                if st_w is not None and hasattr(st_w, "setText"):
                    curr_st = str(st_w.text()) if hasattr(st_w, "text") else str(getattr(st_w, "_text", ""))
                    if "停止要求" in curr_st:
                        st_w.setText("処理を停止しました。キャンバスは変更されていません。")
        prog = _get_attr(self, "progress")
        if prog is not None and hasattr(prog, "setRange"):
            prog.setRange(0, 1)
            prog.setValue(1)
        r_btn = _get_attr(self, "run_btn")
        if r_btn is not None and hasattr(r_btn, "setEnabled"):
            r_btn.setEnabled(True)
        self._set_generation_controls_enabled(True)
        s_btn = _get_attr(self, "stop_btn")
        if s_btn is not None and hasattr(s_btn, "setEnabled"):
            s_btn.setEnabled(False)
        apply_button = _get_attr(self, "apply_btn")
        if apply_button is not None and hasattr(apply_button, "setEnabled"):
            apply_button.setEnabled(False)
        self._active_doc = None
        self._active_view = None
        self._run_render_options = None
        self._session_plans = []
        self._pending_plan = None
        self._applying_pending = False
        self._worker = None
        self._update_action_buttons_state()
