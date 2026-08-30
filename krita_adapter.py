"""Krita の paintLine API, レイヤー管理, カラーマネジメント, キャンバスキャプチャを CanvasPort に接続する Adapter。"""

from __future__ import annotations

from collections.abc import Callable, Sequence, Sized
import contextlib
from dataclasses import dataclass
import hashlib
import math
import re
from typing import TYPE_CHECKING, Any

from .brushes import brush_definition, infer_brush_profile
from .domain import LAYER_RENDER_ORDER, Stroke, StrokePoint, split_color_alpha
from .native_bridge import NativeBridgeUnavailable, discover_native_bridge
from .ports import CanvasPort, NativeStrokeBridgePort
from .qt_compat import (
    QApplication,
    QBuffer,
    QByteArray,
    QColor,
    QEvent,
    QIODevice,
    QObject,
    QPainterPath,
    QPoint,
    QPointF,
    write_only_open_mode,
)

if TYPE_CHECKING:
    from .domain import DrawingPlan


# 標準的なレイヤー階層順序（インデックスが大きいほど上層/前面に配置）
LAYER_STACK_ORDER = LAYER_RENDER_ORDER
MAX_ACTIVE_LAYER_SNAPSHOT_BYTES = 512 * 1024 * 1024


_last_applied_color: str | None = None


@dataclass(frozen=True)
class _LayerSnapshot:
    """既存ペイントレイヤーを失敗前の状態へ戻すための画素スナップショット。"""

    node: Any
    pixels: Any
    x: int
    y: int
    width: int
    height: int


@dataclass(frozen=True)
class _LayerFingerprint:
    """Full-pixel state identifier retained without another full image copy."""

    node: Any
    width: int
    height: int
    digest: bytes


class ActiveLayerSessionConflict(RuntimeError):
    """A direct target changed outside this render session, so rollback is unsafe."""


def _canvas_input_event_types() -> frozenset[Any]:
    event_type = getattr(QEvent, "Type", QEvent)
    return frozenset(
        value
        for name in (
            "MouseButtonPress",
            "MouseButtonRelease",
            "MouseButtonDblClick",
            "MouseMove",
            "KeyPress",
            "KeyRelease",
            "ShortcutOverride",
            "TabletMove",
            "TabletPress",
            "TabletRelease",
            "TouchBegin",
            "TouchUpdate",
            "TouchEnd",
        )
        if (value := getattr(event_type, name, getattr(QEvent, name, None))) is not None
    )


_CANVAS_INPUT_EVENT_TYPES = _canvas_input_event_types()


class _CanvasInputGuard(QObject):
    """Keep direct-layer pixels stable while a synchronous render pumps Qt events."""

    def eventFilter(self, _watched: Any, event: Any) -> bool:  # noqa: N802
        event_type = getattr(event, "type", None)
        value = event_type() if callable(event_type) else None
        return value in _CANVAS_INPUT_EVENT_TYPES


def _install_canvas_input_guard(view: Any | None) -> tuple[Any, _CanvasInputGuard] | None:
    canvas_getter = getattr(view, "canvas", None)
    if not callable(canvas_getter):
        return None
    try:
        canvas = canvas_getter()
    except Exception:
        return None
    install = getattr(canvas, "installEventFilter", None)
    if not callable(install):
        return None
    guard = _CanvasInputGuard()
    try:
        install(guard)
    except Exception:
        return None
    return canvas, guard


def _remove_canvas_input_guard(guard_state: tuple[Any, _CanvasInputGuard] | None) -> None:
    if guard_state is None:
        return
    canvas, guard = guard_state
    remove = getattr(canvas, "removeEventFilter", None)
    if callable(remove):
        with contextlib.suppress(Exception):
            remove(guard)


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
        native_bridge: NativeStrokeBridgePort | None = None,
    ) -> None:
        self.brush_size_multiplier = float(brush_size_multiplier)
        self.opacity_multiplier = float(opacity_multiplier)
        self.layer_mode = layer_mode
        self.layer_prefix = layer_prefix
        self.event_interval = max(1, int(event_interval))
        if native_bridge is not None:
            self.native_bridge: NativeStrokeBridgePort | None = native_bridge
        else:
            # 環境変数の設定ミスでプラグイン全体を開始不能にしないよう、bridge だけ無効化する
            try:
                self.native_bridge = discover_native_bridge()
            except ValueError:
                self.native_bridge = None
        self._session_document: Any | None = None
        self._session_mode: str | None = None
        self._session_container: Any | None = None
        self._session_active_target: Any | None = None
        self._session_active_snapshot: _LayerSnapshot | None = None
        self._session_active_expected: _LayerFingerprint | None = None
        self._session_active_conflict: str | None = None
        self._session_active_created: bool = False
        self._session_layer_cache: dict[str, Any] = {}
        self._session_macro_open: bool = False
        self._session_has_changes: bool = False
        self._brush_preset_cache: dict[tuple[str, bool], Any] = {}
        self.last_render_trace: dict[str, Any] = {}

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
        self._session_active_snapshot = None
        self._session_active_expected = None
        self._session_active_conflict = None
        self._session_active_created = False
        self._session_layer_cache = {}
        self._session_has_changes = False
        self._session_macro_open = _start_macro(document, "AI Stroke Painter Session")

    def end_render_session(self, document: Any | None = None, *, commit: bool) -> None:
        """描画セッションを確定するか、その実行の変更だけをロールバックする。"""
        session_document = self._session_document
        if session_document is None:
            return
        if document is not None and document is not session_document:
            raise RuntimeError("終了対象の描画セッションとドキュメントが一致しません")
        container = self._session_container
        try:
            if not commit and self._session_has_changes:
                if hasattr(session_document, "waitForDone"):
                    with contextlib.suppress(Exception):
                        session_document.waitForDone()
                if self._session_mode == "active_layer":
                    active_target = self._session_active_target
                    if self._session_active_created and container is not None:
                        self._assert_active_session_rollback_safe(session_document, active_target)
                        self._remove_node(session_document.rootNode(), container)
                    elif self._session_active_snapshot is not None:
                        self._assert_active_session_rollback_safe(session_document, active_target)
                        _restore_layer_snapshot(self._session_active_snapshot)
                elif container is not None:
                    self._remove_node(session_document.rootNode(), container)
                if hasattr(session_document, "refreshProjection"):
                    with contextlib.suppress(Exception):
                        session_document.refreshProjection()
        finally:
            try:
                if self._session_macro_open:
                    _end_macro(session_document)
            finally:
                self._clear_session()

    def _clear_session(self) -> None:
        self._session_document = None
        self._session_mode = None
        self._session_container = None
        self._session_active_target = None
        self._session_active_snapshot = None
        self._session_active_expected = None
        self._session_active_conflict = None
        self._session_active_created = False
        self._session_layer_cache = {}
        self._session_macro_open = False
        self._session_has_changes = False

    def _record_active_session_state(self, document: Any, node: Any) -> None:
        try:
            self._session_active_expected = _fingerprint_layer(document, node)
        except Exception as exc:
            self._session_active_conflict = "描画後のアクティブレイヤー状態を検証できませんでした"
            raise ActiveLayerSessionConflict(
                "アクティブレイヤーの描画後状態を検証できないため、安全な自動ロールバックを継続できません"
            ) from exc

    def _assert_active_session_rollback_safe(self, document: Any, node: Any | None) -> None:
        if self._session_active_conflict is not None:
            raise ActiveLayerSessionConflict(
                f"{self._session_active_conflict}。アクティブレイヤーを上書きしないため、自動ロールバックを中止しました"
            )
        expected = self._session_active_expected
        if node is None or expected is None:
            self._session_active_conflict = "アクティブレイヤーの復元対象を確認できませんでした"
            raise ActiveLayerSessionConflict(
                "アクティブレイヤーの復元対象を確認できないため、自動ロールバックを中止しました"
            )
        node_match = node is expected.node
        if not node_match:
            with contextlib.suppress(Exception):
                node_match = bool(node == expected.node)
        if not node_match:
            self._session_active_conflict = "アクティブレイヤーの復元対象を確認できませんでした"
            raise ActiveLayerSessionConflict(
                "アクティブレイヤーの復元対象を確認できないため、自動ロールバックを中止しました"
            )
        try:
            actual = _fingerprint_layer(document, node)
        except Exception as exc:
            self._session_active_conflict = "アクティブレイヤーの現在状態を検証できませんでした"
            raise ActiveLayerSessionConflict(
                "アクティブレイヤーの現在状態を検証できないため、自動ロールバックを中止しました"
            ) from exc
        if not _fingerprints_match(expected, actual):
            self._session_active_conflict = "アクティブレイヤーが描画セッション中に外部変更されました"
            raise ActiveLayerSessionConflict(
                "アクティブレイヤーが描画セッション中に外部変更されたため、変更を上書きしないよう自動ロールバックを中止しました"
            )

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
        if node is None:
            raise RuntimeError(f"Kritaがペイントレイヤーを作成できませんでした: {layer_name}")
        effective_blend = blend_mode
        if effective_blend == "normal":
            lname_lower = layer_name.lower()
            if "shading" in lname_lower or "shadow" in lname_lower:
                effective_blend = "multiply"
            elif "highlight" in lname_lower or "fx" in lname_lower or "glow" in lname_lower:
                effective_blend = "addition"

        if hasattr(node, "setBlendingMode") and effective_blend != "normal":
            with contextlib.suppress(Exception):
                node.setBlendingMode(effective_blend)
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
            self._add_child_node(root, node, above_node)
        else:
            higher_children = [
                c
                for c in root.childNodes()
                if LAYER_STACK_ORDER.get(getattr(c, "name", lambda: "")(), 35) > target_rank
            ]
            self._add_child_node(root, node, None)
            if higher_children and not preserve_existing_order and hasattr(root, "removeChildNode"):
                prev = node
                for hc in higher_children:
                    removed = False
                    try:
                        self._remove_node(root, hc)
                        removed = True
                        self._add_child_node(root, hc, prev)
                        prev = hc
                    except Exception:
                        if removed:
                            with contextlib.suppress(Exception):
                                self._add_child_node(root, hc, None)
                        raise
        document.setActiveNode(node)
        return node

    def create_output_group(self, document: Any, group_name: str) -> Any:
        """既存作品と衝突しない、1実行専用の出力グループを作成する。"""
        root = document.rootNode()
        safe_name = group_name.strip() or self.DEFAULT_GROUP_NAME
        unique_name = self._unique_child_name(root, safe_name)
        node = document.createNode(unique_name, "grouplayer")
        if node is None:
            raise RuntimeError(f"Kritaが出力グループを作成できませんでした: {unique_name}")
        self._add_child_node(root, node, None)
        document.setActiveNode(node)
        return node

    def _add_child_node(self, parent: Any, child: Any, above: Any | None) -> None:
        add_child = getattr(parent, "addChildNode", None)
        if not callable(add_child):
            raise RuntimeError("Kritaノードが子レイヤー追加APIに対応していません")
        try:
            result = add_child(child, above)
        except Exception as exc:
            raise RuntimeError("Kritaレイヤー階層への追加に失敗しました") from exc
        if result is False:
            raise RuntimeError("Kritaがレイヤー階層への追加を拒否しました")

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
                        try:
                            qimage.save(qbuf, "PNG")
                            data: Any = ba.data() if hasattr(ba, "data") else b""
                            if data:
                                return bytes(data)
                        finally:
                            with contextlib.suppress(Exception):
                                qbuf.close()
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
        draft_preview_only = plan.metadata.get("draft_policy") == "preview_only"
        render_strokes = tuple(
            stroke
            for stroke in plan.strokes
            if not (draft_preview_only and stroke.layer_name.casefold().startswith("draft"))
        )
        self.last_render_trace = {
            "draft_policy": "preview_only" if draft_preview_only else "render",
            "draft_skipped": len(plan.strokes) - len(render_strokes),
            "routes": {"native_bridge": 0, "continuous_path": 0, "segmented_line": 0},
            "resolved_presets": {},
        }
        if not render_strokes:
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
        mutated = False
        segment_count = 0
        old_batchmode: bool | None = None
        completed = False
        use_float_points = QPointF is not None and callable(QPointF)
        standalone_macro_open = False
        active_snapshot: _LayerSnapshot | None = None
        active_target_created = False
        native_bridge = self.native_bridge
        active_input_guard: tuple[Any, _CanvasInputGuard] | None = None
        process_events_during_render = True

        if not session_active:
            standalone_macro_open = _start_macro(document, "AI Stroke Paint")

        try:
            if mode == "active_layer":
                current_node = (
                    self._session_active_target
                    if session_active and self._session_active_target is not None
                    else document.activeNode()
                )
                if current_node is None:
                    current_node = self.ensure_layer(document, self.DEFAULT_LAYER_NAME, preserve_existing_order=True)
                    generated_container = current_node
                    active_target_created = True
                if session_active and self._session_active_target is None:
                    self._session_active_target = current_node
                    self._session_active_created = active_target_created
                if session_active:
                    if not self._session_active_created and self._session_active_snapshot is None:
                        self._session_active_snapshot = _capture_layer_snapshot(document, current_node)
                    if self._session_active_expected is not None:
                        self._assert_active_session_rollback_safe(document, current_node)
                elif rollback_on_cancel and not active_target_created:
                    active_snapshot = _capture_layer_snapshot(
                        document,
                        current_node,
                        bounds=_plan_snapshot_bounds(document, plan, size_mult),
                    )
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
                first_layer = render_strokes[0].layer_name
                current_node = layer_cache.get(first_layer)
                if current_node is None:
                    current_node = self.ensure_layer(document, first_layer, parent=output_group)
                    layer_cache[first_layer] = current_node
                current_layer_name = first_layer

            if mode == "active_layer":
                active_input_guard = _install_canvas_input_guard(target_view)
                # Do not pump user events into an unguarded direct target.
                process_events_during_render = active_input_guard is not None

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

            for stroke in render_strokes:
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

                if not hasattr(current_node, "paintLine") and native_bridge is None:
                    raise RuntimeError("このKritaには Node.paintLine がありません。Krita 6.0以降を使用してください。")
                paint_ability = current_node.paintAbility()
                if paint_ability != "PAINT":
                    raise RuntimeError(f"対象レイヤーに描画できません（paintAbility: {paint_ability}）")

                if native_bridge is not None:
                    effective_stroke = Stroke(
                        id=stroke.id,
                        points=stroke.points,
                        brush_preset=stroke.brush_preset,
                        color=stroke.color,
                        size_px=stroke.size_px * size_mult,
                        layer_name=stroke.layer_name,
                        opacity=min(1.0, stroke.opacity * op_mult),
                        is_eraser=stroke.is_eraser,
                    )
                    try:
                        accepted_points = native_bridge.submit_stroke(document, current_node, effective_stroke)
                    except NativeBridgeUnavailable:
                        native_bridge = None
                    else:
                        if accepted_points != len(stroke.points):
                            raise RuntimeError("Native Bridge がストローク全点を受理しませんでした")
                        mutated = True
                        rendered += 1
                        self.last_render_trace["routes"]["native_bridge"] += 1
                        segment_count += max(1, len(stroke.points) - 1)
                        if segment_count >= evt_interval:
                            segment_count = 0
                            if process_events_during_render:
                                _process_events()
                        continue

                if not hasattr(current_node, "paintLine"):
                    raise RuntimeError("このKritaには Node.paintLine がありません。Krita 6.0以降を使用してください。")
                if stroke.is_eraser and target_view is None:
                    raise RuntimeError("消しゴムストロークにはKritaのアクティブビューが必要です")
                _apply_color_to_krita(stroke.color, view=target_view)

                if _can_use_continuous_path(current_node, stroke):
                    painted_sections: int = 0
                    try:
                        for section in _pressure_path_sections(stroke):
                            resolved_preset = _apply_stroke_style(
                                stroke,
                                size_multiplier=size_mult * _points_pressure(section),
                                opacity_multiplier=op_mult,
                                view=target_view,
                                preset_cache=self._brush_preset_cache,
                            )
                            if resolved_preset:
                                self.last_render_trace["resolved_presets"][stroke.brush_preset] = resolved_preset
                            current_node.paintPath(_make_continuous_path(section))
                            painted_sections += 1
                    except (TypeError, AttributeError, NotImplementedError) as exc:
                        # 一部ビルドで paintPath のPython bindingが欠ける場合だけ区間描画へ戻す。
                        if painted_sections:
                            raise RuntimeError("連続ストロークの一部だけが描画されたため安全に中止しました") from exc
                        _apply_stroke_style(
                            stroke,
                            size_multiplier=size_mult,
                            opacity_multiplier=op_mult,
                            view=target_view,
                            preset_cache=self._brush_preset_cache,
                        )
                    else:
                        mutated = True
                        rendered += 1
                        self.last_render_trace["routes"]["continuous_path"] += 1
                        segment_count += max(1, len(stroke.points) - 1)
                        if segment_count >= evt_interval:
                            segment_count = 0
                            if process_events_during_render:
                                _process_events()
                        continue

                resolved_preset = _apply_stroke_style(
                    stroke,
                    size_multiplier=size_mult,
                    opacity_multiplier=op_mult,
                    view=target_view,
                    preset_cache=self._brush_preset_cache,
                )
                if resolved_preset:
                    self.last_render_trace["resolved_presets"][stroke.brush_preset] = resolved_preset

                stroke_painted = False
                for start, end in zip(stroke.points, stroke.points[1:], strict=False):
                    if cancelled():
                        return rendered
                    if math.hypot(end.x - start.x, end.y - start.y) < 0.5:
                        continue
                    painted_with_float = False
                    if use_float_points:
                        try:
                            current_node.paintLine(
                                _qpoint_float(start.x, start.y),
                                _qpoint_float(end.x, end.y),
                                start.pressure,
                                end.pressure,
                            )
                            painted_with_float = True
                        except TypeError:
                            # 一部の Krita Python バインディングは QPoint のみを受け付ける。
                            use_float_points = False
                    if not painted_with_float:
                        current_node.paintLine(
                            _qpoint(start.x, start.y), _qpoint(end.x, end.y), start.pressure, end.pressure
                        )

                    mutated = True
                    stroke_painted = True
                    segment_count += 1
                    if segment_count >= evt_interval:
                        segment_count = 0
                        if process_events_during_render:
                            _process_events()

                # 全区間が0.5px未満でスキップされた微小ストローク（ドット・ハイライト等）の描画補償
                if not stroke_painted and stroke.points:
                    pt0 = stroke.points[0]
                    pt_end_x = pt0.x + 0.5
                    pt_end_y = pt0.y
                    painted_with_float = False
                    if use_float_points:
                        try:
                            current_node.paintLine(
                                _qpoint_float(pt0.x, pt0.y),
                                _qpoint_float(pt_end_x, pt_end_y),
                                pt0.pressure,
                                pt0.pressure,
                            )
                            painted_with_float = True
                        except TypeError:
                            use_float_points = False
                    if not painted_with_float:
                        current_node.paintLine(
                            _qpoint(pt0.x, pt0.y), _qpoint(pt_end_x, pt_end_y), pt0.pressure, pt0.pressure
                        )
                    mutated = True
                    segment_count += 1
                    if segment_count >= evt_interval:
                        segment_count = 0
                        if process_events_during_render:
                            _process_events()

                rendered += 1
                self.last_render_trace["routes"]["segmented_line"] += 1
            completed = not cancelled()
        finally:
            rollback_error: Exception | None = None
            macro_error: Exception | None = None
            if session_active and mutated:
                self._session_has_changes = True
            # キュー内の描画ジョブ完了を安全に待機してからプロジェクションを更新
            if hasattr(document, "waitForDone"):
                with contextlib.suppress(Exception):
                    document.waitForDone()
            if session_active and mode == "active_layer" and mutated and current_node is not None:
                try:
                    self._record_active_session_state(document, current_node)
                except Exception as exc:
                    rollback_error = exc
            if generated_container is not None and rollback_on_cancel and not completed:
                try:
                    self._remove_node(document.rootNode(), generated_container)
                except Exception as exc:
                    rollback_error = exc
                else:
                    if session_active and generated_container is self._session_container:
                        # オプションのUndoマクロを閉じる責務は end_render_session に残す。
                        self._session_container = None
                        self._session_layer_cache = {}
                        if mode == "active_layer":
                            self._session_active_target = None
                            self._session_active_created = False
            elif (
                mode == "active_layer"
                and rollback_on_cancel
                and not completed
                and mutated
                and not session_active
                and active_snapshot is not None
            ):
                try:
                    _restore_layer_snapshot(active_snapshot)
                except Exception as exc:
                    rollback_error = exc
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
            if standalone_macro_open:
                try:
                    _end_macro(document)
                except Exception as exc:
                    macro_error = exc
            _remove_canvas_input_guard(active_input_guard)
            _restore_view_state(target_view, view_state)
            if rollback_error is not None:
                raise rollback_error
            if macro_error is not None:
                raise macro_error

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
        errors: list[Exception] = []
        remove_child = getattr(parent, "removeChildNode", None)
        if callable(remove_child):
            try:
                result = remove_child(node)
                if result is False:
                    raise RuntimeError("removeChildNode returned false")
                return
            except Exception as exc:
                errors.append(exc)
        remove = getattr(node, "remove", None)
        if callable(remove):
            try:
                result = remove()
                if result is False:
                    raise RuntimeError("Node.remove returned false")
                return
            except Exception as exc:
                errors.append(exc)
        cause = errors[-1] if errors else None
        raise RuntimeError("Kritaレイヤー階層からノードを除去できませんでした") from cause


def _start_macro(document: Any, title: str = "AI Stroke Paint") -> bool:
    """Krita のアンドゥマクロを開始する。"""
    if hasattr(document, "createMacro"):
        with contextlib.suppress(Exception):
            document.createMacro(title)
            return True
    return False


def _end_macro(document: Any) -> None:
    """Krita のアンドゥマクロを終了・確定する。"""
    end_macro = getattr(document, "endMacro", None)
    if not callable(end_macro):
        raise RuntimeError("Krita Undoマクロを終了できません")
    try:
        end_macro()
    except Exception as exc:
        raise RuntimeError("Krita Undoマクロの終了に失敗しました") from exc


def _estimated_pixel_bytes(node: Any, width: int, height: int) -> int:
    depth = str(getattr(node, "colorDepth", lambda: "U8")()).upper()
    model = str(getattr(node, "colorModel", lambda: "RGBA")()).upper()
    bytes_per_channel = {"U8": 1, "U16": 2, "F16": 2, "F32": 4}.get(depth, 4)
    channels = 5 if "CMYK" in model else (2 if model in {"A", "ALPHA", "GRAYA"} else 4)
    return width * height * bytes_per_channel * channels


def _plan_snapshot_bounds(document: Any, plan: DrawingPlan, size_multiplier: float) -> tuple[int, int, int, int]:
    width = int(document.width())
    height = int(document.height())
    if not plan.strokes:
        return 0, 0, max(1, width), max(1, height)
    max_radius = max(stroke.size_px * size_multiplier * 1.5 + 4.0 for stroke in plan.strokes)
    min_x = max(0, math.floor(min(point.x for stroke in plan.strokes for point in stroke.points) - max_radius))
    min_y = max(0, math.floor(min(point.y for stroke in plan.strokes for point in stroke.points) - max_radius))
    max_x = min(width, math.ceil(max(point.x for stroke in plan.strokes for point in stroke.points) + max_radius + 1))
    max_y = min(height, math.ceil(max(point.y for stroke in plan.strokes for point in stroke.points) + max_radius + 1))
    return min_x, min_y, max(1, max_x - min_x), max(1, max_y - min_y)


def _capture_layer_snapshot(
    document: Any,
    node: Any,
    bounds: tuple[int, int, int, int] | None = None,
) -> _LayerSnapshot:
    """標準Krita Node APIだけで、既存レイヤーの復元可能なコピーを取得する。"""
    pixel_data = getattr(node, "pixelData", None)
    set_pixel_data = getattr(node, "setPixelData", None)
    if not callable(pixel_data) or not callable(set_pixel_data):
        raise RuntimeError("対象レイヤーが画素スナップショットAPIに対応していないため安全に描画できません")
    try:
        if bounds is None:
            x, y, width, height = 0, 0, int(document.width()), int(document.height())
        else:
            x, y, width, height = bounds
        if width <= 0 or height <= 0:
            raise ValueError("invalid document dimensions")
        estimated = _estimated_pixel_bytes(node, width, height)
        if estimated > MAX_ACTIVE_LAYER_SNAPSHOT_BYTES:
            raise RuntimeError(
                f"アクティブレイヤーのスナップショットが上限 ({MAX_ACTIVE_LAYER_SNAPSHOT_BYTES // (1024 * 1024)}MB) を超えるため安全に描画できません"
            )
        pixels = pixel_data(x, y, width, height)
    except RuntimeError:
        raise
    except Exception as exc:
        raise RuntimeError("アクティブレイヤーのロールバック用スナップショットを取得できません") from exc
    if pixels is None:
        raise RuntimeError("アクティブレイヤーのロールバック用スナップショットが空です")
    with contextlib.suppress(TypeError, ValueError):
        pixel_len = len(pixels) if isinstance(pixels, Sized) else getattr(pixels, "size", lambda: 0)()
        if pixel_len > MAX_ACTIVE_LAYER_SNAPSHOT_BYTES:
            raise RuntimeError("アクティブレイヤーのスナップショットが安全なメモリ上限を超えています")
    return _LayerSnapshot(node=node, pixels=pixels, x=x, y=y, width=width, height=height)


def _fingerprint_layer(document: Any, node: Any) -> _LayerFingerprint:
    """Identify a layer's full pixel state without retaining another full snapshot."""
    snapshot = _capture_layer_snapshot(document, node)
    try:
        digest = hashlib.blake2b(bytes(snapshot.pixels), digest_size=32).digest()
    except (TypeError, ValueError) as exc:
        raise RuntimeError("アクティブレイヤーの画素状態を検証できません") from exc
    return _LayerFingerprint(
        node=snapshot.node,
        width=snapshot.width,
        height=snapshot.height,
        digest=digest,
    )


def _fingerprints_match(expected: _LayerFingerprint, actual: _LayerFingerprint) -> bool:
    node_match = expected.node is actual.node
    if not node_match:
        with contextlib.suppress(Exception):
            node_match = bool(expected.node == actual.node)
    return (
        node_match
        and expected.width == actual.width
        and expected.height == actual.height
        and expected.digest == actual.digest
    )


def _restore_layer_snapshot(snapshot: _LayerSnapshot) -> None:
    """取得済みスナップショットを同じレイヤーだけに復元する。"""
    set_pixel_data = getattr(snapshot.node, "setPixelData", None)
    if not callable(set_pixel_data):
        raise RuntimeError("対象レイヤーが画素復元APIに対応していません")
    try:
        result = set_pixel_data(snapshot.pixels, snapshot.x, snapshot.y, snapshot.width, snapshot.height)
    except Exception as exc:
        raise RuntimeError("アクティブレイヤーの画素復元に失敗しました") from exc
    if result is False:
        raise RuntimeError("Kritaがアクティブレイヤーの画素復元を拒否しました")


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


def _constant_path_pressure(stroke: Stroke) -> float:
    return _points_pressure(stroke.points)


def _points_pressure(points: Sequence[StrokePoint]) -> float:
    return max(0.05, sum(point.pressure for point in points) / max(1, len(points)))


def _pressure_path_sections(
    stroke: Stroke,
    *,
    pressure_delta: float = 0.18,
    max_sections: int = 6,
) -> tuple[tuple[StrokePoint, ...], ...]:
    """筆圧を少数の連続パスへ量子化し、点ごとの丸い継ぎ目を防ぐ。"""
    points = tuple(stroke.points)
    if len(points) < 3:
        return (points,)
    sections: list[tuple[StrokePoint, ...]] = []
    current: list[StrokePoint] = [points[0]]
    pressure_sum = points[0].pressure
    for point in points[1:]:
        mean_pressure = pressure_sum / len(current)
        can_split = len(current) >= 2 and len(sections) < max_sections - 1
        if can_split and abs(point.pressure - mean_pressure) > pressure_delta:
            current.append(point)
            sections.append(tuple(current))
            current = [point]
            pressure_sum = point.pressure
        else:
            current.append(point)
            pressure_sum += point.pressure
    if len(current) == 1 and sections:
        sections[-1] = (*sections[-1], current[0])
    else:
        sections.append(tuple(current))
    return tuple(section for section in sections if len(section) >= 2)


def _can_use_continuous_path(node: Any, stroke: Stroke) -> bool:
    if stroke.is_eraser or len(stroke.points) < 3 or not callable(QPainterPath):
        return False
    return callable(getattr(node, "paintPath", None))


def _make_continuous_path(points: Sequence[StrokePoint]) -> Any:
    path = QPainterPath()
    first = points[0]
    path.moveTo(float(first.x), float(first.y))
    for point in points[1:]:
        path.lineTo(float(point.x), float(point.y))
    return path


# 後方互換エイリアス
_qpointf = _qpoint


def _process_events() -> None:
    if QApplication is not None and hasattr(QApplication, "processEvents"):
        QApplication.processEvents()


def _parse_hex_rgb(hex_str: str) -> tuple[float, float, float] | None:
    h = hex_str.lstrip("#")
    try:
        if len(h) in (3, 4):
            return int(h[0] * 2, 16) / 255.0, int(h[1] * 2, 16) / 255.0, int(h[2] * 2, 16) / 255.0
        if len(h) in (6, 8):
            return int(h[0:2], 16) / 255.0, int(h[2:4], 16) / 255.0, int(h[4:6], 16) / 255.0
    except ValueError:
        return None
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
    preset_cache: dict[tuple[str, bool], Any] | None = None,
) -> str | None:
    """Apply the DrawingPlan brush contract to Krita's active view."""
    try:
        from krita import Krita

        app = Krita.instance()
        target_view = view or _active_view()
        if target_view is None:
            return None

        is_eraser = bool(getattr(stroke, "is_eraser", False))
        preset_name = str(stroke.brush_preset).strip()
        cache_key = (preset_name.lower(), is_eraser)
        cached_preset = preset_cache.get(cache_key) if preset_cache is not None else None
        presets: Any = getattr(app, "resources", lambda _kind: {})("preset")
        preset: Any = cached_preset

        if is_eraser and preset is None:
            # 消しゴム用プリセットの優先探索
            eraser_candidates = list(brush_definition("eraser").candidates)
            if hasattr(presets, "values"):
                all_presets = list(presets.values())
                for cand in eraser_candidates:
                    match = next(
                        (p for p in all_presets if getattr(p, "name", lambda: "")().lower() == cand.lower()),
                        None,
                    )
                    if match is not None:
                        preset = match
                        break
                if preset is None:
                    # 名前に "eraser" を含むプリセットを部分一致で検索
                    preset = next(
                        (p for p in all_presets if "eraser" in getattr(p, "name", lambda: "")().lower()),
                        None,
                    )

        if is_eraser and preset is None:
            raise RuntimeError(
                "利用可能な消しゴムプリセットを解決できませんでした。通常ブラシで上書きする危険があるため描画を中止します。"
            )

        if preset is None and hasattr(presets, "get"):
            preset = presets.get(preset_name)
            if preset is not None:
                resolved_name_getter = getattr(preset, "name", None)
                resolved_name = (
                    str(resolved_name_getter()) if callable(resolved_name_getter) else str(resolved_name_getter or "")
                )
                if "(mypaint)" in resolved_name.casefold() and "(mypaint)" not in preset_name.casefold():
                    preset = None

        if preset is None and hasattr(presets, "values"):
            all_presets = list(presets.values())

            def _clean_p_name(raw_name: str) -> str:
                return re.sub(r"^[a-zA-Z]\)\s*", "", raw_name.strip()).lower()

            p_low = preset_name.lower()
            p_clean = _clean_p_name(preset_name)

            # 1. プレフィックス除去を含む完全一致
            for p in all_presets:
                p_name = getattr(p, "name", None)
                p_name_str: str = str(p_name()) if callable(p_name) else (str(p_name) if p_name is not None else "")
                p_cand_clean = _clean_p_name(p_name_str)
                if p_name_str.lower() == p_low or p_cand_clean == p_clean:
                    if "(mypaint)" in p_name_str.casefold() and "(mypaint)" not in preset_name.casefold():
                        continue
                    preset = p
                    break

            # 2. 意味プロファイルの candidates（候補リスト）による優先探索
            if preset is None:
                profile_key = infer_brush_profile(preset_name)
                cand_list = list(brush_definition(profile_key).candidates)
                for cand in cand_list:
                    c_clean = _clean_p_name(cand)
                    for p in all_presets:
                        p_name = getattr(p, "name", None)
                        p_name_str = str(p_name()) if callable(p_name) else (str(p_name) if p_name is not None else "")
                        p_cand_clean = _clean_p_name(p_name_str)
                        if p_cand_clean == c_clean or c_clean in p_cand_clean:
                            # 通常ブラシの場合、非互換な mypaint ブラシを優先候補から除外
                            if "(mypaint)" in p_name_str.lower() and "(mypaint)" not in cand.lower():
                                continue
                            preset = p
                            break
                    if preset is not None:
                        break

            # 3. 意味プロファイルに基づくカテゴリ柔軟マッチング (mypaint を除外)
            if preset is None:
                profile_key = infer_brush_profile(preset_name)
                cat_keywords = list(brush_definition(profile_key).keywords)

                for kw in cat_keywords:
                    for p in all_presets:
                        p_name = getattr(p, "name", None)
                        p_name_str = str(p_name()) if callable(p_name) else (str(p_name) if p_name is not None else "")
                        if "(mypaint)" in p_name_str.lower() and "(mypaint)" not in preset_name.lower():
                            continue
                        if kw in p_name_str.lower():
                            preset = p
                            break
                    if preset is not None:
                        break

            # 4. 汎用フォールバック (Basic系ピクセルブラシ)
            if preset is None:
                for p in all_presets:
                    p_name = getattr(p, "name", None)
                    p_name_str = str(p_name()) if callable(p_name) else (str(p_name) if p_name is not None else "")
                    if "basic" in p_name_str.lower() and "(mypaint)" not in p_name_str.lower():
                        preset = p
                        break

        if preset is None:
            raise RuntimeError(f"描画ブラシプリセットを解決できませんでした: {preset_name}")
        if not hasattr(target_view, "setCurrentBrushPreset"):
            raise RuntimeError("Kritaビューがブラシプリセット切替に対応していません")
        target_view.setCurrentBrushPreset(preset)
        if preset_cache is not None:
            preset_cache[cache_key] = preset

        effective_size = max(0.5, float(stroke.size_px) * size_multiplier)
        _rgb_color, color_alpha = split_color_alpha(stroke.color)
        effective_opacity = max(0.0, min(1.0, float(stroke.opacity) * opacity_multiplier * color_alpha))

        if hasattr(target_view, "setBrushSize"):
            target_view.setBrushSize(effective_size)
        if hasattr(target_view, "setPaintingOpacity"):
            target_view.setPaintingOpacity(effective_opacity)
        preset_name_getter = getattr(preset, "name", None)
        return str(preset_name_getter()) if callable(preset_name_getter) else str(preset_name)
    except Exception:
        if view is not None or bool(getattr(stroke, "is_eraser", False)):
            raise
    return None
