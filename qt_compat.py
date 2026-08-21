"""PyQt5 / PyQt6 の透過的インポートおよび非 GUI・ヘッドレス環境用スタブを一元管理する互換モジュール。"""

from __future__ import annotations

import importlib
from typing import Any

HAS_QT: bool = False
QT_BINDING: str | None = None

# 各種 Qt クラスの初期値
QObject: Any = None
pyqtSignal: Any = None
QWidget: Any = None
QLabel: Any = None
QPushButton: Any = None
QLineEdit: Any = None
QPlainTextEdit: Any = None
QSpinBox: Any = None
QCheckBox: Any = None
QComboBox: Any = None
QProgressBar: Any = None
QGroupBox: Any = None
QVBoxLayout: Any = None
QHBoxLayout: Any = None
QFormLayout: Any = None
QMessageBox: Any = None
QFileDialog: Any = None
QApplication: Any = None
QPainter: Any = None
QColor: Any = None
QPen: Any = None
QImage: Any = None
QPoint: Any = None
QPointF: Any = None
QByteArray: Any = None
QBuffer: Any = None
QIODevice: Any = None
QSettings: Any = None

# PyQt5 または PyQt6 のインポートを試行
for binding in ("PyQt5", "PyQt6"):
    try:
        _core = importlib.import_module(f"{binding}.QtCore")
        _widgets = importlib.import_module(f"{binding}.QtWidgets")
        _gui = importlib.import_module(f"{binding}.QtGui")

        QObject = getattr(_core, "QObject", None)
        pyqtSignal = getattr(_core, "pyqtSignal", None)
        QPoint = getattr(_core, "QPoint", None)
        QPointF = getattr(_core, "QPointF", None)
        QByteArray = getattr(_core, "QByteArray", None)
        QBuffer = getattr(_core, "QBuffer", None)
        QIODevice = getattr(_core, "QIODevice", None)
        QSettings = getattr(_core, "QSettings", None)

        QWidget = getattr(_widgets, "QWidget", None)
        QLabel = getattr(_widgets, "QLabel", None)
        QPushButton = getattr(_widgets, "QPushButton", None)
        QLineEdit = getattr(_widgets, "QLineEdit", None)
        QPlainTextEdit = getattr(_widgets, "QPlainTextEdit", None)
        QSpinBox = getattr(_widgets, "QSpinBox", None)
        QCheckBox = getattr(_widgets, "QCheckBox", None)
        QComboBox = getattr(_widgets, "QComboBox", None)
        QProgressBar = getattr(_widgets, "QProgressBar", None)
        QGroupBox = getattr(_widgets, "QGroupBox", None)
        QVBoxLayout = getattr(_widgets, "QVBoxLayout", None)
        QHBoxLayout = getattr(_widgets, "QHBoxLayout", None)
        QFormLayout = getattr(_widgets, "QFormLayout", None)
        QMessageBox = getattr(_widgets, "QMessageBox", None)
        QFileDialog = getattr(_widgets, "QFileDialog", None)
        QApplication = getattr(_widgets, "QApplication", None)

        QPainter = getattr(_gui, "QPainter", None)
        QColor = getattr(_gui, "QColor", None)
        QPen = getattr(_gui, "QPen", None)
        QImage = getattr(_gui, "QImage", None)

        if QObject is not None and QWidget is not None:
            HAS_QT = True
            QT_BINDING = binding
            break
    except (ImportError, AttributeError):
        continue

# Qt が存在しない環境（CI、ヘッドレス、テスト環境等）用の完全なフォールバックスタブ
if not HAS_QT:

    class _FakeSignal:
        def __init__(self, *args: Any, **kwargs: Any) -> None:
            self._slots: list[Any] = []

        def connect(self, slot: Any) -> None:
            self._slots.append(slot)

        def emit(self, *args: Any) -> None:
            for slot in list(self._slots):
                slot(*args)

    def _pyqtSignal(*_args: Any) -> Any:
        return _FakeSignal()

    pyqtSignal = _pyqtSignal

    class QObject:  # type: ignore[no-redef]
        def __init__(self, *args: Any, **kwargs: Any) -> None:
            pass

    class QWidget(QObject):  # type: ignore[no-redef]
        def __init__(self, *args: Any, **kwargs: Any) -> None:
            super().__init__()
            self._visible = True
            self._width = 200
            self._height = 160

        def setLayout(self, *args: Any) -> None:
            pass

        def update(self) -> None:
            pass

        def setFixedSize(self, *args: Any) -> None:
            pass

        def setMinimumHeight(self, h: int) -> None:
            self._height = h

        def setMaximumHeight(self, h: int) -> None:
            self._height = h

        def setEnabled(self, *args: Any) -> None:
            pass

        def setVisible(self, visible: bool) -> None:
            self._visible = visible

        def isVisible(self) -> bool:
            return self._visible

        def width(self) -> int:
            return self._width

        def height(self) -> int:
            return self._height

    class QLabel(QWidget):  # type: ignore[no-redef]
        def __init__(self, text: str = "", *args: Any, **kwargs: Any) -> None:
            super().__init__()
            self._text = text

        def setText(self, text: str) -> None:
            self._text = text

        def text(self) -> str:
            return self._text

        def setWordWrap(self, *args: Any) -> None:
            pass

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

        def setPlaceholderText(self, *args: Any) -> None:
            pass

        def setEchoMode(self, *args: Any) -> None:
            pass

    class QPlainTextEdit(QWidget):  # type: ignore[no-redef]
        def __init__(self, text: str = "", *args: Any, **kwargs: Any) -> None:
            super().__init__()
            self._text = text

        def toPlainText(self) -> str:
            return self._text

        def setPlainText(self, text: str) -> None:
            self._text = text

        def appendPlainText(self, text: str) -> None:
            self._text += ("\n" if self._text else "") + text

        def clear(self) -> None:
            self._text = ""

        def setReadOnly(self, *args: Any) -> None:
            pass

    class QSpinBox(QWidget):  # type: ignore[no-redef]
        def __init__(self, *args: Any, **kwargs: Any) -> None:
            super().__init__()
            self._val = 0

        def value(self) -> int:
            return self._val

        def setValue(self, v: int) -> None:
            self._val = v

        def setRange(self, *args: Any) -> None:
            pass

        def setSuffix(self, *args: Any) -> None:
            pass

    class QCheckBox(QWidget):  # type: ignore[no-redef]
        def __init__(self, text: str = "", *args: Any, **kwargs: Any) -> None:
            super().__init__()
            self._checked = False
            self.toggled = _FakeSignal()

        def isChecked(self) -> bool:
            return self._checked

        def setChecked(self, c: bool) -> None:
            self._checked = c
            self.toggled.emit(c)

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

        def currentIndex(self) -> int:
            return self._idx

        def setCurrentIndex(self, i: int) -> None:
            self._idx = i

    class QProgressBar(QWidget):  # type: ignore[no-redef]
        def __init__(self, *args: Any, **kwargs: Any) -> None:
            super().__init__()
            self._val = 0

        def setValue(self, v: int) -> None:
            self._val = v

        def setRange(self, *args: Any) -> None:
            pass

    class QGroupBox(QWidget):  # type: ignore[no-redef]
        def __init__(self, title: str = "", *args: Any, **kwargs: Any) -> None:
            super().__init__()

    class QVBoxLayout:  # type: ignore[no-redef]
        def __init__(self, *args: Any, **kwargs: Any) -> None:
            pass

        def addWidget(self, *args: Any) -> None:
            pass

        def addLayout(self, *args: Any) -> None:
            pass

        def addStretch(self, *args: Any) -> None:
            pass

    class QHBoxLayout:  # type: ignore[no-redef]
        def __init__(self, *args: Any, **kwargs: Any) -> None:
            pass

        def addWidget(self, *args: Any) -> None:
            pass

        def addLayout(self, *args: Any) -> None:
            pass

        def addStretch(self, *args: Any) -> None:
            pass

    class QFormLayout:  # type: ignore[no-redef]
        def __init__(self, *args: Any, **kwargs: Any) -> None:
            pass

        def addRow(self, *args: Any) -> None:
            pass

    class QMessageBox:  # type: ignore[no-redef]
        @staticmethod
        def information(*args: Any) -> None:
            pass

        @staticmethod
        def warning(*args: Any) -> None:
            pass

        @staticmethod
        def critical(*args: Any) -> None:
            pass

    class QFileDialog:  # type: ignore[no-redef]
        @staticmethod
        def getOpenFileName(*args: Any) -> tuple[str, str]:
            return "", ""

        @staticmethod
        def getSaveFileName(*args: Any) -> tuple[str, str]:
            return "", ""

    class _FakeClipboard:
        def __init__(self) -> None:
            self._text = ""

        def setText(self, text: str) -> None:
            self._text = text

        def text(self) -> str:
            return self._text

    class QApplication:  # type: ignore[no-redef]
        _clip = _FakeClipboard()

        @classmethod
        def clipboard(cls) -> Any:
            return cls._clip

        @classmethod
        def processEvents(cls) -> None:
            pass

        @classmethod
        def instance(cls) -> Any:
            return None

    class QPoint:  # type: ignore[no-redef]
        def __init__(self, x: int, y: int) -> None:
            self.x = int(x)
            self.y = int(y)

    class QPointF:  # type: ignore[no-redef]
        def __init__(self, x: float, y: float) -> None:
            self.x = float(x)
            self.y = float(y)

    class QColor:  # type: ignore[no-redef]
        def __init__(self, *args: Any) -> None:
            pass

        def setAlphaF(self, *args: Any) -> None:
            pass

        @staticmethod
        def fromRgbF(r: float, g: float, b: float, a: float = 1.0) -> Any:
            return QColor()

    class QPen:  # type: ignore[no-redef]
        def __init__(self, *args: Any) -> None:
            pass

    class QPainter:  # type: ignore[no-redef]
        def __init__(self, *args: Any) -> None:
            pass

        def fillRect(self, *args: Any) -> None:
            pass

        def setPen(self, *args: Any) -> None:
            pass

        def drawText(self, *args: Any) -> None:
            pass

        def drawLine(self, *args: Any) -> None:
            pass

        def end(self) -> None:
            pass

    class QImage:  # type: ignore[no-redef]
        def __init__(self, *args: Any) -> None:
            pass

        def loadFromData(self, *args: Any) -> bool:
            return False

        def width(self) -> int:
            return 0

        def height(self) -> int:
            return 0

    class QByteArray:  # type: ignore[no-redef]
        def __init__(self, *args: Any) -> None:
            pass

        def data(self) -> bytes:
            return b""

    class QBuffer:  # type: ignore[no-redef]
        def __init__(self, *args: Any) -> None:
            pass

        def open(self, *args: Any) -> bool:
            return True

    class QIODevice:  # type: ignore[no-redef]
        WriteOnly = 2
        ReadOnly = 1

    class QSettings:  # type: ignore[no-redef]
        _storage: dict[str, Any] = {}

        def __init__(self, *args: Any) -> None:
            pass

        def value(self, key: str, defaultValue: Any = None) -> Any:  # noqa: N803
            return self._storage.get(key, defaultValue)

        def setValue(self, key: str, value: Any) -> None:  # noqa: N802
            self._storage[key] = value

        def clear(self) -> None:
            self._storage.clear()

        def sync(self) -> None:
            pass
