"""Krita の paintLine API, レイヤー管理, カラーマネジメント, キャンバスキャプチャを CanvasPort に接続する Adapter。"""

from __future__ import annotations

from collections.abc import Callable
import contextlib
import math
from typing import TYPE_CHECKING, Any

from .domain import split_color_alpha
from .ports import CanvasPort
from .qt_compat import (
    QApplication,
    QBuffer,
    QByteArray,
    QColor,
    QIODevice,
    QPoint,
    QPointF,
    write_only_open_mode,
)

if TYPE_CHECKING:
    from .domain import DrawingPlan


# 標準的なレイヤー階層順序（インデックスが大きいほど上層/前面に配置）
LAYER_STACK_ORDER: dict[str, int] = {
    "Draft": 10,
    "Flats": 20,
    "Shading": 30,
    "Lineart": 40,
    "Highlights": 50,
    "FX": 60,
}


_last_applied_color: str | None = None


class KritaCanvasAdapter(CanvasPort):
    DEFAULT_GROUP_NAME = "AI Artwork"
    DEFAULT_LAYER_NAME = "AI Strokes (editable)"
    EVENT_INTERVAL = 30

    def __init__(
        self,
        brush_size_multiplier: float = 1.0,
        opacity_multiplier: float = 1.0,
        layer_mode: str = "multi_layer",
        layer_prefix: str = "AI Artwork",
        event_interval: int = 30,
    ) -> None:
        self.brush_size_multiplier = float(brush_size_multiplier)
        self.opacity_multiplier = float(opacity_multiplier)
        self.layer_mode = layer_mode
        self.layer_prefix = layer_prefix
        self.event_interval = max(1, int(event_interval))
        self._session_document: Any | None = None
        self._session_mode: str | None = None
        self._session_container: Any | None = None
        self._session_active_target: Any | None = None
        self._session_layer_cache: dict[str, Any] = {}

    def begin_render_session(self, document: Any) -> None:
        """複数の Auto-Refine 描画を一つのコミット／ロールバック単位として開始する。"""
        if document is None:
            raise ValueError("描画セッションにはドキュメントが必要です")
        if self._session_document is not None:
            self.end_render_session(commit=False)
        self._session_document = document
        self._session_mode = None
        self._session_container = None
        self._session_active_target = None
        self._session_layer_cache = {}

    def end_render_session(self, document: Any | None = None, *, commit: bool) -> None:
        """描画セッションを確定するか、その実行で生成したコンテナを除去する。"""
        session_document = self._session_document
        if session_document is None:
            return
        if document is not None and document is not session_document:
            raise RuntimeError("終了対象の描画セッションとドキュメントが一致しません")
        container = self._session_container
        try:
            if not commit and container is not None:
                self._remove_node(session_document.rootNode(), container)
                if hasattr(session_document, "waitForDone"):
                    with contextlib.suppress(Exception):
                        session_document.waitForDone()
                if hasattr(session_document, "refreshProjection"):
                    with contextlib.suppress(Exception):
                        session_document.refreshProjection()
        finally:
            self._clear_session()

    def _clear_session(self) -> None:
        self._session_document = None
        self._session_mode = None
        self._session_container = None
        self._session_active_target = None
        self._session_layer_cache = {}

    def ensure_target(self, document: Any) -> Any:
        return self.ensure_layer(document, self.DEFAULT_LAYER_NAME)

    def ensure_layer(
        self,
        document: Any,
        layer_name: str,
        blend_mode: str = "normal",
        opacity: float = 1.0,
        parent: Any | None = None,
        reuse_existing: bool = True,
        preserve_existing_order: bool = False,
    ) -> Any:
        """指定されたレイヤー名を取得または階層順を考慮して自動作成する。"""
        active = document.activeNode()
        if parent is None and reuse_existing and self._is_layer_match(active, layer_name):
            return active

        root = parent if parent is not None else document.rootNode()
        existing = self._find_layer(root, layer_name) if reuse_existing else None
        if existing is not None and existing is not root:
            document.setActiveNode(existing)
            return existing

        if not reuse_existing:
            layer_name = self._unique_child_name(root, layer_name)

        # レイヤーの新規作成
        node = document.createNode(layer_name, "paintlayer")
        if hasattr(node, "setBlendingMode") and blend_mode != "normal":
            with contextlib.suppress(Exception):
                node.setBlendingMode(blend_mode)
        if hasattr(node, "setOpacity") and opacity < 1.0:
            with contextlib.suppress(Exception):
                node.setOpacity(int(opacity * 255))

        # イラスト標準順序に基づいた適切な挿入位置の決定
        # Krita の Node.addChildNode(child, aboveThisNode) は aboveThisNode の「上」に挿入する。
        # そのため、target_rank 以下の最上位レイヤーを探し、その上に挿入する。
        target_rank = LAYER_STACK_ORDER.get(layer_name, 35)
        above_node = None
        for child in root.childNodes():
            child_name = getattr(child, "name", lambda: "")()
            child_rank = LAYER_STACK_ORDER.get(child_name, 35)
            if child_rank <= target_rank:
                above_node = child

        if above_node is not None:
            root.addChildNode(node, above_node)
        else:
            higher_children = [
                c
                for c in root.childNodes()
                if LAYER_STACK_ORDER.get(getattr(c, "name", lambda: "")(), 35) > target_rank
            ]
            root.addChildNode(node, None)
            if higher_children and not preserve_existing_order and hasattr(root, "removeChildNode"):
                prev = node
                for hc in higher_children:
                    removed = False
                    try:
                        root.removeChildNode(hc)
                        removed = True
                        root.addChildNode(hc, prev)
                        prev = hc
                    except Exception:
                        if removed:
                            with contextlib.suppress(Exception):
                                root.addChildNode(hc, None)
                        raise
        document.setActiveNode(node)
        return node

    def create_output_group(self, document: Any, group_name: str) -> Any:
        """既存作品と衝突しない、1実行専用の出力グループを作成する。"""
        root = document.rootNode()
        safe_name = group_name.strip() or self.DEFAULT_GROUP_NAME
        unique_name = self._unique_child_name(root, safe_name)
        node = document.createNode(unique_name, "grouplayer")
        root.addChildNode(node, None)
        document.setActiveNode(node)
        return node

    def _unique_child_name(self, parent: Any, base_name: str) -> str:
        names = {str(getattr(child, "name", lambda: "")()) for child in parent.childNodes()}
        if base_name not in names:
            return base_name
        suffix = 2
        while f"{base_name} ({suffix})" in names:
            suffix += 1
        return f"{base_name} ({suffix})"

    def capture_canvas(self, document: Any, width: int = 512, height: int = 512) -> bytes:
        """現在のキャンバス状態を PNG 画像バイト列としてキャプチャする（メインスレッド呼出推奨）。"""
        if hasattr(document, "waitForDone"):
            with contextlib.suppress(Exception):
                document.waitForDone()

        if hasattr(document, "thumbnail"):
            try:
                qimage = document.thumbnail(width, height)
                if qimage is not None and hasattr(qimage, "save"):
                    ba = QByteArray() if callable(QByteArray) else None
                    if ba is not None and callable(QBuffer):
                        qbuf: Any = QBuffer(ba)
                        mode = write_only_open_mode(QIODevice)
                        qbuf.open(mode)
                        qimage.save(qbuf, "PNG")
                        data = ba.data() if hasattr(ba, "data") else b""
                        if data:
                            return bytes(data)
            except Exception:
                pass

        # 取得失敗を白い画像で偽装せず、呼び出し側に「画像なし」として伝える。
        return b""

    def render(
        self,
        document: Any,
        plan: DrawingPlan,
        cancelled: Callable[[], bool] = lambda: False,
        brush_size_multiplier: float | None = None,
        opacity_multiplier: float | None = None,
        layer_mode: str | None = None,
        layer_prefix: str | None = None,
        event_interval: int | None = None,
        rollback_on_cancel: bool = True,
        view: Any | None = None,
        **kwargs: Any,
    ) -> int:
        global _last_applied_color
        _last_applied_color = None

        size_mult = float(brush_size_multiplier if brush_size_multiplier is not None else self.brush_size_multiplier)
        op_mult = float(opacity_multiplier if opacity_multiplier is not None else self.opacity_multiplier)
        mode = str(layer_mode if layer_mode is not None else self.layer_mode)
        prefix = str(layer_prefix if layer_prefix is not None else self.layer_prefix)
        evt_interval = max(1, int(event_interval if event_interval is not None else self.event_interval))
        if not math.isfinite(size_mult) or size_mult <= 0:
            raise ValueError("ブラシ太さ倍率は正の有限数値である必要があります")
        if not math.isfinite(op_mult) or not 0.0 <= op_mult <= 1.0:
            raise ValueError("不透明度倍率は 0.0 から 1.0 の有限数値である必要があります")
        if mode not in {"multi_layer", "active_layer", "single_layer"}:
            raise ValueError(f"未対応のレイヤーモードです: {mode}")
        if not plan.strokes:
            return 0

        session_active = self._session_document is document
        if session_active and self._session_mode is not None and self._session_mode != mode:
            raise RuntimeError("描画セッション中にレイヤーモードは変更できません")

        target_view = view or _active_view()
        if view is not None and target_view is not None and hasattr(target_view, "document"):
            view_document = target_view.document()
            if view_document is not None and view_document != document:
                raise RuntimeError("描画開始時のKritaビューと対象ドキュメントが一致しません")
        view_state = _capture_view_state(target_view)

        original_active = document.activeNode()
        generated_container: Any | None = None
        output_group: Any | None = None
        layer_cache: dict[str, Any] = {}
        current_node: Any | None = None
        current_layer_name = ""
        rendered = 0
        segment_count = 0
        old_batchmode: bool | None = None
        completed = False
        use_float_points = QPointF is not None and callable(QPointF)

        try:
            if mode == "active_layer":
                current_node = self._session_active_target if session_active else document.activeNode()
                if current_node is None:
                    current_node = self.ensure_layer(document, self.DEFAULT_LAYER_NAME, preserve_existing_order=True)
                    generated_container = current_node
                if session_active and self._session_active_target is None:
                    self._session_active_target = current_node
                current_layer_name = getattr(current_node, "name", lambda: self.DEFAULT_LAYER_NAME)()
            elif mode == "single_layer":
                current_node = self._session_container if session_active else None
                single_name = f"{prefix} (Combined)" if prefix else self.DEFAULT_LAYER_NAME
                if current_node is None:
                    current_node = self.ensure_layer(
                        document, single_name, reuse_existing=False, preserve_existing_order=True
                    )
                generated_container = current_node
                current_layer_name = getattr(current_node, "name", lambda: single_name)()
            else:
                output_group = self._session_container if session_active else None
                if output_group is None:
                    output_group = self.create_output_group(document, prefix)
                generated_container = output_group
                layer_cache = self._session_layer_cache if session_active else {}
                first_layer = plan.strokes[0].layer_name
                current_node = layer_cache.get(first_layer)
                if current_node is None:
                    current_node = self.ensure_layer(document, first_layer, parent=output_group)
                    layer_cache[first_layer] = current_node
                current_layer_name = first_layer

            if session_active:
                self._session_mode = mode
                if generated_container is not None:
                    self._session_container = generated_container

            if cancelled():
                return 0

            # バッチモードを有効化して不要な待機ダイアログを抑止
            if hasattr(document, "setBatchmode") and hasattr(document, "batchmode"):
                with contextlib.suppress(Exception):
                    old_batchmode = bool(document.batchmode())
                    document.setBatchmode(True)

            for stroke in plan.strokes:
                if cancelled():
                    break

                if mode == "multi_layer":
                    target_layer_name = stroke.layer_name or self.DEFAULT_LAYER_NAME
                    if target_layer_name != current_layer_name or current_node is None:
                        current_node = layer_cache.get(target_layer_name)
                        if current_node is None:
                            current_node = self.ensure_layer(document, target_layer_name, parent=output_group)
                            layer_cache[target_layer_name] = current_node
                        current_layer_name = target_layer_name

                if not hasattr(current_node, "paintLine"):
                    raise RuntimeError("このKritaには Node.paintLine がありません。Krita 6.0以降を使用してください。")
                _apply_stroke_style(stroke, size_multiplier=size_mult, opacity_multiplier=op_mult, view=target_view)
                paint_ability = current_node.paintAbility()
                if paint_ability != "PAINT":
                    raise RuntimeError(f"対象レイヤーに描画できません（paintAbility: {paint_ability}）")

                _apply_color_to_krita(stroke.color, view=target_view)

                for start, end in zip(stroke.points, stroke.points[1:], strict=False):
                    if cancelled():
                        return rendered
                    if use_float_points:
                        try:
                            current_node.paintLine(
                                _qpoint_float(start.x, start.y),
                                _qpoint_float(end.x, end.y),
                                start.pressure,
                                end.pressure,
                            )
                            continue
                        except TypeError:
                            # 一部の Krita Python バインディングは QPoint のみを受け付ける。
                            use_float_points = False
                    current_node.paintLine(
                        _qpoint(start.x, start.y), _qpoint(end.x, end.y), start.pressure, end.pressure
                    )

                rendered += 1
                segment_count += max(1, len(stroke.points) - 1)
                if segment_count >= evt_interval:
                    segment_count = 0
                    _process_events()
            completed = not cancelled()
        finally:
            if generated_container is not None and rollback_on_cancel and not completed:
                self._remove_node(document.rootNode(), generated_container)
                if session_active and generated_container is self._session_container:
                    self._clear_session()
            # キュー内の描画ジョブ完了を安全に待機してからプロジェクションを更新
            if hasattr(document, "waitForDone"):
                with contextlib.suppress(Exception):
                    document.waitForDone()
            if hasattr(document, "refreshProjection"):
                with contextlib.suppress(Exception):
                    document.refreshProjection()
            if old_batchmode is not None and hasattr(document, "setBatchmode"):
                with contextlib.suppress(Exception):
                    document.setBatchmode(old_batchmode)
            if hasattr(document, "setActiveNode"):
                restore_node = original_active if original_active is not None else document.rootNode()
                with contextlib.suppress(Exception):
                    document.setActiveNode(restore_node)
            _restore_view_state(target_view, view_state)

        return rendered

    def _is_layer_match(self, node: Any, layer_name: str) -> bool:
        return bool(node is not None and node.name() == layer_name and node.type() == "paintlayer")

    def _find_layer(self, root: Any, layer_name: str) -> Any:
        pending = [root]
        while pending:
            node = pending.pop()
            if self._is_layer_match(node, layer_name):
                return node
            pending.extend(node.childNodes())
        return None

    def _remove_node(self, parent: Any, node: Any) -> None:
        if hasattr(parent, "removeChildNode"):
            with contextlib.suppress(Exception):
                parent.removeChildNode(node)
                return
        if hasattr(node, "remove"):
            with contextlib.suppress(Exception):
                node.remove()


def _qpoint(x: float, y: float) -> Any:
    """QPoint のみを受け付ける Krita バインディング向けの整数座標を生成する。"""
    if QPoint is not None and callable(QPoint):
        return QPoint(round(x), round(y))
    return (round(x), round(y))


def _qpoint_float(x: float, y: float) -> Any:
    """QPointF を受け付ける Krita バインディング向けに座標精度を維持する。"""
    if QPointF is not None and callable(QPointF):
        return QPointF(float(x), float(y))
    return _qpoint(x, y)


# 後方互換エイリアス
_qpointf = _qpoint


def _process_events() -> None:
    if QApplication is not None and hasattr(QApplication, "processEvents"):
        QApplication.processEvents()


def _parse_hex_rgb(hex_str: str) -> tuple[float, float, float] | None:
    h = hex_str.lstrip("#")
    if len(h) in (3, 4):
        return int(h[0] * 2, 16) / 255.0, int(h[1] * 2, 16) / 255.0, int(h[2] * 2, 16) / 255.0
    if len(h) in (6, 8):
        return int(h[0:2], 16) / 255.0, int(h[2:4], 16) / 255.0, int(h[4:6], 16) / 255.0
    return None


def _active_view() -> Any | None:
    try:
        from krita import Krita

        window = getattr(Krita.instance(), "activeWindow", lambda: None)()
        return getattr(window, "activeView", lambda: None)() if window is not None else None
    except Exception:
        return None


def _capture_view_state(view: Any | None) -> dict[str, Any]:
    if view is None:
        return {}
    getters = {
        "preset": ("currentBrushPreset",),
        "size": ("brushSize",),
        "opacity": ("paintingOpacity",),
        "color": ("foregroundColor", "foreGroundColor"),
    }
    state: dict[str, Any] = {}
    for key, names in getters.items():
        for name in names:
            getter = getattr(view, name, None)
            if callable(getter):
                with contextlib.suppress(Exception):
                    state[key] = getter()
                    break
    return state


def _restore_view_state(view: Any | None, state: dict[str, Any]) -> None:
    if view is None:
        return
    setters = {
        "preset": "setCurrentBrushPreset",
        "size": "setBrushSize",
        "opacity": "setPaintingOpacity",
        "color": "setForeGroundColor",
    }
    for key, setter_name in setters.items():
        if key not in state:
            continue
        setter = getattr(view, setter_name, None)
        if callable(setter):
            with contextlib.suppress(Exception):
                setter(state[key])


def _apply_color_to_krita(hex_color: str, force: bool = False, view: Any | None = None) -> None:
    """Krita の描画前景色にストロークカラーを反映する。

    Node.paintLine は前景色で描画するため、ストローク色をアクティブビューの
    前景色 (View.setForeGroundColor) へ ManagedColor 経由で適用する。
    """
    global _last_applied_color
    if not force and hex_color == _last_applied_color:
        return

    target_view = view or _active_view()
    if target_view is None or not hasattr(target_view, "setForeGroundColor"):
        return
    try:
        from krita import ManagedColor

        rgb = _parse_hex_rgb(hex_color)
        if rgb is None:
            return
        if QColor is None or not hasattr(QColor, "fromRgbF"):
            return
        target_view.setForeGroundColor(ManagedColor.fromQColor(QColor.fromRgbF(*rgb)))
        _last_applied_color = hex_color
    except Exception:
        if view is not None:
            raise


def _apply_stroke_style(
    stroke: Any,
    size_multiplier: float = 1.0,
    opacity_multiplier: float = 1.0,
    view: Any | None = None,
) -> None:
    """Apply the DrawingPlan brush contract to Krita's active view."""
    try:
        from krita import Krita

        app = Krita.instance()
        target_view = view or _active_view()
        if target_view is None:
            return

        preset_name = str(stroke.brush_preset).strip()
        presets: Any = getattr(app, "resources", lambda _kind: {})("preset")
        preset: Any = presets.get(preset_name) if hasattr(presets, "get") else None
        if preset is None and hasattr(presets, "values"):
            preset = next(
                (
                    candidate
                    for candidate in presets.values()
                    if getattr(candidate, "name", lambda: "")() == preset_name
                ),
                None,
            )
        if preset is not None and hasattr(target_view, "setCurrentBrushPreset"):
            target_view.setCurrentBrushPreset(preset)

        effective_size = max(0.5, float(stroke.size_px) * size_multiplier)
        _rgb_color, color_alpha = split_color_alpha(stroke.color)
        effective_opacity = max(0.0, min(1.0, float(stroke.opacity) * opacity_multiplier * color_alpha))

        if hasattr(target_view, "setBrushSize"):
            target_view.setBrushSize(effective_size)
        if hasattr(target_view, "setPaintingOpacity"):
            target_view.setPaintingOpacity(effective_opacity)
    except Exception:
        if view is not None:
            raise
