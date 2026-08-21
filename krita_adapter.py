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


def _qpoint(x: float, y: float) -> Any:
    for module_name in ("PyQt5.QtCore", "PyQt6.QtCore"):
        try:
            module = importlib.import_module(module_name)
            qpoint_cls = getattr(module, "QPoint", None)
            if qpoint_cls is not None:
                # Krita の Node.paintLine は整数座標の QPoint を要求する。
                return qpoint_cls(int(round(x)), int(round(y)))
        except (ImportError, AttributeError):
            continue
    # PyQt がない環境（テスト等）用のフォールバック
    return (int(round(x)), int(round(y)))


def _process_events() -> None:
    """長い描画中にも停止ボタンのクリックを処理する。"""
    for module_name in ("PyQt5.QtWidgets", "PyQt6.QtWidgets"):
        try:
            module = importlib.import_module(module_name)
            app_cls = getattr(module, "QApplication", None)
            if app_cls is not None and hasattr(app_cls, "processEvents"):
                app_cls.processEvents()
                return
        except (ImportError, AttributeError):
            continue
