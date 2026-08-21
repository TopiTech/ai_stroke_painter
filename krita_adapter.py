"""Krita の paintLine API, レイヤー管理, カラーマネジメント, キャンバスキャプチャを CanvasPort に接続する Adapter。"""

from __future__ import annotations

from collections.abc import Callable
import contextlib
import importlib
from typing import TYPE_CHECKING, Any

from .ports import CanvasPort

if TYPE_CHECKING:
    from .domain import DrawingPlan


class KritaCanvasAdapter(CanvasPort):
    DEFAULT_GROUP_NAME = "AI Artwork"
    DEFAULT_LAYER_NAME = "AI Strokes (editable)"
    EVENT_INTERVAL = 15

    def __init__(self) -> None:
        self._layer_cache: dict[str, Any] = {}

    def ensure_target(self, document: Any) -> Any:
        return self.ensure_layer(document, self.DEFAULT_LAYER_NAME)

    def ensure_layer(
        self,
        document: Any,
        layer_name: str,
        blend_mode: str = "normal",
        opacity: float = 1.0,
    ) -> Any:
        """指定されたレイヤー名を取得または自動作成する。"""
        active = document.activeNode()
        if self._is_layer_match(active, layer_name):
            return active

        existing = self._find_layer(document.rootNode(), layer_name)
        if existing is not None:
            document.setActiveNode(existing)
            return existing

        # レイヤーの新規作成
        node = document.createNode(layer_name, "paintlayer")
        if hasattr(node, "setBlendingMode") and blend_mode != "normal":
            with contextlib.suppress(Exception):
                node.setBlendingMode(blend_mode)
        if hasattr(node, "setOpacity") and opacity < 1.0:
            with contextlib.suppress(Exception):
                node.setOpacity(int(opacity * 255))

        document.rootNode().addChildNode(node, None)
        document.setActiveNode(node)
        return node

    def capture_canvas(self, document: Any, width: int = 512, height: int = 512) -> bytes:
        """現在のキャンバス状態を PNG 画像バイト列としてキャプチャする。"""
        if hasattr(document, "thumbnail"):
            try:
                qimage = document.thumbnail(width, height)
                if qimage is not None and hasattr(qimage, "save"):
                    buffer, qbuffer_cls, io_device_cls = _resolve_qbuffer()
                    if buffer is not None and qbuffer_cls is not None:
                        qbuf = qbuffer_cls(buffer)
                        qbuf.open(io_device_cls.WriteOnly if io_device_cls else 2)
                        qimage.save(qbuf, "PNG")
                        return bytes(buffer.data())
            except Exception:
                pass

        # フォールバック: 最小の 1x1 白 PNG バイト列
        return _MINIMAL_PNG_BYTES

    def render(
        self,
        document: Any,
        plan: DrawingPlan,
        cancelled: Callable[[], bool] = lambda: False,
    ) -> int:
        first_layer = plan.strokes[0].layer_name if plan.strokes else self.DEFAULT_LAYER_NAME
        current_node = self.ensure_layer(document, first_layer)
        current_layer_name = first_layer

        if cancelled():
            document.refreshProjection()
            return 0

        if not hasattr(current_node, "paintLine"):
            raise RuntimeError("このKritaには Node.paintLine がありません。Krita 6.0以降を使用してください。")
        paint_ability = current_node.paintAbility()
        if paint_ability != "PAINT":
            raise RuntimeError(f"対象レイヤーに描画できません（paintAbility: {paint_ability}）")

        rendered = 0
        segment_count = 0

        try:
            for stroke in plan.strokes:
                if cancelled():
                    break

                target_layer_name = stroke.layer_name or self.DEFAULT_LAYER_NAME
                if target_layer_name != current_layer_name or current_node is None:
                    current_node = self.ensure_layer(document, target_layer_name, opacity=stroke.opacity)
                    current_layer_name = target_layer_name

                if not hasattr(current_node, "paintLine"):
                    raise RuntimeError("このKritaには Node.paintLine がありません。Krita 6.0以降を使用してください。")
                paint_ability = current_node.paintAbility()
                if paint_ability != "PAINT":
                    raise RuntimeError(f"対象レイヤーに描画できません（paintAbility: {paint_ability}）")

                _apply_color_to_krita(document, stroke.color)

                for start, end in zip(stroke.points, stroke.points[1:]):
                    if cancelled():
                        return rendered
                    current_node.paintLine(
                        _qpoint(start.x, start.y),
                        _qpoint(end.x, end.y),
                        start.pressure,
                        end.pressure,
                    )
                    segment_count += 1
                    if segment_count % self.EVENT_INTERVAL == 0:
                        _process_events()
                rendered += 1
        finally:
            document.refreshProjection()
        return rendered

    def _is_layer_match(self, node: Any, layer_name: str) -> bool:
        return node is not None and node.name() == layer_name and node.type() == "paintlayer"

    def _find_layer(self, root: Any, layer_name: str) -> Any | None:
        pending = [root]
        while pending:
            node = pending.pop()
            if self._is_layer_match(node, layer_name):
                return node
            pending.extend(node.childNodes())
        return None


def _resolve_qt() -> tuple[Any, Any]:
    for module_base in ("PyQt5", "PyQt6"):
        try:
            core = importlib.import_module(f"{module_base}.QtCore")
            widgets = importlib.import_module(f"{module_base}.QtWidgets")
            qpoint = getattr(core, "QPoint", None)
            qapp = getattr(widgets, "QApplication", None)
            if qpoint is not None:
                return qpoint, qapp
        except (ImportError, AttributeError):
            continue
    return None, None


def _resolve_qbuffer() -> tuple[Any, Any, Any]:
    for module_base in ("PyQt5", "PyQt6"):
        try:
            core = importlib.import_module(f"{module_base}.QtCore")
            qbytearray = getattr(core, "QByteArray", None)
            qbuffer = getattr(core, "QBuffer", None)
            qiodevice = getattr(core, "QIODevice", None)
            if qbytearray is not None and qbuffer is not None:
                return qbytearray(), qbuffer, qiodevice
        except (ImportError, AttributeError):
            continue
    return None, None, None


_QPOINT_CLS, _QAPP_CLS = _resolve_qt()


def _qpoint(x: float, y: float) -> Any:
    if _QPOINT_CLS is not None:
        return _QPOINT_CLS(int(round(x)), int(round(y)))
    return (int(round(x)), int(round(y)))


def _process_events() -> None:
    if _QAPP_CLS is not None and hasattr(_QAPP_CLS, "processEvents"):
        _QAPP_CLS.processEvents()


def _apply_color_to_krita(document: Any, hex_color: str) -> None:
    """Krita の描画前景色にストロークカラーを反映する。"""
    try:
        from krita import Krita, ManagedColor

        krita_inst = Krita.instance()
        doc = document or krita_inst.activeDocument()
        if doc is not None and hasattr(krita_inst, "setManagedColor"):
            r = int(hex_color[1:3], 16) / 255.0
            g = int(hex_color[3:5], 16) / 255.0
            b = int(hex_color[5:7], 16) / 255.0
            mc = ManagedColor.fromColor(r, g, b, 1.0)
            doc.setCurrentColor(mc)
    except Exception:
        pass


# 最小限の 1x1 白 PNG (フォールバック用)
_MINIMAL_PNG_BYTES = (
    b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00\x1f\x15c4"
    b"\x00\x00\x00\rIDATx\x9cc\xf8\xff\xff?\x03\x00\x08\xfc\x02\xfe\xa7\x9a\xa0\xa0\x00\x00\x00\x00IEND\xaeB`\x82"
)
