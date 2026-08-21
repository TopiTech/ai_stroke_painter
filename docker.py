from __future__ import annotations

import contextlib
import importlib
import os
from pathlib import Path
from typing import TYPE_CHECKING, Any

from .domain import DrawingPlan
from .krita_adapter import KritaCanvasAdapter
from .llm_planner import OpenAICompatiblePlanner, OpenAICompatibleSettings
from .planner import RuleBasedPlanner
from .ports import PlannerPort
from .storage import save_plan, save_svg

if TYPE_CHECKING:
    from PyQt5.QtCore import QObject, QThread, pyqtSignal
    from PyQt5.QtWidgets import (
        QCheckBox,
        QComboBox,
        QFileDialog,
        QFormLayout,
        QGroupBox,
        QHBoxLayout,
        QLabel,
        QLineEdit,
        QMessageBox,
        QPlainTextEdit,
        QProgressBar,
        QPushButton,
        QSpinBox,
        QVBoxLayout,
        QWidget,
    )
else:
    try:
        from PyQt5.QtCore import QObject, QThread, pyqtSignal
        from PyQt5.QtWidgets import (
            QCheckBox,
            QComboBox,
            QFileDialog,
            QFormLayout,
            QGroupBox,
            QHBoxLayout,
            QLabel,
            QLineEdit,
            QMessageBox,
            QPlainTextEdit,
            QProgressBar,
            QPushButton,
            QSpinBox,
            QVBoxLayout,
            QWidget,
        )
    except ImportError:
        try:
            from PyQt6.QtCore import QObject, QThread, pyqtSignal
            from PyQt6.QtWidgets import (
                QCheckBox,
                QComboBox,
                QFileDialog,
                QFormLayout,
                QGroupBox,
                QHBoxLayout,
                QLabel,
                QLineEdit,
                QMessageBox,
                QPlainTextEdit,
                QProgressBar,
                QPushButton,
                QSpinBox,
                QVBoxLayout,
                QWidget,
            )
        except ImportError:
            # PyQt がない環境（CI/テスト等）用のフォールバックスタブ
            class QObject:  # type: ignore[no-redef]
                def __init__(self, *args: Any, **kwargs: Any) -> None: ...

            class _FakeSignal:
                def __init__(self) -> None:
                    self._slots: list[Any] = []

                def connect(self, slot: Any) -> None:
                    self._slots.append(slot)

                def emit(self, *args: Any) -> None:
                    for slot in list(self._slots):
                        slot(*args)

            def pyqtSignal(*_args: Any) -> Any:  # type: ignore[no-redef]
                return _FakeSignal()

            class QThread(QObject):  # type: ignore[no-redef]
                def __init__(self, *args: Any, **kwargs: Any) -> None:
                    super().__init__()
                    self.finished = _FakeSignal()

                def start(self) -> None:
                    self.run()
                    self.finished.emit()

                def run(self) -> None: ...
                def isRunning(self) -> bool:
                    return False

            class QWidget(QObject):  # type: ignore[no-redef]
                def __init__(self, *args: Any, **kwargs: Any) -> None:
                    super().__init__()

                def setLayout(self, *args: Any) -> None: ...
                def update(self) -> None: ...
                def setFixedSize(self, *args: Any) -> None: ...
                def setMinimumHeight(self, *args: Any) -> None: ...
                def setMaximumHeight(self, *args: Any) -> None: ...
                def setEnabled(self, *args: Any) -> None: ...

            class QLabel(QWidget):  # type: ignore[no-redef]
                def __init__(self, text: str = "", *args: Any, **kwargs: Any) -> None:
                    super().__init__()
                    self._text = text

                def setText(self, text: str) -> None:
                    self._text = text

                def text(self) -> str:
                    return self._text

                def setWordWrap(self, *args: Any) -> None: ...

            class QPushButton(QWidget):  # type: ignore[no-redef]
                def __init__(self, text: str = "", *args: Any, **kwargs: Any) -> None:
                    super().__init__()
                    self.clicked = _FakeSignal()

            class QLineEdit(QWidget):  # type: ignore[no-redef]
                Password = 1

                def __init__(self, text: str = "", *args: Any, **kwargs: Any) -> None:
                    super().__init__()
                    self._text = text

                def text(self) -> str:
                    return self._text

                def setText(self, text: str) -> None:
                    self._text = text

                def setPlaceholderText(self, *args: Any) -> None: ...
                def setEchoMode(self, *args: Any) -> None: ...

            class QPlainTextEdit(QWidget):  # type: ignore[no-redef]
                def __init__(self, text: str = "", *args: Any, **kwargs: Any) -> None:
                    super().__init__()
                    self._text = text

                def toPlainText(self) -> str:
                    return self._text

                def setPlainText(self, text: str) -> None:
                    self._text = text

                def setMaximumHeight(self, *args: Any) -> None: ...

            class QSpinBox(QWidget):  # type: ignore[no-redef]
                def __init__(self, *args: Any, **kwargs: Any) -> None:
                    super().__init__()
                    self._val = 0

                def value(self) -> int:
                    return self._val

                def setValue(self, v: int) -> None:
                    self._val = v

                def setRange(self, *args: Any) -> None: ...

            class QCheckBox(QWidget):  # type: ignore[no-redef]
                def __init__(self, text: str = "", *args: Any, **kwargs: Any) -> None:
                    super().__init__()
                    self._checked = False

                def isChecked(self) -> bool:
                    return self._checked

                def setChecked(self, c: bool) -> None:
                    self._checked = c

            class QComboBox(QWidget):  # type: ignore[no-redef]
                def __init__(self, *args: Any, **kwargs: Any) -> None:
                    super().__init__()
                    self.currentIndexChanged = _FakeSignal()
                    self._items: list[tuple[str, Any]] = []
                    self._idx = 0

                def addItem(self, text: str, data: Any = None) -> None:
                    self._items.append((text, data))

                def itemData(self, index: int) -> Any:
                    if 0 <= index < len(self._items):
                        return self._items[index][1]
                    return None

                def count(self) -> int:
                    return len(self._items)

                def currentData(self) -> Any:
                    if self._items and 0 <= self._idx < len(self._items):
                        return self._items[self._idx][1]
                    return None

                def currentText(self) -> str:
                    if self._items and 0 <= self._idx < len(self._items):
                        return self._items[self._idx][0]
                    return ""

                def setCurrentIndex(self, i: int) -> None:
                    self._idx = i

            class QProgressBar(QWidget):  # type: ignore[no-redef]
                def __init__(self, *args: Any, **kwargs: Any) -> None:
                    super().__init__()

                def setValue(self, *args: Any) -> None: ...
                def setRange(self, *args: Any) -> None: ...

            class QGroupBox(QWidget):  # type: ignore[no-redef]
                def __init__(self, title: str = "", *args: Any, **kwargs: Any) -> None:
                    super().__init__()

            class QVBoxLayout:  # type: ignore[no-redef]
                def __init__(self, *args: Any, **kwargs: Any) -> None: ...
                def addWidget(self, *args: Any) -> None: ...
                def addLayout(self, *args: Any) -> None: ...
                def addStretch(self, *args: Any) -> None: ...

            class QHBoxLayout:  # type: ignore[no-redef]
                def __init__(self, *args: Any, **kwargs: Any) -> None: ...
                def addWidget(self, *args: Any) -> None: ...
                def addLayout(self, *args: Any) -> None: ...
                def addStretch(self, *args: Any) -> None: ...

            class QFormLayout:  # type: ignore[no-redef]
                def __init__(self, *args: Any, **kwargs: Any) -> None: ...
                def addRow(self, *args: Any) -> None: ...

            class QMessageBox:  # type: ignore[no-redef]
                @staticmethod
                def information(*args: Any) -> None: ...
                @staticmethod
                def warning(*args: Any) -> None: ...
                @staticmethod
                def critical(*args: Any) -> None: ...

            class QFileDialog:  # type: ignore[no-redef]
                @staticmethod
                def getOpenFileName(*args: Any) -> tuple[str, str]:
                    return "", ""


try:
    from krita import DockWidget, Krita
except ImportError:

    class DockWidget:  # type: ignore[no-redef]
        def __init__(self) -> None: ...
        def setWindowTitle(self, title: str) -> None: ...
        def setWidget(self, widget: Any) -> None: ...

    class Krita:  # type: ignore[no-redef]
        @staticmethod
        def instance() -> Any: ...


def _resolve_gui_paint() -> tuple[Any, Any, Any]:
    for module_base in ("PyQt5", "PyQt6"):
        try:
            gui = importlib.import_module(f"{module_base}.QtGui")
            qpainter = getattr(gui, "QPainter", None)
            qcolor = getattr(gui, "QColor", None)
            qpen = getattr(gui, "QPen", None)
            if qpainter is not None and qcolor is not None and qpen is not None:
                return qpainter, qcolor, qpen
        except (ImportError, AttributeError):
            continue
    return None, None, None


_QPAINTER_CLS, _QCOLOR_CLS, _QPEN_CLS = _resolve_gui_paint()


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
        if _QPAINTER_CLS is None or _QCOLOR_CLS is None or _QPEN_CLS is None:
            return

        painter = _QPAINTER_CLS(self)
        try:
            w = float(self.width()) if hasattr(self, "width") else 200.0
            h = float(self.height()) if hasattr(self, "height") else 160.0

            painter.fillRect(0, 0, int(w), int(h), _QCOLOR_CLS("#1e1e24"))

            if self._plan is None or not self._plan.strokes:
                painter.setPen(_QCOLOR_CLS("#777788"))
                painter.drawText(int(w * 0.2), int(h * 0.5), "ストローク プレビュー")
                return

            max_x = max((p.x for s in self._plan.strokes for p in s.points), default=w)
            max_y = max((p.y for s in self._plan.strokes for p in s.points), default=h)
            scale = min(w / max(1.0, max_x), h / max(1.0, max_y)) * 0.92
            ox = (w - max_x * scale) * 0.5
            oy = (h - max_y * scale) * 0.5

            for stroke in self._plan.strokes:
                col = _QCOLOR_CLS(stroke.color)
                if stroke.opacity < 1.0 and hasattr(col, "setAlphaF"):
                    col.setAlphaF(stroke.opacity)
                pen = _QPEN_CLS(col, max(1.0, stroke.size_px * scale * 0.6))
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


class PlanWorker(QThread):
    """自律ビジョン改善ループおよびバックグラウンド計画生成ワーカー。"""

    plan_ready = pyqtSignal(object)
    iteration_progress = pyqtSignal(int, int, str)
    plan_failed = pyqtSignal(str)

    def __init__(
        self,
        planner: PlannerPort,
        canvas_port: KritaCanvasAdapter,
        document: Any,
        prompt: str,
        seed: int,
        count: int,
        width: float,
        height: float,
        image_data: bytes | None = None,
        max_iterations: int = 1,
        palette_name: str = "anime",
        parent: Any | None = None,
    ) -> None:
        super().__init__(parent)
        self.planner = planner
        self.canvas_port = canvas_port
        self.document = document
        self.prompt = prompt
        self.seed = seed
        self.count = count
        self.width = width
        self.height = height
        self.image_data = image_data
        self.max_iterations = max_iterations
        self.palette_name = palette_name
        self._is_cancelled = False

    def cancel(self) -> None:
        self._is_cancelled = True

    def is_cancelled(self) -> bool:
        return self._is_cancelled

    def run(self) -> None:
        try:
            for iter_idx in range(1, self.max_iterations + 1):
                if self.is_cancelled():
                    return

                self.iteration_progress.emit(
                    iter_idx, self.max_iterations, f"イテレーション {iter_idx}/{self.max_iterations}: 計画を生成中..."
                )

                canvas_img: bytes | None = None
                if iter_idx > 1 and self.document is not None:
                    canvas_img = self.canvas_port.capture_canvas(self.document, 512, 512)

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
                    return

                self.plan_ready.emit(current_plan)

            if not self.is_cancelled():
                self.iteration_progress.emit(self.max_iterations, self.max_iterations, "全イテレーションが完了しました")

        except Exception as exc:
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
        llm_form.addRow("Model", self.model)
        self.api_key = QLineEdit()
        with contextlib.suppress(AttributeError):
            self.api_key.setEchoMode(QLineEdit.Password)
        self.api_key.setPlaceholderText("空欄なら OPENAI_API_KEY")
        llm_form.addRow("API Key", self.api_key)

        test_conn_btn = QPushButton("API 接続テスト")
        test_conn_btn.clicked.connect(self._test_api_connection)
        llm_form.addRow("", test_conn_btn)
        root_layout.addWidget(self.llm_settings)

        # 7. ベクタープレビュー
        self.preview = PreviewWidget(self)
        root_layout.addWidget(self.preview)

        # 8. 保存オプション
        save_layout = QHBoxLayout()
        self.save_json = QCheckBox("計画 JSON 保存")
        self.save_json.setChecked(True)
        self.save_svg_chk = QCheckBox("SVG 保存")
        self.save_svg_chk.setChecked(True)
        save_layout.addWidget(self.save_json)
        save_layout.addWidget(self.save_svg_chk)
        root_layout.addLayout(save_layout)

        # 9. 描画・停止ボタン
        self.run_btn = QPushButton("🎨 AIストロークを描画")
        self.stop_btn = QPushButton("⏹ 停止")
        self.stop_btn.setEnabled(False)
        btn_layout = QHBoxLayout()
        btn_layout.addWidget(self.run_btn)
        btn_layout.addWidget(self.stop_btn)
        root_layout.addLayout(btn_layout)

        # 10. プログレスバー & ステータス
        self.progress = QProgressBar()
        self.progress.setRange(0, 1)
        self.progress.setValue(0)
        root_layout.addWidget(self.progress)
        self.status = QLabel("待機中: プロンプトまたはプリセットを選んで描画を開始してください")
        self.status.setWordWrap(True)
        root_layout.addWidget(self.status)

        root_layout.addStretch(1)
        self.setWidget(container)

        # イベント接続
        self.run_btn.clicked.connect(self.run)
        self.stop_btn.clicked.connect(self.cancel)
        self.planner_mode.currentIndexChanged.connect(self._update_planner_settings_state)
        self._update_planner_settings_state()

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

    def _select_reference_image(self) -> None:
        file_path, _ = QFileDialog.getOpenFileName(
            self, "参照画像を開く", "", "画像ファイル (*.png *.jpg *.jpeg *.webp *.bmp)"
        )
        if file_path and Path(file_path).is_file():
            try:
                self._image_bytes = Path(file_path).read_bytes()
                self.image_status_label.setText(Path(file_path).name)
                self.clear_image_btn.setEnabled(True)
            except Exception as exc:
                QMessageBox.critical(self, "エラー", f"画像を読み込めませんでした: {exc}")

    def _clear_reference_image(self) -> None:
        self._image_bytes = None
        self.image_status_label.setText("画像なし")
        self.clear_image_btn.setEnabled(False)

    def _test_api_connection(self) -> None:
        try:
            planner = OpenAICompatiblePlanner(
                OpenAICompatibleSettings(
                    base_url=self.base_url.text(),
                    model=self.model.text(),
                    api_key=self.api_key.text() or os.environ.get("OPENAI_API_KEY", ""),
                    timeout_seconds=10.0,
                )
            )
            msg = planner.test_connection()
            QMessageBox.information(self, "API 接続テスト", msg)
        except Exception as exc:
            QMessageBox.critical(self, "接続テスト失敗", str(exc))

    def _update_planner_settings_state(self, *_args: Any) -> None:
        self.llm_settings.setEnabled(self.planner_mode.currentData() == "openai_compatible")

    def _planner(self) -> PlannerPort:
        if self.planner_mode.currentData() == "offline":
            return self.planner
        return OpenAICompatiblePlanner(
            OpenAICompatibleSettings(
                base_url=self.base_url.text(),
                model=self.model.text(),
                api_key=self.api_key.text() or os.environ.get("OPENAI_API_KEY", ""),
            )
        )

    def is_cancelled(self) -> bool:
        return self._cancel

    def cancel(self) -> None:
        self._cancel = True
        if self._worker is not None and hasattr(self._worker, "cancel"):
            self._worker.cancel()
        self.status.setText("停止要求を受け付けました。現在の処理完了後に停止します。")

    def run(self) -> None:
        document: Any | None = Krita.instance().activeDocument()
        if document is None:
            QMessageBox.warning(
                self, "AI Stroke Painter", "先にドキュメントを開いてください。描画先キャンバスがありません。"
            )
            return

        self._active_doc = document
        self._cancel = False
        self.run_btn.setEnabled(False)
        self.stop_btn.setEnabled(True)
        self.progress.setRange(0, 0)

        max_iters = self.iterations.value() if self.auto_refine.isChecked() else 1
        palette = self.palette_combo.currentData() or "anime"

        self.status.setText("描画計画を生成中… 停止できます。")

        try:
            planner = self._planner()
            worker = PlanWorker(
                planner=planner,
                canvas_port=self.canvas_port,
                document=document,
                prompt=self.prompt.toPlainText().strip(),
                seed=self.seed.value(),
                count=self.count.value(),
                width=float(document.width()),
                height=float(document.height()),
                image_data=self._image_bytes,
                max_iterations=max_iters,
                palette_name=palette,
                parent=self,
            )
            self._worker = worker
            worker.plan_ready.connect(self._on_plan_ready)
            worker.iteration_progress.connect(self._on_iteration_progress)
            worker.plan_failed.connect(self._on_plan_failed)
            worker.finished.connect(self._reset_run_state)
            worker.start()
        except Exception as exc:
            self.status.setText(f"エラー: {exc}")
            QMessageBox.critical(self, "AI Stroke Painter", str(exc))
            self._reset_run_state()

    def _on_iteration_progress(self, current: int, total: int, msg: str) -> None:
        self.status.setText(msg)

    def _on_plan_ready(self, plan: DrawingPlan) -> None:
        self._last_plan = plan
        self.preview.set_plan(plan)

        document = self._active_doc or Krita.instance().activeDocument()
        if document is None or self.is_cancelled():
            return

        try:
            paths = []
            if self.save_json.isChecked():
                paths.append(str(save_plan(plan)))
            if self.save_svg_chk.isChecked():
                paths.append(str(save_svg(plan)))

            rendered = self.canvas_port.render(document, plan, self.is_cancelled)
            suffix = f" ({', '.join(paths)})" if paths else ""
            if self.is_cancelled():
                self.status.setText(f"{rendered}本を描画して停止しました{suffix}")
            else:
                self.status.setText(f"描画完了: {rendered}本を生成しました{suffix}")
        except Exception as exc:
            self.status.setText(f"描画エラー: {exc}")
        finally:
            if self._worker is None or not self._worker.isRunning():
                self._reset_run_state()

    def _on_plan_failed(self, error_msg: str) -> None:
        self._worker = None
        if not self.is_cancelled():
            self.status.setText(f"エラー: {error_msg}")
            QMessageBox.critical(self, "AI Stroke Painter", error_msg)
        self._reset_run_state()

    def _reset_run_state(self) -> None:
        self.progress.setRange(0, 1)
        self.progress.setValue(1)
        self.run_btn.setEnabled(True)
        self.stop_btn.setEnabled(False)
        self._active_doc = None
        self._worker = None
