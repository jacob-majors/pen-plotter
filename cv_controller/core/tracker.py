import time
import queue
import threading
import numpy as np
import cv2
from math import sqrt, atan2, degrees

import mediapipe as mp
from mediapipe.tasks import python as mp_python
from mediapipe.tasks.python import vision as mp_vision
from mediapipe.tasks.python.vision import (
    FaceLandmarkerOptions, RunningMode,
    GestureRecognizerOptions,
)

from PyQt6.QtCore import QObject, QTimer, pyqtSignal


class FaceTracker(QObject):
    """
    Camera capture runs on main thread via QTimer (required on macOS).
    MediaPipe face + gesture processing runs in a background thread.
    """
    frame_ready  = pyqtSignal(object)  # BGR numpy array
    face_data    = pyqtSignal(dict)    # parsed face + hand info
    tracking_error = pyqtSignal(str)

    FACE_BLENDSHAPES = [
        "eyeBlinkLeft", "eyeBlinkRight", "jawOpen",
        "browInnerUp", "browOuterUpLeft", "browOuterUpRight",
        "mouthSmileLeft", "mouthSmileRight",
    ]

    def __init__(self, model_path: str, gesture_model_path: str = None,
                 camera_index: int = 0, parent=None):
        super().__init__(parent)
        self.model_path = model_path
        self.gesture_model_path = gesture_model_path
        self.camera_index = camera_index
        self.flip_horizontal = True

        self._cap = None
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._grab_frame)

        self._frame_queue:  queue.Queue = queue.Queue(maxsize=2)
        self._result_queue: queue.Queue = queue.Queue(maxsize=4)
        self._worker_thread: threading.Thread | None = None
        self._running = False
        self._start_time = 0.0

    def start(self):
        self._cap = cv2.VideoCapture(self.camera_index)
        if not self._cap.isOpened():
            self.tracking_error.emit("Could not open camera. Check camera permissions.")
            return
        self._cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
        self._cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
        self._running = True
        self._start_time = time.time()
        self._worker_thread = threading.Thread(target=self._mediapipe_worker, daemon=True)
        self._worker_thread.start()
        self._timer.start(33)

    def stop(self):
        self._running = False
        self._timer.stop()
        if self._cap:
            self._cap.release()
            self._cap = None
        try:
            self._frame_queue.put_nowait(None)
        except queue.Full:
            pass
        if self._worker_thread:
            self._worker_thread.join(timeout=2)
            self._worker_thread = None

    def isRunning(self) -> bool:
        return self._running

    # ── Main-thread frame grab ───────────────────────────────────────────────

    def _grab_frame(self):
        if not self._cap or not self._cap.isOpened():
            return
        ret, frame = self._cap.read()
        if not ret:
            return
        if self.flip_horizontal:
            frame = cv2.flip(frame, 1)
        self.frame_ready.emit(frame.copy())
        try:
            self._frame_queue.get_nowait()
        except queue.Empty:
            pass
        try:
            self._frame_queue.put_nowait(frame)
        except queue.Full:
            pass
        try:
            while True:
                data = self._result_queue.get_nowait()
                self.face_data.emit(data)
        except queue.Empty:
            pass

    # ── Background MediaPipe worker ──────────────────────────────────────────

    def _mediapipe_worker(self):
        face_options = FaceLandmarkerOptions(
            base_options=mp_python.BaseOptions(model_asset_path=self.model_path),
            output_face_blendshapes=True,
            output_facial_transformation_matrixes=True,
            running_mode=RunningMode.VIDEO,
            num_faces=1,
        )

        gesture_recognizer = None
        if self.gesture_model_path:
            try:
                g_options = GestureRecognizerOptions(
                    base_options=mp_python.BaseOptions(
                        model_asset_path=self.gesture_model_path
                    ),
                    running_mode=RunningMode.VIDEO,
                    num_hands=2,
                )
                gesture_recognizer = mp_vision.GestureRecognizer.create_from_options(g_options)
            except Exception:
                gesture_recognizer = None

        with mp_vision.FaceLandmarker.create_from_options(face_options) as face_lm:
            try:
                while self._running:
                    try:
                        frame = self._frame_queue.get(timeout=1.0)
                    except queue.Empty:
                        continue
                    if frame is None:
                        break

                    rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
                    mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
                    ts = int((time.time() - self._start_time) * 1000)

                    try:
                        face_result = face_lm.detect_for_video(mp_image, ts)
                        data = self._parse_face(face_result, frame.shape)

                        if gesture_recognizer:
                            g_result = gesture_recognizer.recognize_for_video(mp_image, ts)
                            data.update(self._parse_gestures(g_result, frame.shape))

                        try:
                            self._result_queue.put_nowait(data)
                        except queue.Full:
                            pass
                    except Exception:
                        pass
            finally:
                if gesture_recognizer:
                    gesture_recognizer.close()

    # ── Parsers ──────────────────────────────────────────────────────────────

    def _parse_face(self, result, frame_shape) -> dict:
        if not result.face_landmarks:
            return {"face_detected": False, "blendshapes": {}, "pose": {}, "landmarks": []}

        h, w = frame_shape[:2]
        landmarks = [(int(lm.x * w), int(lm.y * h)) for lm in result.face_landmarks[0]]

        blendshapes = {}
        if result.face_blendshapes:
            for bs in result.face_blendshapes[0]:
                if bs.category_name in self.FACE_BLENDSHAPES:
                    blendshapes[bs.category_name] = round(bs.score, 4)
        left  = blendshapes.get("eyeBlinkLeft", 0.0)
        right = blendshapes.get("eyeBlinkRight", 0.0)
        blendshapes["eyeBlinkBoth"] = round(min(left, right), 4)

        pose = {}
        if result.facial_transformation_matrixes:
            mat = np.array(result.facial_transformation_matrixes[0].data).reshape(4, 4)
            pitch = atan2(-mat[2, 1], mat[2, 2])
            yaw   = atan2(mat[2, 0], sqrt(mat[2, 1] ** 2 + mat[2, 2] ** 2))
            roll  = atan2(-mat[1, 0], mat[0, 0])
            pose  = {
                "pitch": round(degrees(pitch), 2),
                "yaw":   round(degrees(yaw),   2),
                "roll":  round(degrees(roll),   2),
            }

        # Key face landmark indices (MediaPipe 478-point model)
        # 1=nose tip, 10=forehead, 152=chin, 168=between eyes
        h, w = frame_shape[:2]
        raw = result.face_landmarks[0]
        def _lm(idx):
            lm = raw[idx]
            return round(lm.x, 4), round(lm.y, 4)

        body_points = {
            "nose":     _lm(1),
            "forehead": _lm(10),
            "chin":     _lm(152),
            "head":     _lm(168),
        }

        return {
            "face_detected": True,
            "blendshapes": blendshapes,
            "pose": pose,
            "landmarks": landmarks,
            "body_points": body_points,
            "gestures": {},
            "hand_position": {},
            "hand_landmarks": [],
            "fingers": {},
        }

    def _parse_gestures(self, result, frame_shape) -> dict:
        gestures = {}
        hand_position = {}
        hand_positions = []
        hand_landmarks = []
        fingers = {}
        index_tip = {}
        hands_together = False

        if result.gestures:
            for gesture_list in result.gestures:
                for g in gesture_list:
                    gestures[g.category_name] = round(g.score, 4)

        if result.hand_landmarks:
            h, w = frame_shape[:2]
            all_hands = result.hand_landmarks[:2]
            for hand_idx, lms in enumerate(all_hands):
                wrist_x = round(lms[0].x, 4)
                wrist_y = round(lms[0].y, 4)
                hand_positions.append({"x": wrist_x, "y": wrist_y})

                if hand_idx == 0:
                    hand_landmarks = [(int(lm.x * w), int(lm.y * h)) for lm in lms]
                    hand_position = {"x": wrist_x, "y": wrist_y}
                    index_tip = {"x": round(lms[8].x, 4), "y": round(lms[8].y, 4)}
                    fingers = {
                        "index": 1 if lms[8].y < lms[6].y else 0,
                        "pinky": 1 if lms[20].y < lms[18].y else 0,
                    }

            if len(hand_positions) >= 2:
                dx = hand_positions[0]["x"] - hand_positions[1]["x"]
                dy = hand_positions[0]["y"] - hand_positions[1]["y"]
                hands_together = (dx * dx + dy * dy) ** 0.5 <= 0.12

        return {
            "gestures": gestures,
            "hand_position": hand_position,
            "hand_positions": hand_positions,
            "hand_landmarks": hand_landmarks,
            "index_tip": index_tip,
            "fingers": fingers,
            "hands_together": hands_together,
        }
