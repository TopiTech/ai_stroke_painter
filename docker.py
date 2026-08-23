"""AI Stroke Painter Pro Krita Docker UI および非同期制御ワーカー。"""

from __future__ import annotations

import contextlib
import datetime
import os
from pathlib import Path
import threading
import traceback
from typing import TYPE_CHECKING, Any, cast

from .domain import DrawingPlan
from .krita_adapter import KritaCanvasAdapter
from .llm_planner import OpenAICompatiblePlanner, OpenAICompatibleSettings
from .planner import RuleBasedPlanner
from .ports import PlannerPort
from .qt_compat import (
    QApplication,
    QCheckBox,
    QColor,
    QComboBox,
    QFileDialog,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QObject,
    QPainter,
    QPen,
    QPlainTextEdit,
    QProgressBar,
    QPushButton,
    QSettings,
    QSpinBox,
    QVBoxLayout,
    QWidget,
    pyqtSignal,
)
from .storage import save_plan, save_svg

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


class PreviewWidget(QWidget):
    """描画計画のストロークをリアルタイムでベクタープレビューするミニキャンバス。"""

    def __init__(self, parent: Any | None = None) -> None:
        super().__init__(parent)
        self._plan: DrawingPlan | None = None
        self.setMinimumHeight(160)
        self.setMaximumHeight(200)

    def set_plan(self, plan: DrawingPlan | None) -> None:
        self._plan = plan
        self.update()

    def paintEvent(self, event: Any) -> None:  # noqa: N802
        if QPainter is None or QColor is None or QPen is None or not callable(QPainter):
            return

        painter: Any = QPainter(self)
        try:
            w = float(self.width()) if hasattr(self, "width") else 200.0
            h = float(self.height()) if hasattr(self, "height") else 160.0

            painter.fillRect(0, 0, int(w), int(h), QColor("#1e1e24"))

            if self._plan is None or not self._plan.strokes:
                painter.setPen(QColor("#777788"))
                painter.drawText(int(w * 0.2), int(h * 0.5), "ストローク プレビュー")
                return

            max_x = max((p.x for s in self._plan.strokes for p in s.points), default=w)
            max_y = max((p.y for s in self._plan.strokes for p in s.points), default=h)
            scale = min(w / max(1.0, max_x), h / max(1.0, max_y)) * 0.92
            ox = (w - max_x * scale) * 0.5
            oy = (h - max_y * scale) * 0.5

            for stroke in self._plan.strokes:
                col = QColor(stroke.color)
                if stroke.opacity < 1.0 and hasattr(col, "setAlphaF"):
                    col.setAlphaF(stroke.opacity)
                pen = QPen(col, max(1.0, stroke.size_px * scale * 0.6))
                painter.setPen(pen)
                pts = stroke.points
                for p0, p1 in zip(pts, pts[1:]):
                    painter.drawLine(
                        int(ox + p0.x * scale),
                        int(oy + p0.y * scale),
                        int(ox + p1.x * scale),
                        int(oy + p1.y * scale),
                    )
        finally:
            painter.end()


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
        count: int,
        width: float,
        height: float,
        image_data: bytes | None = None,
        max_iterations: int = 1,
        palette_name: str = "anime",
        parent: Any | None = None,
        canvas_port: Any | None = None,  # 下位互換用（ワーカー内では不使用）
        document: Any | None = None,  # 下位互換用（ワーカー内では不使用）
    ) -> None:
        super().__init__(parent)
        self.planner = planner
        self.prompt = prompt
        self.seed = seed
        self.count = count
        self.width = width
        self.height = height
        self.image_data = image_data
        self.max_iterations = max_iterations
        self.palette_name = palette_name
        self._is_cancelled = False
        self._render_done_event = threading.Event()
        self._next_canvas_image: bytes | None = None
        self._thread: threading.Thread | None = None
        self._is_running = False

        # プランナーがログコールバックをサポートしている場合はワーカーシグナルに接続
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

    def cancel(self) -> None:
        self._is_cancelled = True
        self._render_done_event.set()
        self.debug_log.emit("[ワーカー] キャンセル要求を受信しました")

    def is_cancelled(self) -> bool:
        return self._is_cancelled

    def isRunning(self) -> bool:  # noqa: N802
        return self._is_running or (self._thread is not None and self._thread.is_alive())

    def start(self) -> None:
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
            self.debug_log.emit(
                f"[ワーカー開始] Total Iterations: {self.max_iterations}, Target Size: {self.width:.0f}x{self.height:.0f}"
            )

            for iter_idx in range(1, self.max_iterations + 1):
                if self.is_cancelled():
                    self.debug_log.emit("[ワーカー] 処理が中断されました")
                    return

                msg = f"イテレーション {iter_idx}/{self.max_iterations}: 計画を生成中..."
                self.iteration_progress.emit(iter_idx, self.max_iterations, msg)
                self.debug_log.emit(f"[イテレーション {iter_idx}/{self.max_iterations}] 計画生成処理を開始")

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
                )

                if self.is_cancelled():
                    self.debug_log.emit("[ワーカー] 描画計画受領後にキャンセルを確認しました")
                    return

                self.debug_log.emit(
                    f"[イテレーション {iter_idx}] 計画生成完了。メインスレッドへ描画を要求します (ストローク数: {len(current_plan.strokes)})"
                )

                self._render_done_event.clear()
                self._next_canvas_image = None
                self.plan_ready.emit(current_plan)

                # メインスレッドでの描画 & キャプチャ完了を待機
                while not self._render_done_event.wait(timeout=0.05):
                    if self.is_cancelled():
                        self.debug_log.emit("[ワーカー] 描画待機中にキャンセルを確認しました")
                        return

            if not self.is_cancelled():
                self.iteration_progress.emit(self.max_iterations, self.max_iterations, "全イテレーションが完了しました")
                self.debug_log.emit("[ワーカー完了] 全ての処理が正常に完了しました")

        except Exception as exc:
            tb = traceback.format_exc()
            self.debug_log.emit(f"[例外発生] {exc}\nスタックトレース:\n{tb}")
            if not self._is_cancelled:
                self.plan_failed.emit(str(exc))


class AIStrokePainterDocker(DockWidget):
    """AI Stroke Painter 製品版 Krita Docker パネル。"""

    PRESETS = [
        ("👤 美少女アニメ顔", "anime girl portrait, delicate eyes, flowing hair", "anime", 40),
        ("👤 少年ヒーロー", "anime boy hero with spiky hair and confident smile", "anime", 35),
        ("🌿 幻想的な山と桜", "fantasy sakura landscape with mountains and clouds", "nature", 30),
        ("🌊 浮世絵風の大波", "hokusai great wave with foam and ripples", "nature", 30),
        ("💥 迫力の集中線", "intense manga focus radial speed lines", "monochrome", 45),
        ("🧙 魔法陣エフェクト", "intense magical circle with runes and radiant rays", "cyberpunk", 35),
        ("🐱 優雅な猫", "cute cat face with whiskers and emerald eyes", "nature", 30),
        ("🏛️ サイバーパンク都市", "cyberpunk city skyline with neon buildings", "cyberpunk", 40),
        ("🌀 神聖幾何学マンダラ", "sacred geometry kaleidoscope mandala", "cyberpunk", 40),
        ("🌹 バラの花束", "blooming rose with stem and organic leaves", "nature", 30),
    ]

    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("AI Stroke Painter Pro")
        self.planner = RuleBasedPlanner()
        self.canvas_port = KritaCanvasAdapter()
        self._cancel: bool = False
        self._worker: PlanWorker | None = None
        self._active_doc: Any | None = None
        self._canvas: Any | None = None
        self._image_bytes: bytes | None = None
        self._last_plan: DrawingPlan | None = None

        container = QWidget(self)
        root_layout = QVBoxLayout(container)

        # 1. プリセットクイック選択
        preset_box = QGroupBox("クイック・プリセット")
        preset_layout = QHBoxLayout(preset_box)
        self.preset_combo = QComboBox()
        for title, prompt_text, palette, count in self.PRESETS:
            self.preset_combo.addItem(title, (prompt_text, palette, count))
        preset_apply_btn = QPushButton("適用")
        preset_apply_btn.clicked.connect(self._apply_preset)
        preset_layout.addWidget(self.preset_combo)
        preset_layout.addWidget(preset_apply_btn)
        root_layout.addWidget(preset_box)

        # 2. プロンプト入力欄
        root_layout.addWidget(QLabel("描画指示 (Prompt)"))
        self.prompt = QPlainTextEdit("anime girl portrait, delicate eyes, flowing hair")
        self.prompt.setMaximumHeight(70)
        root_layout.addWidget(self.prompt)

        # 3. 参照画像 (Image-to-Stroke) パネル
        image_box = QGroupBox("参照画像 (Image-to-Stroke)")
        image_layout = QHBoxLayout(image_box)
        self.load_image_btn = QPushButton("画像を選択...")
        self.clear_image_btn = QPushButton("クリア")
        self.clear_image_btn.setEnabled(False)
        self.image_status_label = QLabel("画像なし")
        image_layout.addWidget(self.load_image_btn)
        image_layout.addWidget(self.clear_image_btn)
        image_layout.addWidget(self.image_status_label)
        self.load_image_btn.clicked.connect(self._select_reference_image)
        self.clear_image_btn.clicked.connect(self._clear_reference_image)
        root_layout.addWidget(image_box)

        # 4. パラメータ設定 (Seed, 本数, 自律改善ループ)
        params_layout = QHBoxLayout()
        params_layout.addWidget(QLabel("Seed"))
        self.seed = QSpinBox()
        self.seed.setRange(0, 2147483647)
        self.seed.setValue(42)
        params_layout.addWidget(self.seed)

        params_layout.addWidget(QLabel("本数"))
        self.count = QSpinBox()
        self.count.setRange(1, 200)
        self.count.setValue(35)
        params_layout.addWidget(self.count)
        root_layout.addLayout(params_layout)

        # 自律改善反復設定
        refine_layout = QHBoxLayout()
        self.auto_refine = QCheckBox("自律反復改善 (Auto-Refine)")
        self.auto_refine.setChecked(False)
        refine_layout.addWidget(self.auto_refine)
        refine_layout.addWidget(QLabel("反復回数"))
        self.iterations = QSpinBox()
        self.iterations.setRange(1, 10)
        self.iterations.setValue(3)
        refine_layout.addWidget(self.iterations)
        root_layout.addLayout(refine_layout)

        # 5. カラーパレット & スタイル
        style_layout = QHBoxLayout()
        style_layout.addWidget(QLabel("パレット"))
        self.palette_combo = QComboBox()
        self.palette_combo.addItem("アニメカラー", "anime")
        self.palette_combo.addItem("モノクロ線画", "monochrome")
        self.palette_combo.addItem("サイバーパンク", "cyberpunk")
        self.palette_combo.addItem("自然アースカラー", "nature")
        style_layout.addWidget(self.palette_combo)
        root_layout.addLayout(style_layout)

        # 6. Planner 選択 & LLM 設定
        root_layout.addWidget(QLabel("Planner エンジン"))
        self.planner_mode = QComboBox()
        self.planner_mode.addItem("プロシージャル (オフライン 高品質)", "offline")
        self.planner_mode.addItem("OpenAI 互換 LLM / Vision", "openai_compatible")
        root_layout.addWidget(self.planner_mode)

        self.llm_settings = QGroupBox("OpenAI 互換 API 設定")
        llm_form = QFormLayout(self.llm_settings)
        self.base_url = QLineEdit("https://api.openai.com/v1")
        llm_form.addRow("Base URL", self.base_url)
        self.model = QLineEdit("gpt-4o")
        self.model.setPlaceholderText("例: gpt-4o, o3-mini, deepseek-r1, qwq-32b")
        llm_form.addRow("Model", self.model)
        self.api_key = QLineEdit()
        with contextlib.suppress(AttributeError):
            self.api_key.setEchoMode(QLineEdit.Password)
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

        test_conn_btn = QPushButton("API 接続テスト")
        test_conn_btn.clicked.connect(self._test_api_connection)
        llm_form.addRow("", test_conn_btn)
        root_layout.addWidget(self.llm_settings)

        # 7. ベクタープレビュー
        self.preview = PreviewWidget(self)
        root_layout.addWidget(self.preview)

        # 8. 保存オプション & デバッグモード切替
        save_layout = QHBoxLayout()
        self.save_json = QCheckBox("計画 JSON 保存")
        self.save_json.setChecked(True)
        self.save_svg_chk = QCheckBox("SVG 保存")
        self.save_svg_chk.setChecked(True)
        save_layout.addWidget(self.save_json)
        save_layout.addWidget(self.save_svg_chk)
        root_layout.addLayout(save_layout)

        debug_toggle_layout = QHBoxLayout()
        self.debug_mode_chk = QCheckBox("🐞 デバッグモード (詳細ログを表示)")
        self.debug_mode_chk.setChecked(False)
        debug_toggle_layout.addWidget(self.debug_mode_chk)
        root_layout.addLayout(debug_toggle_layout)

        # 9. デバッグログパネル
        self.debug_box = QGroupBox("デバッグログ (リアルタイム通信・処理ログ)")
        debug_box_layout = QVBoxLayout(self.debug_box)
        self.debug_log_edit = QPlainTextEdit()
        self.debug_log_edit.setReadOnly(True)
        self.debug_log_edit.setMaximumHeight(140)
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
        root_layout.addWidget(self.debug_box)

        # 10. 描画・停止ボタン
        self.run_btn = QPushButton("🎨 AIストロークを描画")
        self.stop_btn = QPushButton("⏹ 停止")
        self.stop_btn.setEnabled(False)
        btn_layout = QHBoxLayout()
        btn_layout.addWidget(self.run_btn)
        btn_layout.addWidget(self.stop_btn)
        root_layout.addLayout(btn_layout)

        # 11. プログレスバー & ステータス
        self.progress = QProgressBar()
        self.progress.setRange(0, 1)
        self.progress.setValue(0)
        root_layout.addWidget(self.progress)
        self.status = QLabel("待機中: プロンプトまたはプリセットを選んで描画を開始してください")
        self.status.setWordWrap(True)
        root_layout.addWidget(self.status)

        root_layout.addStretch(1)
        self.setWidget(container)

        # 設定の復元
        self._load_settings()

        # イベント接続
        self.run_btn.clicked.connect(self.run)
        self.stop_btn.clicked.connect(self.cancel)
        self.planner_mode.currentIndexChanged.connect(self._update_planner_settings_state)
        self._update_planner_settings_state()

    def canvasChanged(self, canvas: Any) -> None:  # noqa: N802
        """Kritaからキャンバス切り替えイベント通知を受け取る (DockWidgetの必須抽象メソッド)。"""
        self._canvas = canvas

    def _load_settings(self) -> None:
        """QSettings から前回の UI 設定値を自動復元する。"""
        if QSettings is None or not callable(QSettings):
            return
        with contextlib.suppress(Exception):
            settings: Any = QSettings("AIStrokePainter", "DockerSettings")
            if settings.value("base_url"):
                self.base_url.setText(str(settings.value("base_url")))
            if settings.value("model"):
                self.model.setText(str(settings.value("model")))
            if settings.value("timeout_sec"):
                self.timeout_sec.setValue(int(settings.value("timeout_sec")))
            if settings.value("max_tokens"):
                self.max_tokens.setValue(int(settings.value("max_tokens")))
            if settings.value("reasoning_effort"):
                effort_val = str(settings.value("reasoning_effort"))
                for i in range(self.reasoning_effort.count()):
                    if self.reasoning_effort.itemData(i) == effort_val:
                        self.reasoning_effort.setCurrentIndex(i)
                        break
            if settings.value("prompt"):
                self.prompt.setPlainText(str(settings.value("prompt")))
            if settings.value("seed") is not None:
                self.seed.setValue(int(settings.value("seed")))
            if settings.value("count") is not None:
                self.count.setValue(int(settings.value("count")))
            if settings.value("iterations") is not None:
                self.iterations.setValue(int(settings.value("iterations")))
            if settings.value("auto_refine") is not None:
                self.auto_refine.setChecked(str(settings.value("auto_refine")).lower() in ("true", "1"))
            if settings.value("save_json") is not None:
                self.save_json.setChecked(str(settings.value("save_json")).lower() in ("true", "1"))
            if settings.value("save_svg") is not None:
                self.save_svg_chk.setChecked(str(settings.value("save_svg")).lower() in ("true", "1"))
            if settings.value("debug_mode") is not None:
                self.debug_mode_chk.setChecked(str(settings.value("debug_mode")).lower() in ("true", "1"))

    def _save_settings(self) -> None:
        """現在の UI 設定値を QSettings に保存する。"""
        if QSettings is None or not callable(QSettings):
            return
        with contextlib.suppress(Exception):
            settings: Any = QSettings("AIStrokePainter", "DockerSettings")
            settings.setValue("base_url", self.base_url.text())
            settings.setValue("model", self.model.text())
            settings.setValue("timeout_sec", self.timeout_sec.value())
            settings.setValue("max_tokens", self.max_tokens.value())
            settings.setValue("reasoning_effort", self.reasoning_effort.currentData() or "low")
            settings.setValue("prompt", self.prompt.toPlainText())
            settings.setValue("seed", self.seed.value())
            settings.setValue("count", self.count.value())
            settings.setValue("iterations", self.iterations.value())
            settings.setValue("auto_refine", self.auto_refine.isChecked())
            settings.setValue("save_json", self.save_json.isChecked())
            settings.setValue("save_svg", self.save_svg_chk.isChecked())
            settings.setValue("debug_mode", self.debug_mode_chk.isChecked())
            if hasattr(settings, "sync"):
                settings.sync()

    def _toggle_debug_panel(self, checked: bool) -> None:
        self.debug_box.setVisible(checked)
        if checked and not self.debug_log_edit.toPlainText():
            self._log_debug("デバッグモードが有効化されました。")

    def _log_debug(self, message: str) -> None:
        self.debug_log_edit.appendPlainText(message)

    def _copy_debug_log(self) -> None:
        text = self.debug_log_edit.toPlainText()
        if text:
            clipboard = QApplication.clipboard()
            if clipboard is not None:
                clipboard.setText(text)
                self.status.setText("デバッグログをクリップボードにコピーしました")

    def _clear_debug_log(self) -> None:
        self.debug_log_edit.clear()

    def _save_debug_log(self) -> None:
        text = self.debug_log_edit.toPlainText()
        if not text:
            QMessageBox.information(self, "ログ保存", "保存するログがありません。")
            return
        file_path, _ = QFileDialog.getSaveFileName(
            self, "デバッグログを保存", "ai_stroke_painter_debug.log", "テキストログ (*.log *.txt)"
        )
        if file_path:
            try:
                Path(file_path).write_text(text, encoding="utf-8")
                QMessageBox.information(self, "ログ保存", f"ログを保存しました:\n{file_path}")
            except Exception as exc:
                QMessageBox.critical(self, "エラー", f"ログの保存に失敗しました: {exc}")

    def _apply_preset(self) -> None:
        data = self.preset_combo.currentData()
        if data:
            prompt_text, palette, count = data
            self.prompt.setPlainText(prompt_text)
            self.count.setValue(count)
            for i in range(self.palette_combo.count()):
                if self.palette_combo.itemData(i) == palette:
                    self.palette_combo.setCurrentIndex(i)
                    break
            self._log_debug(f"[プリセット適用] {self.preset_combo.currentText()}")

    def _select_reference_image(self) -> None:
        file_path, _ = QFileDialog.getOpenFileName(
            self, "参照画像を開く", "", "画像ファイル (*.png *.jpg *.jpeg *.webp *.bmp)"
        )
        if file_path and Path(file_path).is_file():
            try:
                self._image_bytes = Path(file_path).read_bytes()
                self.image_status_label.setText(Path(file_path).name)
                self.clear_image_btn.setEnabled(True)
                self._log_debug(f"[参照画像読込] {file_path} ({len(self._image_bytes)} bytes)")
            except Exception as exc:
                QMessageBox.critical(self, "エラー", f"画像を読み込めませんでした: {exc}")

    def _clear_reference_image(self) -> None:
        self._image_bytes = None
        self.image_status_label.setText("画像なし")
        self.clear_image_btn.setEnabled(False)
        self._log_debug("[参照画像クリア]")

    def _test_api_connection(self) -> None:
        self._log_debug("[API 接続テスト開始]")
        try:
            planner = OpenAICompatiblePlanner(
                OpenAICompatibleSettings(
                    base_url=self.base_url.text(),
                    model=self.model.text(),
                    api_key=self.api_key.text().strip() or os.environ.get("OPENAI_API_KEY", ""),
                    timeout_seconds=min(15.0, float(self.timeout_sec.value())),
                    max_tokens=self.max_tokens.value(),
                    reasoning_effort=self.reasoning_effort.currentData() or "low",
                ),
                log_callback=self._log_debug,
            )
            msg = planner.test_connection()
            QMessageBox.information(self, "API 接続テスト", msg)
        except Exception as exc:
            self._log_debug(f"[API 接続テスト失敗] {exc}")
            QMessageBox.critical(self, "接続テスト失敗", str(exc))

    def _update_planner_settings_state(self, *_args: Any) -> None:
        mode_data = self.planner_mode.currentData()
        mode_text = self.planner_mode.currentText()
        idx = self.planner_mode.currentIndex()
        is_openai = mode_data == "openai_compatible" or "OpenAI" in mode_text or idx == 1
        self.llm_settings.setEnabled(is_openai)

    def _planner(self) -> PlannerPort:
        mode_data = self.planner_mode.currentData()
        mode_text = self.planner_mode.currentText()
        idx = self.planner_mode.currentIndex()
        is_openai = mode_data == "openai_compatible" or "OpenAI" in mode_text or idx == 1

        if not is_openai:
            self._log_debug("[エンジン選択] プロシージャル (オフライン)")
            return self.planner

        effort_val = self.reasoning_effort.currentData() or "low"
        self._log_debug(
            f"[エンジン選択] OpenAI 互換 API (Base URL: {self.base_url.text()}, Model: {self.model.text()}, MaxTokens: {self.max_tokens.value()}, ReasoningEffort: {effort_val})"
        )
        return OpenAICompatiblePlanner(
            OpenAICompatibleSettings(
                base_url=self.base_url.text(),
                model=self.model.text(),
                api_key=self.api_key.text().strip() or os.environ.get("OPENAI_API_KEY", ""),
                timeout_seconds=float(self.timeout_sec.value()),
                max_tokens=self.max_tokens.value(),
                reasoning_effort=effort_val,
            ),
            log_callback=self._log_debug,
        )

    def is_cancelled(self) -> bool:
        return self._cancel

    def cancel(self) -> None:
        self._cancel = True
        if self._worker is not None and hasattr(self._worker, "cancel"):
            self._worker.cancel()
        self.status.setText("停止要求を受け付けました。現在の処理完了後に停止します。")
        self._log_debug("[UI] 停止ボタンが押下されました")

    def run(self) -> None:
        document: Any | None = Krita.instance().activeDocument()
        if document is None:
            QMessageBox.warning(
                self, "AI Stroke Painter", "先にドキュメントを開いてください。描画先キャンバスがありません。"
            )
            return

        self._save_settings()
        self._active_doc = document
        self._cancel = False
        self.run_btn.setEnabled(False)
        self.stop_btn.setEnabled(True)
        self.progress.setRange(0, 0)

        max_iters = self.iterations.value() if self.auto_refine.isChecked() else 1
        palette = self.palette_combo.currentData() or "anime"

        now_str = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        self._log_debug(f"\n========== 描画タスク開始 [{now_str}] ==========")
        self._log_debug(f"選択モード: {self.planner_mode.currentText()}, 反復数: {max_iters}, パレット: {palette}")
        self.status.setText("描画計画を生成中… 停止できます。")

        try:
            planner = self._planner()
            worker = PlanWorker(
                planner=planner,
                prompt=self.prompt.toPlainText().strip(),
                seed=self.seed.value(),
                count=self.count.value(),
                width=float(document.width()),
                height=float(document.height()),
                image_data=self._image_bytes,
                max_iterations=max_iters,
                palette_name=palette,
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
            self.status.setText(f"エラー: {exc}")
            QMessageBox.critical(self, "AI Stroke Painter", str(exc))
            self._reset_run_state()

    def _on_iteration_progress(self, current: int, total: int, msg: str) -> None:
        self.status.setText(msg)

    def _on_plan_ready(self, plan: DrawingPlan) -> None:
        self._last_plan = plan
        self.preview.set_plan(plan)

        document = self._active_doc or (Krita.instance().activeDocument() if Krita.instance() is not None else None)

        try:
            if document is None:
                self.status.setText("ドキュメントが閉じられたため描画を中断しました")
                self._log_debug("[描画中断] アクティブなドキュメントがありません")
                self._cancel = True
                if self._worker is not None and hasattr(self._worker, "cancel"):
                    self._worker.cancel()
                return

            if self.is_cancelled():
                return

            paths = []
            if self.save_json.isChecked():
                saved_json_path = save_plan(plan)
                paths.append(str(saved_json_path))
                self._log_debug(f"[JSON保存] {saved_json_path}")
            if self.save_svg_chk.isChecked():
                saved_svg_path = save_svg(plan)
                paths.append(str(saved_svg_path))
                self._log_debug(f"[SVG保存] {saved_svg_path}")

            self._log_debug(f"[描画レンダリング開始] ストローク本数={len(plan.strokes)}")
            rendered = self.canvas_port.render(document, plan, self.is_cancelled)
            suffix = f" ({', '.join(paths)})" if paths else ""
            if self.is_cancelled():
                self.status.setText(f"{rendered}本を描画して停止しました{suffix}")
                self._log_debug(f"[描画停止] {rendered} 本を描画後に停止")
            else:
                self.status.setText(f"描画完了: {rendered}本を生成しました{suffix}")
                self._log_debug(f"[描画完了] 合計 {rendered} 本をキャンバスに描画しました")
        except Exception as exc:
            self._log_debug(f"[描画レンダリング例外] {exc}\n{traceback.format_exc()}")
            self.status.setText(f"描画エラー: {exc}")
        finally:
            # 次イテレーション用のキャンバスキャプチャをメインスレッドで安全に取得してワーカーへ渡す
            if self._worker is not None:
                if (
                    document is not None
                    and not self.is_cancelled()
                    and hasattr(self._worker, "provide_canvas_capture")
                    and self._worker.max_iterations > plan.iteration
                ):
                    self._log_debug("[自律改善] メインスレッドでキャンバスキャプチャを取得中...")
                    cap_img = self.canvas_port.capture_canvas(document, 512, 512)
                    self._log_debug(f"[自律改善] キャプチャ完了 ({len(cap_img)} bytes)")
                    self._worker.provide_canvas_capture(cap_img)
                elif hasattr(self._worker, "notify_render_done"):
                    self._worker.notify_render_done()
            if self._worker is None or not self._worker.isRunning():
                self._reset_run_state()

    def _on_plan_failed(self, error_msg: str) -> None:
        self._worker = None
        self._log_debug(f"[計画生成失敗] {error_msg}")
        if not self.is_cancelled():
            if self.debug_mode_chk.isChecked():
                self.status.setText(f"エラー: {error_msg} (詳細はデバッグログ参照)")
            else:
                self.status.setText(f"エラー: {error_msg}")
            QMessageBox.critical(self, "AI Stroke Painter エラー", error_msg)
        self._reset_run_state()

    def _on_worker_finished(self) -> None:
        self._reset_run_state()

    def _reset_run_state(self) -> None:
        self.progress.setRange(0, 1)
        self.progress.setValue(1)
        self.run_btn.setEnabled(True)
        self.stop_btn.setEnabled(False)
        self._active_doc = None
        self._worker = None
