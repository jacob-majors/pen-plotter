from PyQt6.QtCore import Qt, pyqtSignal, QTimer
from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QLabel, QProgressBar,
    QCheckBox, QPushButton, QScrollArea, QFrame, QSizePolicy,
)
from cv_controller.core.switches import SwitchDefinition, MOVEMENT_LABELS, ACTION_LABELS

_ROW_STYLE = """
    QFrame#switchRow {{
        background: {bg};
        border: 1px solid {border};
        border-radius: 8px;
        margin: 2px 4px;
    }}
"""
_IDLE_BG     = "#252525"
_IDLE_BORDER = "#383838"
_FLASH_BG    = "#1a3d2a"
_FLASH_BORDER = "#3d9f5e"


class SwitchRow(QFrame):
    edit_requested   = pyqtSignal(str)
    delete_requested = pyqtSignal(str)

    def __init__(self, sw: SwitchDefinition, parent=None):
        super().__init__(parent)
        self.switch = sw
        self._flash_timer = QTimer(self)
        self._flash_timer.setSingleShot(True)
        self._flash_timer.timeout.connect(self._stop_flash)

        self.setFrameShape(QFrame.Shape.NoFrame)
        self.setObjectName("switchRow")
        self._apply_style(_IDLE_BG, _IDLE_BORDER)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 8, 10, 8)
        layout.setSpacing(5)

        # ── Top row ───────────────────────────────────────────────────────
        top = QHBoxLayout()
        top.setSpacing(6)

        self.enabled_cb = QCheckBox()
        self.enabled_cb.setChecked(sw.enabled)
        self.enabled_cb.toggled.connect(self._on_enabled_toggled)
        self.enabled_cb.setStyleSheet("QCheckBox::indicator { width:14px; height:14px; }")

        self.name_label = QLabel(sw.name)
        self.name_label.setStyleSheet("font-weight: 600; color: #e8e8e8; font-size: 12px;")

        self.edit_btn = QPushButton("✏ Edit")
        self.edit_btn.setFixedSize(64, 24)
        self.edit_btn.setStyleSheet("""
            QPushButton {
                background: #2d4a6a; color: #7ab8f5;
                border: 1px solid #3d6a9a; border-radius: 4px;
                font-size: 11px; font-weight: 600;
            }
            QPushButton:hover { background: #3d5a7a; color: #aad4ff; }
            QPushButton:pressed { background: #1d3a5a; }
        """)
        self.edit_btn.clicked.connect(lambda: self.edit_requested.emit(sw.id))

        self.del_btn = QPushButton("✕")
        self.del_btn.setFixedSize(24, 24)
        self.del_btn.setStyleSheet("""
            QPushButton {
                background: #3a2020; color: #c06060;
                border: 1px solid #5a3030; border-radius: 4px;
                font-size: 12px; font-weight: bold;
            }
            QPushButton:hover { background: #4a2828; color: #e07070; }
            QPushButton:pressed { background: #2a1818; }
        """)
        self.del_btn.clicked.connect(lambda: self.delete_requested.emit(sw.id))

        top.addWidget(self.enabled_cb)
        top.addWidget(self.name_label, 1)
        top.addWidget(self.edit_btn)
        top.addWidget(self.del_btn)
        layout.addLayout(top)

        # ── Info row ──────────────────────────────────────────────────────
        movement_name = MOVEMENT_LABELS.get(sw.movement, sw.movement)
        action_name   = ACTION_LABELS.get(sw.action_type, sw.action_type)
        self.info_label = QLabel(f"{movement_name}  →  {action_name}: {sw.action_key}")
        self.info_label.setStyleSheet("color: #6a8aaa; font-size: 11px;")
        layout.addWidget(self.info_label)

        # ── Progress bar row ──────────────────────────────────────────────
        bar_row = QHBoxLayout()
        bar_row.setSpacing(6)

        self.progress = QProgressBar()
        self.progress.setRange(0, 100)
        self.progress.setValue(0)
        self.progress.setFixedHeight(5)
        self.progress.setTextVisible(False)
        self._update_bar_style(False)

        self.threshold_label = QLabel(f"{int(sw.threshold * 100)}%")
        self.threshold_label.setFixedWidth(30)
        self.threshold_label.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        self.threshold_label.setStyleSheet("color: #555; font-size: 10px;")

        bar_row.addWidget(self.progress, 1)
        bar_row.addWidget(self.threshold_label)
        layout.addLayout(bar_row)

    def update_value(self, value: float):
        self.progress.setValue(int(value * 100))
        self._update_bar_style(value >= self.switch.threshold)

    def flash(self):
        self._apply_style(_FLASH_BG, _FLASH_BORDER)
        self._flash_timer.start(350)

    def refresh(self, sw: SwitchDefinition):
        self.switch = sw
        self.name_label.setText(sw.name)
        movement_name = MOVEMENT_LABELS.get(sw.movement, sw.movement)
        action_name   = ACTION_LABELS.get(sw.action_type, sw.action_type)
        self.info_label.setText(f"{movement_name}  →  {action_name}: {sw.action_key}")
        self.threshold_label.setText(f"{int(sw.threshold * 100)}%")
        self.enabled_cb.setChecked(sw.enabled)

    def _stop_flash(self):
        self._apply_style(_IDLE_BG, _IDLE_BORDER)

    def _apply_style(self, bg, border):
        self.setStyleSheet(_ROW_STYLE.format(bg=bg, border=border))

    def _update_bar_style(self, active: bool):
        color = "#3d9f5e" if active else "#3a3a3a"
        self.progress.setStyleSheet(f"""
            QProgressBar {{ background: #2a2a2a; border-radius: 2px; }}
            QProgressBar::chunk {{ background: {color}; border-radius: 2px; }}
        """)

    def _on_enabled_toggled(self, checked: bool):
        self.switch.enabled = checked


class SwitchListWidget(QWidget):
    edit_requested   = pyqtSignal(str)
    delete_requested = pyqtSignal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumWidth(290)
        self.setStyleSheet("background: transparent;")

        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)

        header = QLabel("Switches")
        header.setStyleSheet(
            "font-size: 13px; font-weight: bold; color: #bbb;"
            "padding: 10px 8px 6px 8px; letter-spacing: 1px;"
        )
        outer.addWidget(header)

        self._empty_label = QLabel(
            "Select a body part in the Mouse panel to move the cursor.\n\n"
            "Starter switches: open palm for left click and blink for right click."
        )
        self._empty_label.setWordWrap(True)
        self._empty_label.setStyleSheet(
            "color: #888; font-size: 11px; padding: 0 8px 10px 8px;"
        )
        outer.addWidget(self._empty_label)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setStyleSheet("background: transparent; border: none;")

        self._container = QWidget()
        self._container.setStyleSheet("background: transparent;")
        self._list_layout = QVBoxLayout(self._container)
        self._list_layout.setContentsMargins(0, 0, 0, 0)
        self._list_layout.setSpacing(2)
        self._list_layout.addStretch()

        scroll.setWidget(self._container)
        outer.addWidget(scroll, 1)

        self._rows: dict[str, SwitchRow] = {}

    def add_switch(self, sw: SwitchDefinition):
        row = SwitchRow(sw)
        row.edit_requested.connect(self.edit_requested)
        row.delete_requested.connect(self.delete_requested)
        idx = self._list_layout.count() - 1
        self._list_layout.insertWidget(idx, row)
        self._rows[sw.id] = row

    def remove_switch(self, switch_id: str):
        row = self._rows.pop(switch_id, None)
        if row:
            self._list_layout.removeWidget(row)
            row.deleteLater()

    def refresh_switch(self, sw: SwitchDefinition):
        row = self._rows.get(sw.id)
        if row:
            row.refresh(sw)

    def clear(self):
        for row in list(self._rows.values()):
            self._list_layout.removeWidget(row)
            row.deleteLater()
        self._rows.clear()

    def update_values(self, values: dict):
        for switch_id, value in values.items():
            row = self._rows.get(switch_id)
            if row:
                row.update_value(value)

    def flash(self, switch_id: str):
        row = self._rows.get(switch_id)
        if row:
            row.flash()

    def set_prompt(self, text: str):
        self._empty_label.setText(text)
