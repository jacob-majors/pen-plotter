import numpy as np
import cv2
from PyQt6.QtCore import Qt, pyqtSlot
from PyQt6.QtGui import QImage, QPixmap, QPainter, QColor, QPen
from PyQt6.QtWidgets import QLabel


class CameraWidget(QLabel):
    """Displays the live webcam feed with facial landmark overlay."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.setMinimumSize(480, 360)
        self.setStyleSheet("background-color: #1a1a1a;")
        self.setText("Camera not started")
        self.setStyleSheet("background-color: #1a1a1a; color: #666; font-size: 14px;")

        self._landmarks: list[tuple[int, int]] = []
        self._face_detected = False
        self._show_landmarks = True
        self._mouse_target: tuple[float, float] | None = None
        self._mouse_source_label = ""

    def set_show_landmarks(self, show: bool):
        self._show_landmarks = show

    @pyqtSlot(object)
    def update_frame(self, frame: np.ndarray):
        """Slot: receive BGR numpy frame and display it."""
        rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        h, w, ch = rgb.shape
        qimg = QImage(rgb.data, w, h, ch * w, QImage.Format.Format_RGB888)
        pixmap = QPixmap.fromImage(qimg)

        # Scale to fit widget while preserving aspect ratio
        pixmap = pixmap.scaled(
            self.size(),
            Qt.AspectRatioMode.KeepAspectRatio,
            Qt.TransformationMode.SmoothTransformation,
        )

        if self._show_landmarks and self._landmarks and self._face_detected:
            pixmap = self._draw_landmarks(pixmap, frame.shape, w, h)
        if self._mouse_target is not None:
            pixmap = self._draw_mouse_target(pixmap, w, h)

        self.setPixmap(pixmap)
        # Clear the placeholder text style
        self.setStyleSheet("background-color: #1a1a1a;")

    @pyqtSlot(dict)
    def update_face_data(self, data: dict):
        self._face_detected = data.get("face_detected", False)
        self._landmarks = data.get("landmarks", [])

    def set_mouse_target(self, pos: tuple[float, float] | None, source_label: str = ""):
        self._mouse_target = pos
        self._mouse_source_label = source_label

    def _draw_landmarks(self, pixmap: QPixmap, orig_shape, orig_w: int, orig_h: int) -> QPixmap:
        pw = pixmap.width()
        ph = pixmap.height()
        sx = pw / orig_w
        sy = ph / orig_h

        painter = QPainter(pixmap)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        # Draw every 4th landmark to keep it lightweight
        dot_pen = QPen(QColor(0, 220, 120, 180))
        dot_pen.setWidth(1)
        painter.setPen(dot_pen)
        painter.setBrush(QColor(0, 220, 120, 140))

        for i, (x, y) in enumerate(self._landmarks):
            if i % 4 != 0:
                continue
            px = int(x * sx)
            py = int(y * sy)
            painter.drawEllipse(px - 1, py - 1, 3, 3)

        painter.end()
        return pixmap

    def _draw_mouse_target(self, pixmap: QPixmap, orig_w: int, orig_h: int) -> QPixmap:
        if self._mouse_target is None:
            return pixmap

        px = int(self._mouse_target[0] * pixmap.width())
        py = int(self._mouse_target[1] * pixmap.height())

        painter = QPainter(pixmap)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        outer_pen = QPen(QColor(255, 196, 64, 230))
        outer_pen.setWidth(3)
        painter.setPen(outer_pen)
        painter.setBrush(QColor(255, 196, 64, 70))
        painter.drawEllipse(px - 10, py - 10, 20, 20)

        inner_pen = QPen(QColor(255, 255, 255, 240))
        inner_pen.setWidth(2)
        painter.setPen(inner_pen)
        painter.setBrush(QColor(255, 255, 255, 220))
        painter.drawEllipse(px - 3, py - 3, 6, 6)

        if self._mouse_source_label:
            painter.setPen(QColor(255, 196, 64, 230))
            painter.drawText(px + 12, py - 12, self._mouse_source_label)

        painter.end()
        return pixmap
