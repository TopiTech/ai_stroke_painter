"""Krita の paintLine API を CanvasPort に接続する Adapter。"""

from __future__ import annotations

from .ports import CanvasPort


class KritaCanvasAdapter(CanvasPort):
    LAYER_NAME = "AI Strokes (editable)"
    EVENT_INTERVAL = 12

    def ensure_target(self, document):
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

    def render(self, document, plan, cancelled=lambda: False):
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
                    node.paintLine(_qpoint(start.x, start.y), _qpoint(end.x, end.y), start.pressure, end.pressure)
                    segment_count += 1
                    if segment_count % self.EVENT_INTERVAL == 0:
                        _process_events()
                rendered += 1
        finally:
            document.refreshProjection()
        return rendered

    def _is_target(self, node):
        return node is not None and node.name() == self.LAYER_NAME and node.type() == "paintlayer"

    def _find_target(self, root):
        pending = [root]
        while pending:
            node = pending.pop()
            if self._is_target(node):
                return node
            pending.extend(node.childNodes())
        return None


def _qpoint(x, y):
    try:
        from PyQt5.QtCore import QPoint
    except ImportError:
        from PyQt6.QtCore import QPoint
    # Krita の Node.paintLine は整数座標の QPoint を要求する。
    return QPoint(int(round(x)), int(round(y)))


def _process_events():
    """長い描画中にも停止ボタンのクリックを処理する。"""
    try:
        try:
            from PyQt5.QtWidgets import QApplication
        except ImportError:
            from PyQt6.QtWidgets import QApplication
        QApplication.processEvents()
    except ImportError:
        # Krita 外の純粋 Python テストでは Qt に依存しない。
        pass
