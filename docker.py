"""AI Stroke Painter Pro Krita Docker UI および非同期制御ワーカー。"""

from __future__ import annotations

import contextlib
import datetime
import json
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
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QInputDialog,
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
    password_echo_mode,
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


def _get_attr(obj: Any, name: str, default: Any = None) -> Any:
    """安全にインスタンス属性を取得 (未初期化QtモックでのRuntimeErrorを回避)。"""
    try:
        if hasattr(obj, "__dict__") and name in obj.__dict__:
            return obj.__dict__[name]
        return getattr(obj, name, default)
    except Exception:
        return default


class PreviewWidget(QWidget):
    """描画計画のストロークをリアルタイムでベクタープレビューするミニキャンバス。"""

    def __init__(self, parent: Any | None = None) -> None:
        super().__init__(parent)
        self._plan: DrawingPlan | None = None
        self._size_multiplier: float = 1.0
        self._opacity_multiplier: float = 1.0
        self.setMinimumHeight(160)
        self.setMaximumHeight(200)

    def set_plan(
        self,
        plan: DrawingPlan | None,
        size_multiplier: float = 1.0,
        opacity_multiplier: float = 1.0,
    ) -> None:
        self._plan = plan
        self._size_multiplier = float(size_multiplier)
        self._opacity_multiplier = float(opacity_multiplier)
        self.update()

    def update_multipliers(self, size_multiplier: float, opacity_multiplier: float) -> None:
        self._size_multiplier = float(size_multiplier)
        self._opacity_multiplier = float(opacity_multiplier)
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
                eff_op = max(0.01, min(1.0, stroke.opacity * self._opacity_multiplier))
                if eff_op < 1.0 and hasattr(col, "setAlphaF"):
                    col.setAlphaF(eff_op)
                pen_w = max(1.0, stroke.size_px * self._size_multiplier * scale * 0.6)
                pen = QPen(col, pen_w)
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
        brush_profile: str = "auto",
        edge_threshold: float = 0.18,
        shading_density: str = "medium",
        enable_flats: bool = True,
        color_mode: str = "original",
        parent: Any | None = None,
        canvas_port: Any | None = None,
        document: Any | None = None,
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
        self.brush_profile = brush_profile
        self.edge_threshold = edge_threshold
        self.shading_density = shading_density
        self.enable_flats = enable_flats
        self.color_mode = color_mode
        self._is_cancelled = False
        self._render_done_event = threading.Event()
        self._next_canvas_image: bytes | None = None
        self._thread: threading.Thread | None = None
        self._is_running = False

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
                f"[ワーカー開始] Total Iterations: {self.max_iterations}, Target Size: {self.width:.0f}x{self.height:.0f}, Palette: {self.palette_name}, Profile: {self.brush_profile}"
            )

            for iter_idx in range(1, self.max_iterations + 1):
                if self.is_cancelled():
                    self.debug_log.emit("[ワーカー] 処理が中断されました")
                    return

                msg = f"ステップ {iter_idx}/{self.max_iterations}: 計画を生成中..."
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
                )

                if self.is_cancelled():
                    self.debug_log.emit("[ワーカー] 描画計画受領後にキャンセルを確認しました")
                    return

                self.debug_log.emit(
                    f"[ステップ {iter_idx}] 計画生成完了。メインスレッドへ描画を要求します (ストローク数: {len(current_plan.strokes)})"
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
                self.iteration_progress.emit(self.max_iterations, self.max_iterations, "全ステップの描画が完了しました")
                self.debug_log.emit("[ワーカー完了] 全ての処理が正常に完了しました")

        except Exception as exc:
            tb = traceback.format_exc()
            self.debug_log.emit(f"[例外発生] {exc}\nスタックトレース:\n{tb}")
            if not self._is_cancelled:
                self.plan_failed.emit(str(exc))


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
        self._active_doc: Any | None = None
        self._canvas: Any | None = None
        self._image_bytes: bytes | None = None
        self._last_plan: DrawingPlan | None = None

        container = QWidget(self)
        root_layout = QVBoxLayout(container)

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
        self.save_preset_btn = QPushButton("💾 プリセット保存...")
        self.save_preset_btn.clicked.connect(self._save_custom_preset)
        self.del_preset_btn = QPushButton("🗑️ 削除")
        self.del_preset_btn.clicked.connect(self._delete_custom_preset)
        preset_row2.addWidget(self.save_preset_btn)
        preset_row2.addWidget(self.del_preset_btn)
        preset_layout.addLayout(preset_row2)

        root_layout.addWidget(preset_box)

        # 2. プロンプト入力欄
        root_layout.addWidget(QLabel("描画指示 (Prompt)"))
        self.prompt = QPlainTextEdit("anime girl portrait, delicate eyes, flowing hair")
        self.prompt.setMaximumHeight(65)
        root_layout.addWidget(self.prompt)

        # 3. 参照画像 (Image-to-Stroke) パネル
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
        root_layout.addWidget(image_box)

        # 4. 基本生成パラメータ (Seed, 本数, パレット, ブラシスタイル, 段階的改善)
        gen_box = QGroupBox("生成パラメータ & スタイル")
        gen_layout = QVBoxLayout(gen_box)

        params_row1 = QHBoxLayout()
        params_row1.addWidget(QLabel("Seed"))
        self.seed = QSpinBox()
        self.seed.setRange(0, 2147483647)
        self.seed.setValue(42)
        params_row1.addWidget(self.seed)

        params_row1.addWidget(QLabel("本数"))
        self.count = QSpinBox()
        self.count.setRange(1, 500)
        self.count.setValue(35)
        params_row1.addWidget(self.count)
        gen_layout.addLayout(params_row1)

        style_row = QHBoxLayout()
        style_row.addWidget(QLabel("パレット"))
        self.palette_combo = QComboBox()
        self.palette_combo.addItem("アニメカラー", "anime")
        self.palette_combo.addItem("モノクロ線画", "monochrome")
        self.palette_combo.addItem("サイバーパンク", "cyberpunk")
        self.palette_combo.addItem("自然アースカラー", "nature")
        self.palette_combo.addItem("パステル・ゆめかわ", "pastel")
        self.palette_combo.addItem("透明水彩", "watercolor")
        self.palette_combo.addItem("80s レトロポップ", "retro_pop")
        self.palette_combo.addItem("ダークファンタジー", "dark_fantasy")
        self.palette_combo.addItem("クラシックセピア", "sepia")
        style_row.addWidget(self.palette_combo)

        style_row.addWidget(QLabel("タッチ"))
        self.brush_profile = QComboBox()
        self.brush_profile.addItem("自動 (Auto)", "auto")
        self.brush_profile.addItem("Gペン (筆圧強)", "gpen")
        self.brush_profile.addItem("丸ペン (均一線)", "marupen")
        self.brush_profile.addItem("毛筆・水彩", "brush")
        self.brush_profile.addItem("マーカー", "marker")
        style_row.addWidget(self.brush_profile)
        gen_layout.addLayout(style_row)

        refine_layout = QHBoxLayout()
        self.auto_refine = QCheckBox("段階的ステップ描画 (Auto-Refine)")
        self.auto_refine.setToolTip(
            "下塗り→陰影→線画→ハイライトを段階的に描き進めます。AI要求時のみキャンバス画像を送信します。"
        )
        self.auto_refine.setChecked(False)
        refine_layout.addWidget(self.auto_refine)
        refine_layout.addWidget(QLabel("反復数"))
        self.iterations = QSpinBox()
        self.iterations.setRange(1, 10)
        self.iterations.setValue(3)
        refine_layout.addWidget(self.iterations)
        gen_layout.addLayout(refine_layout)

        root_layout.addWidget(gen_box)

        # 5. 🖌️ ブラシ・描画 & レイヤーカスタマイズ
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
        self.layer_mode.setToolTip("ストロークをレイヤー別に自動分割するか、単一レイヤーに描画するかを選択します")
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
        self.event_interval.setToolTip("描画中の画面再描画頻度 (値が小さいほどリアルタイム、大きいほど高速)")
        config_row.addWidget(QLabel("描画更新間隔:"))
        config_row.addWidget(self.event_interval)
        brush_form.addRow("キャンバス設定", config_row)

        root_layout.addWidget(brush_box)

        # 6. Planner 選択 & LLM 設定
        root_layout.addWidget(QLabel("Planner エンジン"))
        self.planner_mode = QComboBox()
        self.planner_mode.addItem("プロシージャル (オフライン 高品質)", "offline")
        self.planner_mode.addItem("OpenAI 互換 LLM / Vision", "openai_compatible")
        root_layout.addWidget(self.planner_mode)

        self.llm_settings = QGroupBox("OpenAI 互換 API 詳細設定")
        llm_form = QFormLayout(self.llm_settings)
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

        test_conn_btn = QPushButton("API 接続テスト")
        test_conn_btn.clicked.connect(self._test_api_connection)
        llm_form.addRow("", test_conn_btn)
        root_layout.addWidget(self.llm_settings)

        # 7. ベクタープレビュー
        self.preview = PreviewWidget(self)
        root_layout.addWidget(self.preview)

        # 8. 保存オプション & デバッグモード & 設定初期化
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

        self.reset_defaults_btn = QPushButton("🔄 初期設定に戻す")
        self.reset_defaults_btn.clicked.connect(self._reset_to_defaults)
        debug_toggle_layout.addWidget(self.reset_defaults_btn)
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
        self.brush_size_multiplier.valueChanged.connect(self._update_preview_multipliers)
        self.opacity_multiplier.valueChanged.connect(self._update_preview_multipliers)
        self._update_planner_settings_state()

    def canvasChanged(self, canvas: Any) -> None:  # noqa: N802
        """Kritaからキャンバス切り替えイベント通知を受け取る (DockWidgetの必須抽象メソッド)。"""
        self._canvas = canvas

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
                settings = QSettings("AIStrokePainter", "CustomPresets")
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
        prompt_w = _get_attr(self, "prompt")
        pal_w = _get_attr(self, "palette_combo")
        count_w = _get_attr(self, "count")
        prof_w = _get_attr(self, "brush_profile")
        bs_w = _get_attr(self, "brush_size_multiplier")
        op_w = _get_attr(self, "opacity_multiplier")

        data = {
            "prompt": prompt_w.toPlainText() if prompt_w is not None and hasattr(prompt_w, "toPlainText") else "",
            "palette": pal_w.currentData() if pal_w is not None and hasattr(pal_w, "currentData") else "anime",
            "count": count_w.value() if count_w is not None and hasattr(count_w, "value") else 35,
            "brush_profile": prof_w.currentData() if prof_w is not None and hasattr(prof_w, "currentData") else "auto",
            "brush_size": bs_w.value() if bs_w is not None and hasattr(bs_w, "value") else 1.0,
            "opacity": op_w.value() if op_w is not None and hasattr(op_w, "value") else 100,
            "custom": True,
        }

        if callable(QSettings):
            with contextlib.suppress(Exception):
                settings = QSettings("AIStrokePainter", "CustomPresets")
                raw_json = settings.value("presets_json")
                custom_map: dict[str, Any] = json.loads(str(raw_json)) if raw_json else {}
                custom_map[name] = data
                settings.setValue("presets_json", json.dumps(custom_map, ensure_ascii=False))
                if hasattr(settings, "sync"):
                    settings.sync()

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

        name = current_text.replace("⭐ [カスタム] ", "").strip()
        QMessageBox.question(
            self,
            "プリセット削除確認",
            f"カスタムプリセット '{name}' を削除しますか？",
        )
        if callable(QSettings):
            with contextlib.suppress(Exception):
                settings = QSettings("AIStrokePainter", "CustomPresets")
                raw_json = settings.value("presets_json")
                if raw_json:
                    custom_map: dict[str, Any] = json.loads(str(raw_json))
                    custom_map.pop(name, None)
                    settings.setValue("presets_json", json.dumps(custom_map, ensure_ascii=False))
                    if hasattr(settings, "sync"):
                        settings.sync()

        self._populate_presets()
        self._log_debug(f"[プリセット削除] カスタムプリセット '{name}' を削除しました")

    def _reset_to_defaults(self) -> None:
        """全設定値を標準デフォルト値にリセットする。"""
        QMessageBox.question(
            self,
            "設定リセット",
            "すべての設定を初期値に戻しますか？",
        )
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
        w = _get_attr(self, "save_json")
        if w is not None and hasattr(w, "setChecked"):
            w.setChecked(True)
        w = _get_attr(self, "save_svg_chk")
        if w is not None and hasattr(w, "setChecked"):
            w.setChecked(True)
        w = _get_attr(self, "debug_mode_chk")
        if w is not None and hasattr(w, "setChecked"):
            w.setChecked(False)
        self._save_settings()
        self._log_debug("[設定リセット] 全設定を初期値に戻しました")

    def _load_settings(self) -> None:
        """QSettings から前回の UI 設定値を自動復元する。"""
        if QSettings is None or not callable(QSettings):
            return
        with contextlib.suppress(Exception):
            settings: Any = QSettings("AIStrokePainter", "DockerSettings")
            w = _get_attr(self, "base_url")
            if w is not None and settings.value("base_url") is not None:
                w.setText(str(settings.value("base_url")))
            w = _get_attr(self, "model")
            if w is not None and settings.value("model") is not None:
                w.setText(str(settings.value("model")))
            w = _get_attr(self, "timeout_sec")
            if w is not None and settings.value("timeout_sec") is not None:
                w.setValue(int(settings.value("timeout_sec")))
            w = _get_attr(self, "max_tokens")
            if w is not None and settings.value("max_tokens") is not None:
                w.setValue(int(settings.value("max_tokens")))
            w = _get_attr(self, "reasoning_effort")
            if w is not None and settings.value("reasoning_effort") is not None:
                effort_val = str(settings.value("reasoning_effort"))
                if hasattr(w, "count") and hasattr(w, "itemData"):
                    for i in range(w.count()):
                        if w.itemData(i) == effort_val:
                            w.setCurrentIndex(i)
                            break
            w = _get_attr(self, "temperature")
            if w is not None and settings.value("temperature") is not None:
                w.setValue(float(settings.value("temperature")))
            w = _get_attr(self, "top_p")
            if w is not None and settings.value("top_p") is not None:
                w.setValue(float(settings.value("top_p")))
            w = _get_attr(self, "vision_res")
            if w is not None and settings.value("vision_res") is not None:
                vres_val = int(settings.value("vision_res"))
                if hasattr(w, "count") and hasattr(w, "itemData"):
                    for i in range(w.count()):
                        if w.itemData(i) == vres_val:
                            w.setCurrentIndex(i)
                            break
            w = _get_attr(self, "custom_instructions")
            if w is not None and settings.value("custom_instructions") is not None:
                w.setText(str(settings.value("custom_instructions")))
            w = _get_attr(self, "prompt")
            if w is not None and settings.value("prompt") is not None:
                if hasattr(w, "setPlainText"):
                    w.setPlainText(str(settings.value("prompt")))
                elif hasattr(w, "setText"):
                    w.setText(str(settings.value("prompt")))
            w = _get_attr(self, "seed")
            if w is not None and settings.value("seed") is not None:
                w.setValue(int(settings.value("seed")))
            w = _get_attr(self, "count")
            if w is not None and settings.value("count") is not None:
                w.setValue(int(settings.value("count")))
            w = _get_attr(self, "palette_combo")
            if w is not None and settings.value("palette") is not None:
                pal_val = str(settings.value("palette"))
                if hasattr(w, "count") and hasattr(w, "itemData"):
                    for i in range(w.count()):
                        if w.itemData(i) == pal_val:
                            w.setCurrentIndex(i)
                            break
            w = _get_attr(self, "brush_profile")
            if w is not None and settings.value("brush_profile") is not None:
                prof_val = str(settings.value("brush_profile"))
                if hasattr(w, "count") and hasattr(w, "itemData"):
                    for i in range(w.count()):
                        if w.itemData(i) == prof_val:
                            w.setCurrentIndex(i)
                            break
            w = _get_attr(self, "iterations")
            if w is not None and settings.value("iterations") is not None:
                w.setValue(int(settings.value("iterations")))
            w = _get_attr(self, "auto_refine")
            if w is not None and settings.value("auto_refine") is not None:
                w.setChecked(str(settings.value("auto_refine")).lower() in ("true", "1"))
            w = _get_attr(self, "brush_size_multiplier")
            if w is not None and settings.value("brush_size_multiplier") is not None:
                w.setValue(float(settings.value("brush_size_multiplier")))
            w = _get_attr(self, "opacity_multiplier")
            if w is not None and settings.value("opacity_multiplier") is not None:
                w.setValue(int(settings.value("opacity_multiplier")))
            w = _get_attr(self, "layer_mode")
            if w is not None and settings.value("layer_mode") is not None:
                lmode_val = str(settings.value("layer_mode"))
                if hasattr(w, "count") and hasattr(w, "itemData"):
                    for i in range(w.count()):
                        if w.itemData(i) == lmode_val:
                            w.setCurrentIndex(i)
                            break
            w = _get_attr(self, "layer_prefix")
            if w is not None and settings.value("layer_prefix") is not None:
                w.setText(str(settings.value("layer_prefix")))
            w = _get_attr(self, "event_interval")
            if w is not None and settings.value("event_interval") is not None:
                w.setValue(int(settings.value("event_interval")))
            w = _get_attr(self, "edge_threshold")
            if w is not None and settings.value("edge_threshold") is not None:
                w.setValue(float(settings.value("edge_threshold")))
            w = _get_attr(self, "shading_density")
            if w is not None and settings.value("shading_density") is not None:
                s_val = str(settings.value("shading_density"))
                if hasattr(w, "count") and hasattr(w, "itemData"):
                    for i in range(w.count()):
                        if w.itemData(i) == s_val:
                            w.setCurrentIndex(i)
                            break
            w = _get_attr(self, "enable_flats")
            if w is not None and settings.value("enable_flats") is not None:
                w.setChecked(str(settings.value("enable_flats")).lower() in ("true", "1"))
            w = _get_attr(self, "image_color_mode")
            if w is not None and settings.value("image_color_mode") is not None:
                c_val = str(settings.value("image_color_mode"))
                if hasattr(w, "count") and hasattr(w, "itemData"):
                    for i in range(w.count()):
                        if w.itemData(i) == c_val:
                            w.setCurrentIndex(i)
                            break
            w = _get_attr(self, "save_json")
            if w is not None and settings.value("save_json") is not None:
                w.setChecked(str(settings.value("save_json")).lower() in ("true", "1"))
            w = _get_attr(self, "save_svg_chk")
            if w is not None and settings.value("save_svg") is not None:
                w.setChecked(str(settings.value("save_svg")).lower() in ("true", "1"))
            w = _get_attr(self, "debug_mode_chk")
            if w is not None and settings.value("debug_mode") is not None:
                w.setChecked(str(settings.value("debug_mode")).lower() in ("true", "1"))

    def _save_settings(self) -> None:
        """現在の UI 設定値を QSettings に保存する。"""
        if QSettings is None or not callable(QSettings):
            return
        with contextlib.suppress(Exception):
            settings: Any = QSettings("AIStrokePainter", "DockerSettings")
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
            w = _get_attr(self, "prompt")
            if w is not None and hasattr(w, "toPlainText"):
                settings.setValue("prompt", w.toPlainText())
            w = _get_attr(self, "seed")
            if w is not None and hasattr(w, "value"):
                settings.setValue("seed", w.value())
            w = _get_attr(self, "count")
            if w is not None and hasattr(w, "value"):
                settings.setValue("count", w.value())
            w = _get_attr(self, "palette_combo")
            if w is not None and hasattr(w, "currentData"):
                settings.setValue("palette", w.currentData() or "anime")
            w = _get_attr(self, "brush_profile")
            if w is not None and hasattr(w, "currentData"):
                settings.setValue("brush_profile", w.currentData() or "auto")
            w = _get_attr(self, "iterations")
            if w is not None and hasattr(w, "value"):
                settings.setValue("iterations", w.value())
            w = _get_attr(self, "auto_refine")
            if w is not None and hasattr(w, "isChecked"):
                settings.setValue("auto_refine", w.isChecked())
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
        elif isinstance(data, (list, tuple)):
            # ビルトインプリセット
            prompt_text = data[1]
            palette = data[2]
            count = data[3]
            if prompt_w is not None and hasattr(prompt_w, "setPlainText"):
                prompt_w.setPlainText(prompt_text)
            if count_w is not None and hasattr(count_w, "setValue"):
                count_w.setValue(count)
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
        file_path, _ = QFileDialog.getOpenFileName(
            self, "参照画像を開く", "", "画像ファイル (*.png *.jpg *.jpeg *.webp *.bmp)"
        )
        if file_path and Path(file_path).is_file():
            try:
                self._image_bytes = Path(file_path).read_bytes()
                lbl = _get_attr(self, "image_status_label")
                if lbl is not None and hasattr(lbl, "setText"):
                    lbl.setText(Path(file_path).name)
                btn = _get_attr(self, "clear_image_btn")
                if btn is not None and hasattr(btn, "setEnabled"):
                    btn.setEnabled(True)
                self._log_debug(f"[参照画像読込] {file_path} ({len(self._image_bytes)} bytes)")
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
                    vision_resolution=vres_val,
                ),
                log_callback=self._log_debug,
            )
            msg = planner.test_connection()
            QMessageBox.information(self, "API 接続テスト", msg)
        except Exception as exc:
            self._log_debug(f"[API 接続テスト失敗] {exc}")
            QMessageBox.critical(self, "接続テスト失敗", str(exc))

    def _update_planner_settings_state(self, *_args: Any) -> None:
        is_openai = self._is_openai_compatible_mode()
        box = _get_attr(self, "llm_settings")
        if box is not None and hasattr(box, "setEnabled"):
            box.setEnabled(is_openai)
        ref = _get_attr(self, "auto_refine")
        if ref is not None and hasattr(ref, "setEnabled"):
            ref.setEnabled(is_openai)
            if not is_openai and hasattr(ref, "setChecked"):
                ref.setChecked(False)
        iters = _get_attr(self, "iterations")
        if iters is not None and hasattr(iters, "setEnabled"):
            iters.setEnabled(is_openai)

    def _is_openai_compatible_mode(self) -> bool:
        mode_w = _get_attr(self, "planner_mode")
        if mode_w is None:
            return False
        mode_data = mode_w.currentData() if hasattr(mode_w, "currentData") else None
        mode_text = mode_w.currentText() if hasattr(mode_w, "currentText") else ""
        idx = mode_w.currentIndex() if hasattr(mode_w, "currentIndex") else 0
        return mode_data == "openai_compatible" or "OpenAI" in mode_text or idx == 1

    def _planner(self) -> PlannerPort:
        is_openai = self._is_openai_compatible_mode()

        if not is_openai:
            self._log_debug("[エンジン選択] プロシージャル (オフライン)")
            p = _get_attr(self, "planner")
            return p if p is not None else self.planner

        b_w = _get_attr(self, "base_url")
        m_w = _get_attr(self, "model")
        k_w = _get_attr(self, "api_key")
        t_w = _get_attr(self, "timeout_sec")
        tok_w = _get_attr(self, "max_tokens")
        eff_w = _get_attr(self, "reasoning_effort")
        temp_w = _get_attr(self, "temperature")
        top_w = _get_attr(self, "top_p")
        cust_w = _get_attr(self, "custom_instructions")
        vres_w = _get_attr(self, "vision_res")

        base_url_val = b_w.text() if b_w is not None and hasattr(b_w, "text") else "https://api.openai.com/v1"
        model_val = m_w.text() if m_w is not None and hasattr(m_w, "text") else "gpt-4o"
        key_val = k_w.text().strip() if k_w is not None and hasattr(k_w, "text") else ""
        timeout_val = float(t_w.value()) if t_w is not None and hasattr(t_w, "value") else 120.0
        max_tokens_val = tok_w.value() if tok_w is not None and hasattr(tok_w, "value") else 16384
        effort_val = (eff_w.currentData() or "low") if eff_w is not None and hasattr(eff_w, "currentData") else "low"
        temp_val = float(temp_w.value()) if temp_w is not None and hasattr(temp_w, "value") else 0.70
        top_p_val = float(top_w.value()) if top_w is not None and hasattr(top_w, "value") else 1.0
        custom_val = cust_w.text().strip() if cust_w is not None and hasattr(cust_w, "text") else ""
        vres = int(vres_w.currentData() or 512) if vres_w is not None and hasattr(vres_w, "currentData") else 512

        self._log_debug(
            f"[エンジン選択] OpenAI 互換 API (Base URL: {base_url_val}, Model: {model_val}, MaxTokens: {max_tokens_val}, ReasoningEffort: {effort_val}, Temp: {temp_val:.2f}, TopP: {top_p_val:.2f}, VisionRes: {vres})"
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
                vision_resolution=vres,
            ),
            log_callback=self._log_debug,
        )

    def is_cancelled(self) -> bool:
        return self._cancel

    def cancel(self) -> None:
        self._cancel = True
        worker = _get_attr(self, "_worker")
        if worker is not None and hasattr(worker, "cancel"):
            worker.cancel()
        st = _get_attr(self, "status")
        if st is not None and hasattr(st, "setText"):
            st.setText("停止要求を受け付けました。現在の処理完了後に停止します。")
        self._log_debug("[UI] 停止ボタンが押下されました")

    def run(self) -> None:
        document: Any | None = Krita.instance().activeDocument() if Krita.instance() is not None else None
        if document is None:
            QMessageBox.warning(
                self, "AI Stroke Painter", "先にドキュメントを開いてください。描画先キャンバスがありません。"
            )
            return

        self._save_settings()
        self._active_doc = document
        self._cancel = False
        run_b = _get_attr(self, "run_btn")
        if run_b is not None and hasattr(run_b, "setEnabled"):
            run_b.setEnabled(False)
        stop_b = _get_attr(self, "stop_btn")
        if stop_b is not None and hasattr(stop_b, "setEnabled"):
            stop_b.setEnabled(True)
        prog = _get_attr(self, "progress")
        if prog is not None and hasattr(prog, "setRange"):
            prog.setRange(0, 0)

        iter_w = _get_attr(self, "iterations")
        ref_w = _get_attr(self, "auto_refine")
        pal_w = _get_attr(self, "palette_combo")
        prof_w = _get_attr(self, "brush_profile")
        bs_w = _get_attr(self, "brush_size_multiplier")
        op_w = _get_attr(self, "opacity_multiplier")

        max_iters = (
            iter_w.value()
            if iter_w is not None and ref_w is not None and self._is_openai_compatible_mode() and ref_w.isChecked()
            else 1
        )
        palette = (pal_w.currentData() or "anime") if pal_w is not None and hasattr(pal_w, "currentData") else "anime"
        profile = (prof_w.currentData() or "auto") if prof_w is not None and hasattr(prof_w, "currentData") else "auto"
        size_val = bs_w.value() if bs_w is not None and hasattr(bs_w, "value") else 1.0
        op_val = op_w.value() if op_w is not None and hasattr(op_w, "value") else 100

        now_str = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        self._log_debug(f"\n========== 描画タスク開始 [{now_str}] ==========")
        mode_w = _get_attr(self, "planner_mode")
        mode_str = mode_w.currentText() if mode_w is not None and hasattr(mode_w, "currentText") else ""
        self._log_debug(
            f"選択モード: {mode_str}, 反復数: {max_iters}, パレット: {palette}, プロファイル: {profile}, 太さ倍率: {size_val}x, 不透明度: {op_val}%"
        )
        st = _get_attr(self, "status")
        if st is not None and hasattr(st, "setText"):
            st.setText("描画計画を生成中… 停止できます。")

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
            seed_val = seed_w.value() if seed_w is not None and hasattr(seed_w, "value") else 42
            count_val = count_w.value() if count_w is not None and hasattr(count_w, "value") else 35
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
                width=float(document.width()),
                height=float(document.height()),
                image_data=self._image_bytes,
                max_iterations=max_iters,
                palette_name=palette,
                brush_profile=profile,
                edge_threshold=e_thresh,
                shading_density=s_dens,
                enable_flats=flats_val,
                color_mode=c_mode,
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
        st = _get_attr(self, "status")
        if st is not None and hasattr(st, "setText"):
            st.setText(msg)

    def _on_plan_ready(self, plan: DrawingPlan) -> None:
        self._last_plan = plan
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

        if prev_w is not None and hasattr(prev_w, "set_plan"):
            try:
                prev_w.set_plan(plan, size_multiplier=size_mult, opacity_multiplier=op_mult)
            except TypeError:
                prev_w.set_plan(plan)

        active_doc = _get_attr(self, "_active_doc")
        document = active_doc or (Krita.instance().activeDocument() if Krita.instance() is not None else None)

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

            paths = []
            if sj_w is not None and hasattr(sj_w, "isChecked") and sj_w.isChecked():
                saved_json_path = save_plan(plan)
                paths.append(str(saved_json_path))
                self._log_debug(f"[JSON保存] {saved_json_path}")
            if ss_w is not None and hasattr(ss_w, "isChecked") and ss_w.isChecked():
                saved_svg_path = save_svg(plan)
                paths.append(str(saved_svg_path))
                self._log_debug(f"[SVG保存] {saved_svg_path}")

            l_mode = (
                str(lm_w.currentData() or "multi_layer")
                if lm_w is not None and hasattr(lm_w, "currentData")
                else "multi_layer"
            )
            l_prefix = (
                str(lp_w.text().strip() or "AI Artwork") if lp_w is not None and hasattr(lp_w, "text") else "AI Artwork"
            )
            e_interval = int(ei_w.value()) if ei_w is not None and hasattr(ei_w, "value") else 30

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
            )
            suffix = f" ({', '.join(paths)})" if paths else ""
            st_w = _get_attr(self, "status")
            if self.is_cancelled():
                if st_w is not None and hasattr(st_w, "setText"):
                    st_w.setText(f"{rendered}本を描画して停止しました{suffix}")
                self._log_debug(f"[描画停止] {rendered} 本を描画後に停止")
            else:
                if st_w is not None and hasattr(st_w, "setText"):
                    st_w.setText(f"描画完了: {rendered}本を生成しました{suffix}")
                self._log_debug(f"[描画完了] 合計 {rendered} 本をキャンバスに描画しました")
        except Exception as exc:
            self._log_debug(f"[描画レンダリング例外] {exc}\n{traceback.format_exc()}")
            st_w = _get_attr(self, "status")
            if st_w is not None and hasattr(st_w, "setText"):
                st_w.setText(f"描画エラー: {exc}")
        finally:
            worker = _get_attr(self, "_worker")
            if worker is not None:
                if (
                    document is not None
                    and not self.is_cancelled()
                    and hasattr(worker, "provide_canvas_capture")
                    and getattr(worker, "max_iterations", 1) > plan.iteration
                ):
                    if plan.request_canvas_image:
                        vres_w = _get_attr(self, "vision_res")
                        vres = (
                            int(vres_w.currentData() or 512)
                            if vres_w is not None and hasattr(vres_w, "currentData")
                            else 512
                        )
                        self._log_debug(
                            f"[AI画像要求] AIが視覚確認を要求したため、現在のキャンバスキャプチャを取得します (解像度: {vres}x{vres})..."
                        )
                        cap_img = cp.capture_canvas(document, vres, vres)
                        self._log_debug(
                            f"[AI画像要求] キャプチャ完了 ({len(cap_img)} bytes)。次ステップへ画像を送信します"
                        )
                        worker.provide_canvas_capture(cap_img)
                    else:
                        self._log_debug(
                            "[高速進行] AIからの画像要求がないため、画像送信をスキップして次ステップへ高速進行します"
                        )
                        worker.provide_canvas_capture(None)
                elif hasattr(worker, "notify_render_done"):
                    worker.notify_render_done()
            if worker is None or not (hasattr(worker, "isRunning") and worker.isRunning()):
                self._reset_run_state()

    def _on_plan_failed(self, error_msg: str) -> None:
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
        self._reset_run_state()

    def _reset_run_state(self) -> None:
        prog = _get_attr(self, "progress")
        if prog is not None and hasattr(prog, "setRange"):
            prog.setRange(0, 1)
            prog.setValue(1)
        r_btn = _get_attr(self, "run_btn")
        if r_btn is not None and hasattr(r_btn, "setEnabled"):
            r_btn.setEnabled(True)
        s_btn = _get_attr(self, "stop_btn")
        if s_btn is not None and hasattr(s_btn, "setEnabled"):
            s_btn.setEnabled(False)
        self._active_doc = None
        self._worker = None
