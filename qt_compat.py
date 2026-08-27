"""PyQt5 / PyQt6 の透過的インポートおよび非 GUI・ヘッドレス環境用スタブを一元管理する互換モジュール。"""

from __future__ import annotations

import importlib
import sys
from typing import Any

HAS_QT: bool = False
QT_BINDING: str | None = None

# 各種 Qt クラスの初期値
Qt: Any = None
QObject: Any = None
pyqtSignal: Any = None
QEvent: Any = None
QWidget: Any = None
QLabel: Any = None
QPushButton: Any = None
QLineEdit: Any = None
QPlainTextEdit: Any = None
QSpinBox: Any = None
QDoubleSpinBox: Any = None
QSlider: Any = None
QScrollArea: Any = None
QTabWidget: Any = None
QInputDialog: Any = None
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
QPainterPath: Any = None
QColor: Any = None
QPen: Any = None
QBrush: Any = None
QImage: Any = None
QPoint: Any = None
QPointF: Any = None
QByteArray: Any = None
QBuffer: Any = None
QIODevice: Any = None
QSettings: Any = None


def password_echo_mode(line_edit_cls: Any | None = None) -> Any:
    """Return QLineEdit's password mode for either supported Qt binding.

    PyQt5 exposes ``QLineEdit.Password`` while PyQt6 scopes the value under
    ``QLineEdit.EchoMode.Password``. Failing to resolve either must not leave
    a credential widget in the default visible-text mode.
    """
    cls = QLineEdit if line_edit_cls is None else line_edit_cls
    scoped_mode = getattr(cls, "EchoMode", None)
    scoped_password = getattr(scoped_mode, "Password", None)
    if scoped_password is not None:
        return scoped_password

    legacy_password = getattr(cls, "Password", None)
    if legacy_password is not None:
        return legacy_password

    raise RuntimeError("QLineEdit のパスワード表示モードを取得できません")


def write_only_open_mode(io_device_cls: Any | None = None) -> Any:
    """Return QIODevice's write-only flag for Qt 5 or Qt 6."""
    cls = QIODevice if io_device_cls is None else io_device_cls
    scoped = getattr(cls, "OpenModeFlag", None)
    scoped_value = getattr(scoped, "WriteOnly", None)
    if scoped_value is not None:
        return scoped_value
    legacy_value = getattr(cls, "WriteOnly", None)
    if legacy_value is not None:
        return legacy_value
    raise RuntimeError("QIODevice の WriteOnly モードを取得できません")


def argb32_image_format(image_cls: Any | None = None) -> Any:
    """Return QImage's ARGB32 format for Qt 5 or Qt 6."""
    cls = QImage if image_cls is None else image_cls
    scoped = getattr(cls, "Format", None)
    scoped_value = getattr(scoped, "Format_ARGB32", None)
    if scoped_value is not None:
        return scoped_value
    legacy_value = getattr(cls, "Format_ARGB32", None)
    if legacy_value is not None:
        return legacy_value
    return 4


def round_cap_style(qt_cls: Any | None = None) -> Any:
    """Return Qt.RoundCap / Qt.PenCapStyle.RoundCap for Qt 5 or Qt 6."""
    target_qt = Qt if qt_cls is None else qt_cls
    if target_qt is None:
        return 0x20  # Qt.RoundCap default enum value
    scoped = getattr(target_qt, "PenCapStyle", None)
    scoped_val = getattr(scoped, "RoundCap", None)
    if scoped_val is not None:
        return scoped_val
    legacy_val = getattr(target_qt, "RoundCap", None)
    if legacy_val is not None:
        return legacy_val
    return 0x20


def round_join_style(qt_cls: Any | None = None) -> Any:
    """Return Qt.RoundJoin / Qt.PenJoinStyle.RoundJoin for Qt 5 or Qt 6."""
    target_qt = Qt if qt_cls is None else qt_cls
    if target_qt is None:
        return 0x40  # Qt.RoundJoin default enum value
    scoped = getattr(target_qt, "PenJoinStyle", None)
    scoped_val = getattr(scoped, "RoundJoin", None)
    if scoped_val is not None:
        return scoped_val
    legacy_val = getattr(target_qt, "RoundJoin", None)
    if legacy_val is not None:
        return legacy_val
    return 0x40


def antialiasing_render_hint(painter_cls: Any | None = None) -> Any:
    """Return QPainter.Antialiasing / QPainter.RenderHint.Antialiasing for Qt 5 or Qt 6."""
    target_painter = QPainter if painter_cls is None else painter_cls
    if target_painter is None:
        return 0x01  # QPainter.Antialiasing default enum value
    scoped = getattr(target_painter, "RenderHint", None)
    scoped_val = getattr(scoped, "Antialiasing", None)
    if scoped_val is not None:
        return scoped_val
    legacy_val = getattr(target_painter, "Antialiasing", None)
    if legacy_val is not None:
        return legacy_val
    return 0x01


def composition_mode_source_over(painter_cls: Any | None = None) -> Any:
    """Return QPainter.CompositionMode_SourceOver for Qt 5 or Qt 6."""
    target_painter = QPainter if painter_cls is None else painter_cls
    if target_painter is None:
        return 0
    scoped = getattr(target_painter, "CompositionMode", None)
    scoped_val = getattr(scoped, "CompositionMode_SourceOver", None)
    if scoped_val is not None:
        return scoped_val
    return getattr(target_painter, "CompositionMode_SourceOver", 0)


def composition_mode_multiply(painter_cls: Any | None = None) -> Any:
    """Return QPainter.CompositionMode_Multiply for Qt 5 or Qt 6."""
    target_painter = QPainter if painter_cls is None else painter_cls
    if target_painter is None:
        return 14
    scoped = getattr(target_painter, "CompositionMode", None)
    scoped_val = getattr(scoped, "CompositionMode_Multiply", None)
    if scoped_val is not None:
        return scoped_val
    return getattr(target_painter, "CompositionMode_Multiply", 14)


def composition_mode_plus(painter_cls: Any | None = None) -> Any:
    """Return QPainter.CompositionMode_Plus for Qt 5 or Qt 6."""
    target_painter = QPainter if painter_cls is None else painter_cls
    if target_painter is None:
        return 17
    scoped = getattr(target_painter, "CompositionMode", None)
    scoped_val = getattr(scoped, "CompositionMode_Plus", None)
    if scoped_val is not None:
        return scoped_val
    return getattr(target_painter, "CompositionMode_Plus", 17)


def composition_mode_destination_out(painter_cls: Any | None = None) -> Any:
    """Return QPainter.CompositionMode_DestinationOut for Qt 5 or Qt 6."""
    target_painter = QPainter if painter_cls is None else painter_cls
    if target_painter is None:
        return 4
    scoped = getattr(target_painter, "CompositionMode", None)
    scoped_val = getattr(scoped, "CompositionMode_DestinationOut", None)
    if scoped_val is not None:
        return scoped_val
    return getattr(target_painter, "CompositionMode_DestinationOut", 4)


# Krita が既に読み込んだ Qt バインディングを最優先し、未確定時は Krita 6 の PyQt6 を先に試す。
if any(name == "PyQt5" or name.startswith("PyQt5.") for name in sys.modules):
    _binding_order = ("PyQt5", "PyQt6")
else:
    _binding_order = ("PyQt6", "PyQt5")

for binding in _binding_order:
    try:
        _core = importlib.import_module(f"{binding}.QtCore")
        _widgets = importlib.import_module(f"{binding}.QtWidgets")
        _gui = importlib.import_module(f"{binding}.QtGui")

        Qt = getattr(_core, "Qt", None)
        QObject = getattr(_core, "QObject", None)
        pyqtSignal = getattr(_core, "pyqtSignal", None)
        QEvent = getattr(_core, "QEvent", None)
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
        QDoubleSpinBox = getattr(_widgets, "QDoubleSpinBox", None)
        QSlider = getattr(_widgets, "QSlider", None)
        QScrollArea = getattr(_widgets, "QScrollArea", None)
        QTabWidget = getattr(_widgets, "QTabWidget", None)
        QInputDialog = getattr(_widgets, "QInputDialog", None)
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
        QPainterPath = getattr(_gui, "QPainterPath", None)
        QColor = getattr(_gui, "QColor", None)
        QPen = getattr(_gui, "QPen", None)
        QBrush = getattr(_gui, "QBrush", None)
        QImage = getattr(_gui, "QImage", None)

        if QObject is not None and QWidget is not None:
            HAS_QT = True
            QT_BINDING = binding
            break
    except (ImportError, AttributeError):
        continue


class _FakeSignal:
    def __init__(self, *args: Any, **kwargs: Any) -> None:
        self._slots: list[Any] = []
        self._attribute_name = ""

    def __set_name__(self, _owner: Any, name: str) -> None:
        self._attribute_name = f"__signal_{name}"

    def __get__(self, instance: Any, _owner: Any = None) -> Any:
        if instance is None or not self._attribute_name:
            return self
        signal = instance.__dict__.get(self._attribute_name)
        if signal is None:
            signal = _FakeSignal()
            instance.__dict__[self._attribute_name] = signal
        return signal

    def connect(self, slot: Any) -> None:
        self._slots.append(slot)

    def disconnect(self, slot: Any = None) -> None:
        if slot is None:
            self._slots.clear()
        else:
            self._slots = [s for s in self._slots if s != slot]

    def emit(self, *args: Any) -> None:
        for slot in list(self._slots):
            slot(*args)


# Qt が存在しない環境（CI、ヘッドレス、テスト環境等）用の完全なフォールバックスタブ
if not HAS_QT:

    def _pyqtSignal(*_args: Any) -> Any:
        return _FakeSignal()

    pyqtSignal = _pyqtSignal

    class QObject:  # type: ignore[no-redef]
        def __init__(self, *args: Any, **kwargs: Any) -> None:
            pass

    class QEvent:  # type: ignore[no-redef]
        class Type:
            MouseButtonPress = 2
            MouseButtonRelease = 3
            MouseButtonDblClick = 4
            MouseMove = 5
            KeyPress = 6
            KeyRelease = 7
            Wheel = 31
            TabletMove = 87
            TabletPress = 92
            TabletRelease = 93
            TouchBegin = 194
            TouchUpdate = 195
            TouchEnd = 196
            ShortcutOverride = 51

    class QPainterPath:  # type: ignore[no-redef]
        def __init__(self) -> None:
            self.points: list[tuple[float, float]] = []

        def moveTo(self, x: float, y: float) -> None:  # noqa: N802
            self.points = [(float(x), float(y))]

        def lineTo(self, x: float, y: float) -> None:  # noqa: N802
            self.points.append((float(x), float(y)))

    class QWidget(QObject):  # type: ignore[no-redef]
        def __init__(self, *args: Any, **kwargs: Any) -> None:
            super().__init__()
            self._visible = True
            self._enabled = True
            self._width = 200
            self._height = 160

        def setLayout(self, *args: Any) -> None:
            pass

        def update(self) -> None:
            pass

        def setFixedSize(self, *args: Any) -> None:
            pass

        def setToolTip(self, *args: Any) -> None:
            pass

        def setMinimumHeight(self, h: int) -> None:
            self._height = h

        def setMaximumHeight(self, h: int) -> None:
            self._height = h

        def setMinimumWidth(self, w: int) -> None:
            self._width = w

        def setMaximumWidth(self, w: int) -> None:
            self._width = w

        def setEnabled(self, enabled: bool) -> None:
            self._enabled = bool(enabled)

        def isEnabled(self) -> bool:
            return self._enabled

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

        def clear(self) -> None:
            self._text = ""

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
            self.valueChanged = _FakeSignal()

        def value(self) -> int:
            return self._val

        def setValue(self, v: int) -> None:
            self._val = v
            self.valueChanged.emit(self._val)

        def setRange(self, *args: Any) -> None:
            pass

        def setSingleStep(self, *args: Any) -> None:
            pass

        def setSuffix(self, *args: Any) -> None:
            pass

        def setPrefix(self, *args: Any) -> None:
            pass

        def setToolTip(self, *args: Any) -> None:
            pass

    class QDoubleSpinBox(QWidget):  # type: ignore[no-redef]
        def __init__(self, *args: Any, **kwargs: Any) -> None:
            super().__init__()
            self._val = 0.0
            self.valueChanged = _FakeSignal()

        def value(self) -> float:
            return self._val

        def setValue(self, v: float) -> None:
            self._val = float(v)
            self.valueChanged.emit(self._val)

        def setRange(self, *args: Any) -> None:
            pass

        def setSingleStep(self, *args: Any) -> None:
            pass

        def setDecimals(self, *args: Any) -> None:
            pass

        def setSuffix(self, *args: Any) -> None:
            pass

        def setPrefix(self, *args: Any) -> None:
            pass

        def setToolTip(self, *args: Any) -> None:
            pass

    class QSlider(QWidget):  # type: ignore[no-redef]
        def __init__(self, *args: Any, **kwargs: Any) -> None:
            super().__init__()
            self._val = 0
            self.valueChanged = _FakeSignal()

        def value(self) -> int:
            return self._val

        def setValue(self, v: int) -> None:
            self._val = int(v)
            self.valueChanged.emit(self._val)

        def setRange(self, *args: Any) -> None:
            pass

        def setOrientation(self, *args: Any) -> None:
            pass

        def setToolTip(self, *args: Any) -> None:
            pass

    class QScrollArea(QWidget):  # type: ignore[no-redef]
        def __init__(self, *args: Any, **kwargs: Any) -> None:
            super().__init__()

        def setWidget(self, *args: Any) -> None:
            pass

        def setWidgetResizable(self, *args: Any) -> None:
            pass

    class QTabWidget(QWidget):  # type: ignore[no-redef]
        def __init__(self, *args: Any, **kwargs: Any) -> None:
            super().__init__()
            self.currentChanged = _FakeSignal()
            self._tabs: list[tuple[QWidget, str]] = []
            self._current_index: int = 0

        def addTab(self, widget: Any, label: str = "") -> int:
            self._tabs.append((widget, label))
            return len(self._tabs) - 1

        def count(self) -> int:
            return len(self._tabs)

        def currentIndex(self) -> int:
            return self._current_index

        def setCurrentIndex(self, index: int) -> None:
            self._current_index = index
            self.currentChanged.emit(index)

        def tabText(self, index: int) -> str:
            if 0 <= index < len(self._tabs):
                return self._tabs[index][1]
            return ""

        def widget(self, index: int) -> Any:
            if 0 <= index < len(self._tabs):
                return self._tabs[index][0]
            return None

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

        def clear(self) -> None:
            self._items.clear()
            self._idx = 0

        def itemData(self, index: int) -> Any:
            if 0 <= index < len(self._items):
                return self._items[index][1]
            return None

        def itemText(self, index: int) -> str:
            if 0 <= index < len(self._items):
                return self._items[index][0]
            return ""

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
            self.currentIndexChanged.emit(i)

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
        Yes = 1
        No = 2

        class StandardButton:
            Yes = 1
            No = 2

        @staticmethod
        def information(*args: Any) -> None:
            pass

        @staticmethod
        def warning(*args: Any) -> None:
            pass

        @staticmethod
        def critical(*args: Any) -> None:
            pass

        @staticmethod
        def question(*args: Any) -> int:
            return 2

    class QFileDialog:  # type: ignore[no-redef]
        @staticmethod
        def getOpenFileName(*args: Any) -> tuple[str, str]:
            return "", ""

        @staticmethod
        def getSaveFileName(*args: Any) -> tuple[str, str]:
            return "", ""

    class QInputDialog:  # type: ignore[no-redef]
        @staticmethod
        def getText(*args: Any, **kwargs: Any) -> tuple[str, bool]:
            return "", False

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
            self.x = x
            self.y = y

    class QPointF:  # type: ignore[no-redef]
        def __init__(self, x: float, y: float) -> None:
            self.x = x
            self.y = y

    class Qt:  # type: ignore[no-redef]
        RoundCap = 0x20
        RoundJoin = 0x40

        class PenCapStyle:
            RoundCap = 0x20
            SquareCap = 0x10
            FlatCap = 0x00

        class PenJoinStyle:
            RoundJoin = 0x40
            MiterJoin = 0x00
            BevelJoin = 0x80

    class QBrush:  # type: ignore[no-redef]
        def __init__(self, *args: Any) -> None:
            pass

    class QColor:  # type: ignore[no-redef]
        def __init__(self, *args: Any) -> None:
            pass

        def setAlphaF(self, *args: Any) -> None:
            pass

        @classmethod
        def fromRgbF(cls, r: float, g: float, b: float, a: float = 1.0) -> Any:
            return cls(r, g, b, a)

    class QPen:  # type: ignore[no-redef]
        def __init__(self, *args: Any) -> None:
            pass

        def setCapStyle(self, *args: Any) -> None:
            pass

        def setJoinStyle(self, *args: Any) -> None:
            pass

        def setWidthF(self, *args: Any) -> None:
            pass

        def setWidth(self, *args: Any) -> None:
            pass

    class QPainter:  # type: ignore[no-redef]
        Antialiasing = 0x01
        CompositionMode_SourceOver = 0
        CompositionMode_DestinationOut = 4
        CompositionMode_Multiply = 14
        CompositionMode_Plus = 17

        class RenderHint:
            Antialiasing = 0x01

        class CompositionMode:
            CompositionMode_SourceOver = 0
            CompositionMode_DestinationOut = 4
            CompositionMode_Multiply = 14
            CompositionMode_Plus = 17

        def __init__(self, *args: Any) -> None:
            pass

        def setRenderHint(self, *args: Any) -> None:
            pass

        def setCompositionMode(self, *args: Any) -> None:
            pass

        def setClipRect(self, *args: Any) -> None:
            pass

        def fillRect(self, *args: Any) -> None:
            pass

        def drawRect(self, *args: Any) -> None:
            pass

        def setPen(self, *args: Any) -> None:
            pass

        def setBrush(self, *args: Any) -> None:
            pass

        def drawText(self, *args: Any) -> None:
            pass

        def drawLine(self, *args: Any) -> None:
            pass

        def drawEllipse(self, *args: Any) -> None:
            pass

        def save(self) -> None:
            pass

        def restore(self) -> None:
            pass

        def drawImage(self, *args: Any) -> None:
            pass

        def begin(self, *args: Any) -> bool:
            return True

        def end(self) -> None:
            pass

    class QImage:  # type: ignore[no-redef]
        Format_ARGB32 = 4

        class Format:
            Format_ARGB32 = 4

        def __init__(self, *args: Any) -> None:
            pass

        def fill(self, *args: Any) -> None:
            pass

        def loadFromData(self, *args: Any) -> bool:
            return False

        def save(self, *args: Any) -> bool:
            return True

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
