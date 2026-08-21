from __future__ import annotations

import os
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from PyQt5.QtWidgets import (
        QCheckBox,
        QComboBox,
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
        from PyQt5.QtWidgets import (
            QCheckBox,
            QComboBox,
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
        import contextlib

        with contextlib.suppress(ImportError):
            from PyQt6.QtWidgets import (
                QCheckBox,
                QComboBox,
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

try:
    from krita import DockWidget, Krita
except ImportError:
    # Krita 外での型チェック / スタブ用フォールバック
    class DockWidget:  # type: ignore[no-redef]
        def __init__(self) -> None: ...
        def setWindowTitle(self, title: str) -> None: ...
        def setWidget(self, widget: Any) -> None: ...

    class Krita:  # type: ignore[no-redef]
        @staticmethod
        def instance() -> Any: ...


from .krita_adapter import KritaCanvasAdapter
from .llm_planner import OpenAICompatiblePlanner, OpenAICompatibleSettings
from .planner import RuleBasedPlanner
from .ports import PlannerPort
from .storage import save_plan


class AIStrokePainterDocker(DockWidget):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("AI Stroke Painter MVP")
        self.planner = RuleBasedPlanner()
        self.canvas_port = KritaCanvasAdapter()
        self._cancel: bool = False

        container = QWidget(self)
        layout = QVBoxLayout(container)
        layout.addWidget(QLabel("描画指示（MVP は「髪」または「S字」を S字ストロークとして解釈）"))
        self.prompt = QPlainTextEdit("髪の毛のようなS字線を描く")
        self.prompt.setMaximumHeight(90)
        layout.addWidget(self.prompt)

        controls = QHBoxLayout()
        controls.addWidget(QLabel("Seed"))
        self.seed = QSpinBox()
        self.seed.setRange(0, 2147483647)
        self.seed.setValue(42)
        controls.addWidget(self.seed)
        controls.addWidget(QLabel("本数"))
        self.count = QSpinBox()
        self.count.setRange(1, 50)
        self.count.setValue(10)
        controls.addWidget(self.count)
        layout.addLayout(controls)

        self.planner_mode = QComboBox()
        self.planner_mode.addItem("オフライン（ルールベース）", "offline")
        self.planner_mode.addItem("OpenAI 互換 LLM", "openai_compatible")
        layout.addWidget(QLabel("Planner"))
        layout.addWidget(self.planner_mode)

        self.llm_settings = QGroupBox("OpenAI 互換 API 設定（API キーは保存しません）")
        llm_form = QFormLayout(self.llm_settings)
        self.base_url = QLineEdit("https://api.openai.com/v1")
        self.base_url.setPlaceholderText("例: https://api.openai.com/v1")
        llm_form.addRow("Base URL", self.base_url)
        self.model = QLineEdit()
        self.model.setPlaceholderText("例: 使用する Chat Completions 対応モデル")
        llm_form.addRow("Model", self.model)
        self.api_key = QLineEdit()
        try:
            self.api_key.setEchoMode(QLineEdit.Password)
        except AttributeError:
            self.api_key.setEchoMode(QLineEdit.EchoMode.Password)
        self.api_key.setPlaceholderText("空欄なら OPENAI_API_KEY")
        llm_form.addRow("API Key", self.api_key)
        layout.addWidget(self.llm_settings)

        self.save_json = QCheckBox("計画JSONを保存")
        self.save_json.setChecked(True)
        layout.addWidget(self.save_json)

        self.run_btn = QPushButton("AIストロークを描画")
        self.stop_btn = QPushButton("停止")
        self.stop_btn.setEnabled(False)
        buttons = QHBoxLayout()
        buttons.addWidget(self.run_btn)
        buttons.addWidget(self.stop_btn)
        layout.addLayout(buttons)

        self.progress = QProgressBar()
        self.progress.setRange(0, 1)
        self.progress.setValue(0)
        layout.addWidget(self.progress)
        self.status = QLabel("待機中（現在のブラシと前景色で描画します）")
        self.status.setWordWrap(True)
        layout.addWidget(self.status)
        layout.addStretch(1)
        self.setWidget(container)

        self.run_btn.clicked.connect(self.run)
        self.stop_btn.clicked.connect(self.cancel)
        self.planner_mode.currentIndexChanged.connect(self._update_planner_settings_state)
        self._update_planner_settings_state()

    def canvasChanged(self, canvas: Any) -> None:
        pass

    def cancel(self) -> None:
        self._cancel = True
        self.status.setText("停止要求を受け付けました。現在の線分を完了後に停止します。")

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

    def run(self) -> None:
        document: Any | None = Krita.instance().activeDocument()
        if document is None:
            QMessageBox.warning(
                self, "AI Stroke Painter", "先にドキュメントを開いてください。描画先キャンバスがありません。"
            )
            return

        self._cancel = False
        self.run_btn.setEnabled(False)
        self.stop_btn.setEnabled(True)
        self.progress.setRange(0, 0)
        try:
            planner = self._planner()
            if self.planner_mode.currentData() == "openai_compatible":
                self.status.setText("LLM に描画計画を問い合わせ中…")
            plan = planner.plan(
                self.prompt.toPlainText().strip(),
                self.seed.value(),
                self.count.value(),
                float(document.width()),
                float(document.height()),
            )
            path = save_plan(plan) if self.save_json.isChecked() else None
            self.status.setText("描画中… 停止できます。")
            rendered = self.canvas_port.render(document, plan, lambda: self._cancel)
            suffix = f" / {path}" if path else ""
            if self._cancel:
                self.status.setText(f"{rendered}本を描画して停止しました{suffix}")
            else:
                self.status.setText(f"{rendered}本を描画しました{suffix}")
        except Exception as exc:
            self.status.setText(f"エラー: {exc}")
            QMessageBox.critical(self, "AI Stroke Painter", str(exc))
        finally:
            self.progress.setRange(0, 1)
            self.progress.setValue(1)
            self.run_btn.setEnabled(True)
            self.stop_btn.setEnabled(False)
