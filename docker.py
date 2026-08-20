try:
    from PyQt5.QtWidgets import (
        QCheckBox,
        QHBoxLayout,
        QLabel,
        QMessageBox,
        QPlainTextEdit,
        QProgressBar,
        QPushButton,
        QSpinBox,
        QVBoxLayout,
        QWidget,
    )
except ImportError:
    from PyQt6.QtWidgets import (
        QCheckBox,
        QHBoxLayout,
        QLabel,
        QMessageBox,
        QPlainTextEdit,
        QProgressBar,
        QPushButton,
        QSpinBox,
        QVBoxLayout,
        QWidget,
    )

from krita import DockWidget, Krita

from .krita_adapter import KritaCanvasAdapter
from .planner import RuleBasedPlanner
from .storage import save_plan


class AIStrokePainterDocker(DockWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("AI Stroke Painter MVP")
        self.planner = RuleBasedPlanner()
        self.canvas_port = KritaCanvasAdapter()
        self._cancel = False

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

    def canvasChanged(self, canvas):
        pass

    def cancel(self):
        self._cancel = True
        self.status.setText("停止要求を受け付けました。現在の線分を完了後に停止します。")

    def run(self):
        document = Krita.instance().activeDocument()
        if document is None:
            QMessageBox.warning(self, "AI Stroke Painter", "先にドキュメントを開いてください。描画先キャンバスがありません。")
            return

        self._cancel = False
        self.run_btn.setEnabled(False)
        self.stop_btn.setEnabled(True)
        self.progress.setRange(0, 0)
        try:
            plan = self.planner.plan(
                self.prompt.toPlainText().strip(),
                self.seed.value(),
                self.count.value(),
                document.width(),
                document.height(),
            )
            path = save_plan(plan) if self.save_json.isChecked() else None
            self.status.setText("描画中… 停止できます。")
            rendered = self.canvas_port.render(document, plan, lambda: self._cancel)
            suffix = (" / " + str(path)) if path else ""
            if self._cancel:
                self.status.setText("%d本を描画して停止しました%s" % (rendered, suffix))
            else:
                self.status.setText("%d本を描画しました%s" % (rendered, suffix))
        except Exception as exc:
            self.status.setText("エラー: " + str(exc))
            QMessageBox.critical(self, "AI Stroke Painter", str(exc))
        finally:
            self.progress.setRange(0, 1)
            self.progress.setValue(1)
            self.run_btn.setEnabled(True)
            self.stop_btn.setEnabled(False)
