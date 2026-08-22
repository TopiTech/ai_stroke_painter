"""Krita の paintLine API, レイヤー管理, カラーマネジメント, キャンバスキャプチャを CanvasPort に接続する Adapter。"""

from __future__ import annotations

from collections.abc import Callable
import contextlib
from typing import TYPE_CHECKING, Any

from .ports import CanvasPort
from .qt_compat import (
    QApplication,
    QBuffer,
    QByteArray,
    QColor,
    QIODevice,
    QPoint,
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
        """指定されたレイヤー名を取得または階層順を考慮して自動作成する。"""
        active = document.activeNode()
        if self._is_layer_match(active, layer_name):
            return active

        root = document.rootNode()
        existing = self._find_layer(root, layer_name)
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
            if higher_children and hasattr(root, "removeChildNode"):
                prev = node
                for hc in higher_children:
                    with contextlib.suppress(Exception):
                        root.removeChildNode(hc)
                        root.addChildNode(hc, prev)
                        prev = hc
        document.setActiveNode(node)
        return node

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
                        mode = getattr(QIODevice, "WriteOnly", 2) if QIODevice is not None else 2
                        qbuf.open(mode)
                        qimage.save(qbuf, "PNG")
                        data = ba.data() if hasattr(ba, "data") else b""
                        if data:
                            return bytes(data)
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
        global _last_applied_color
        _last_applied_color = None

        first_layer = plan.strokes[0].layer_name if plan.strokes else self.DEFAULT_LAYER_NAME
        current_node = self.ensure_layer(document, first_layer)
        current_layer_name = first_layer

        if cancelled():
            if hasattr(document, "refreshProjection"):
                with contextlib.suppress(Exception):
                    document.refreshProjection()
            return 0

        if not hasattr(current_node, "paintLine"):
            raise RuntimeError("このKritaには Node.paintLine がありません。Krita 6.0以降を使用してください。")
        paint_ability = current_node.paintAbility()
        if paint_ability != "PAINT":
            raise RuntimeError(f"対象レイヤーに描画できません（paintAbility: {paint_ability}）")

        rendered = 0
        segment_count = 0
        old_batchmode: bool | None = None

        # バッチモードを有効化して不要な待機ダイアログを抑止
        if hasattr(document, "setBatchmode") and hasattr(document, "batchmode"):
            with contextlib.suppress(Exception):
                old_batchmode = bool(document.batchmode())
                document.setBatchmode(True)

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

                _apply_color_to_krita(stroke.color)

                for start, end in zip(stroke.points, stroke.points[1:]):
                    if cancelled():
                        return rendered
                    current_node.paintLine(
                        _qpoint(start.x, start.y),
                        _qpoint(end.x, end.y),
                        start.pressure,
                        end.pressure,
                    )

                rendered += 1
                segment_count += max(1, len(stroke.points) - 1)
                if segment_count >= self.EVENT_INTERVAL:
                    segment_count = 0
                    _process_events()
        finally:
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


def _qpoint(x: float, y: float) -> Any:
    """Krita の Node.paintLine は QPoint (整数ピクセル座標) を要求するため QPoint を生成する。"""
    if QPoint is not None and callable(QPoint):
        return QPoint(round(x), round(y))
    return (round(x), round(y))


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


def _apply_color_to_krita(hex_color: str, force: bool = False) -> None:
    """Krita の描画前景色にストロークカラーを反映する。

    Node.paintLine は前景色で描画するため、ストローク色をアクティブビューの
    前景色 (View.setForeGroundColor) へ ManagedColor 経由で適用する。
    """
    global _last_applied_color
    if not force and hex_color == _last_applied_color:
        return

    try:
        from krita import Krita, ManagedColor

        rgb = _parse_hex_rgb(hex_color)
        if rgb is None:
            return
        if QColor is None or not hasattr(QColor, "fromRgbF"):
            return
        window = getattr(Krita.instance(), "activeWindow", lambda: None)()
        view = getattr(window, "activeView", lambda: None)() if window is not None else None
        if view is None:
            return
        view.setForeGroundColor(ManagedColor.fromQColor(QColor.fromRgbF(*rgb)))
        _last_applied_color = hex_color
    except Exception:
        pass


# 最小限の 1x1 白 PNG (フォールバック用)
_MINIMAL_PNG_BYTES = (
    b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00\x1f\x15\xc4\x89"
    b"\x00\x00\x00\rIDATx\x9cc\xf8\xff\xff?\x03\x00\x08\xfc\x02\xfe\xa7\x9a\xa0\xa0\x00\x00\x00\x00IEND\xaeB`\x82"
)
