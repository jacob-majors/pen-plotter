from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtWidgets import (
    QComboBox,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QSlider,
    QVBoxLayout,
    QWidget,
)

from cv_controller.core.switches import MOUSE_SOURCES


class MouseControlPanel(QWidget):
    """Panel for choosing which body part drives the mouse cursor."""

    source_changed = pyqtSignal(str)
    sensitivity_changed = pyqtSignal(float)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setStyleSheet("color: #ccc;")

        root = QVBoxLayout(self)
        root.setContentsMargins(8, 8, 8, 8)
        root.setSpacing(10)

        title = QLabel("Mouse Control")
        title.setStyleSheet("font-size: 13px; font-weight: bold; color: #3dff7a;")
        root.addWidget(title)

        desc = QLabel(
            "Choose a body part to move the mouse cursor.\n"
            "Starter controls: open palm -> left click, blink -> right click."
        )
        desc.setWordWrap(True)
        desc.setStyleSheet("color: #888; font-size: 11px;")
        root.addWidget(desc)

        src_box = QGroupBox("Body Part")
        src_box.setStyleSheet(
            "QGroupBox { color: #aaa; border: 1px solid #333; border-radius: 4px; "
            "margin-top: 6px; padding: 6px; }"
        )
        src_layout = QVBoxLayout(src_box)

        self.source_combo = QComboBox()
        for key, label in MOUSE_SOURCES.items():
            self.source_combo.addItem(label, key)
        self.source_combo.currentIndexChanged.connect(self._on_source_changed)
        src_layout.addWidget(self.source_combo)

        self._status_label = QLabel("Status: Select a body part")
        self._status_label.setStyleSheet("color: #d8b24c; font-size: 11px;")
        src_layout.addWidget(self._status_label)

        self._toggle_label = QLabel("Mouse toggle: Thumbs up")
        self._toggle_label.setStyleSheet("color: #777; font-size: 10px;")
        src_layout.addWidget(self._toggle_label)

        root.addWidget(src_box)

        sens_box = QGroupBox("Sensitivity")
        sens_box.setStyleSheet(
            "QGroupBox { color: #aaa; border: 1px solid #333; border-radius: 4px; "
            "margin-top: 6px; padding: 6px; }"
        )
        sens_layout = QHBoxLayout(sens_box)

        self.sens_slider = QSlider(Qt.Orientation.Horizontal)
        self.sens_slider.setRange(10, 60)
        self.sens_slider.setValue(25)
        self.sens_slider.valueChanged.connect(self._on_sensitivity_changed)
        sens_layout.addWidget(self.sens_slider)

        self._sens_label = QLabel("1.0x")
        self._sens_label.setFixedWidth(36)
        sens_layout.addWidget(self._sens_label)

        root.addWidget(sens_box)

        tips = QLabel(
            "Tips:\n"
            "- Nose Tip: smooth and centered\n"
            "- Forehead: steadier, less sensitive\n"
            "- Hand (wrist): broad movement\n"
            "- Index Fingertip: precise pointing\n"
            "- Head Center: balanced tracking"
        )
        tips.setWordWrap(True)
        tips.setStyleSheet("color: #555; font-size: 10px;")
        root.addWidget(tips)

        root.addStretch()

    def _on_source_changed(self):
        self.set_active(False)
        self.source_changed.emit(self.get_source())

    def _on_sensitivity_changed(self, value: int):
        sens = value / 10.0
        self._sens_label.setText(f"{sens:.1f}x")
        self.sensitivity_changed.emit(sens)

    def set_active(self, active: bool):
        source = self.get_source()
        if source == "none":
            self._status_label.setText("Status: Select a body part")
            self._status_label.setStyleSheet("color: #d8b24c; font-size: 11px;")
            self._toggle_label.setText("Mouse toggle: Select a body part first")
            self._toggle_label.setStyleSheet("color: #777; font-size: 10px;")
            return

        label = self.source_combo.currentText()
        if active:
            self._status_label.setText(f"Status: Active - {label}")
            self._status_label.setStyleSheet("color: #3dff7a; font-size: 11px;")
            self._toggle_label.setText("Mouse toggle: Thumbs up to pause")
            self._toggle_label.setStyleSheet("color: #777; font-size: 10px;")
        else:
            self._status_label.setText(f"Status: Ready - {label}")
            self._status_label.setStyleSheet("color: #d8b24c; font-size: 11px;")
            self._toggle_label.setText("Mouse toggle: Start tracking, then thumbs up to pause or resume")
            self._toggle_label.setStyleSheet("color: #777; font-size: 10px;")

    def set_mouse_enabled(self, enabled: bool):
        source = self.get_source()
        if source == "none":
            self._status_label.setText("Status: Select a body part")
            self._status_label.setStyleSheet("color: #d8b24c; font-size: 11px;")
            self._toggle_label.setText("Mouse toggle: Select a body part first")
            self._toggle_label.setStyleSheet("color: #777; font-size: 10px;")
            return

        label = self.source_combo.currentText()
        if enabled:
            self._status_label.setText(f"Status: Active - {label}")
            self._status_label.setStyleSheet("color: #3dff7a; font-size: 11px;")
            self._toggle_label.setText("Mouse toggle: Thumbs up to pause")
            self._toggle_label.setStyleSheet("color: #777; font-size: 10px;")
        else:
            self._status_label.setText(f"Status: Paused - {label}")
            self._status_label.setStyleSheet("color: #d8b24c; font-size: 11px;")
            self._toggle_label.setText("Mouse toggle: Thumbs up to resume")
            self._toggle_label.setStyleSheet("color: #d8b24c; font-size: 10px;")

    def get_source(self) -> str:
        return self.source_combo.currentData()

    def get_sensitivity(self) -> float:
        return self.sens_slider.value() / 10.0

    def set_source(self, source: str):
        index = self.source_combo.findData(source)
        if index >= 0:
            self.source_combo.setCurrentIndex(index)

    def set_sensitivity(self, sensitivity: float):
        value = max(10, min(60, int(round(sensitivity * 10))))
        self.sens_slider.setValue(value)
