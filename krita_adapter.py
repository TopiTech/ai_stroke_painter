"""Krita の paintLine API を CanvasPort に接続する Adapter。"""

from __future__ import annotations

from collections.abc import Callable
import importlib
from typing import TYPE_CHECKING, Any

from .ports import CanvasPort

if TYPE_CHECKING:
    from .domain import DrawingPlan


class KritaCanvasAdapter(CanvasPort):
    LAYER_NAME = "AI Strokes (editable)"
    EVENT_INTERVAL = 12

    def ensure_target(self, document: Any) -> Any:
        active = document.activeNode()
        if self._is_target(active):
            return active

        existing = self._find_target(document.rootNode())
        if existing is not None:
            document.setActiveNode(existing)
            return existing

        node = document.createNode(self.LAYER_NAME, "paintlayer")
        document.rootNode().addChildNode(node, None)
        document.setActiveNode(node)
        return node

    def render(
        self,
        document: Any,
        plan: DrawingPlan,
        cancelled: Callable[[], bool] = lambda: False,
    ) -> int:
        node = self.ensure_target(document)
        if not hasattr(node, "paintLine"):
            raise RuntimeError("このKritaには Node.paintLine がありません。Krita 6.0以降を使用してください。")
        paint_ability = node.paintAbility()
        if paint_ability != "PAINT":
            raise RuntimeError(f"現在のブラシでは対象レイヤーに描画できません（paintAbility: {paint_ability}）")

        rendered = 0
        segment_count = 0
        try:
            for stroke in plan.strokes:
                if cancelled():
                    break
                for start, end in zip(stroke.points, stroke.points[1:]):
                    if cancelled():
                        return rendered
                    # Krita の paintLine は pressure を 0.0–1.0 で受け取る。
                    node.paintLine(
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

    def _is_target(self, node: Any) -> bool:
        return node is not None and node.name() == self.LAYER_NAME and node.type() == "paintlayer"

    def _find_target(self, root: Any) -> Any | None:
        pending = [root]
        while pending:
            node = pending.pop()
            if self._is_target(node):
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


_QPOINT_CLS, _QAPP_CLS = _resolve_qt()


def _qpoint(x: float, y: float) -> Any:
    if _QPOINT_CLS is not None:
        # Krita の Node.paintLine は整数座標の QPoint を要求する。
        return _QPOINT_CLS(int(round(x)), int(round(y)))
    # PyQt がない環境（テスト等）用のフォールバック
    return (int(round(x)), int(round(y)))


def _process_events() -> None:
    """長い描画中にも停止ボタンのクリックを処理する。"""
    if _QAPP_CLS is not None and hasattr(_QAPP_CLS, "processEvents"):
        _QAPP_CLS.processEvents()
