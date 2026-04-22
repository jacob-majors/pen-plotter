from __future__ import annotations
import time
import uuid
from collections import deque
from dataclasses import dataclass, field

_POSE_MAX_DEGREES = 45.0
_HAND_ZONE = 0.30   # fraction of frame for hand-left / hand-right zones

MOVEMENT_LABELS = {
    # ── Face blendshapes ────────────────────────────────────────────────────
    "eyeBlinkLeft":      "Left Eye Blink",
    "eyeBlinkRight":     "Right Eye Blink",
    "eyeBlinkBoth":      "Both Eyes Blink",
    "jawOpen":           "Mouth Open",
    "browInnerUp":       "Eyebrows Raise",
    "browOuterUpLeft":   "Left Eyebrow Raise",
    "browOuterUpRight":  "Right Eyebrow Raise",
    "mouthSmileLeft":    "Smile Left",
    "mouthSmileRight":   "Smile Right",
    # ── Head pose ────────────────────────────────────────────────────────────
    "yaw_left":   "Head Turn Left",
    "yaw_right":  "Head Turn Right",
    "pitch_up":   "Head Tilt Up",
    "pitch_down": "Head Tilt Down",
    "roll_left":  "Head Roll Left",
    "roll_right": "Head Roll Right",
    # ── Hand gestures ────────────────────────────────────────────────────────
    "gesture_Thumb_Up":    "Thumbs Up 👍",
    "gesture_Victory":     "Peace Sign ✌",
    "gesture_Closed_Fist": "Fist 👊",
    "gesture_Open_Palm":   "Open Palm 🖐",
    "gesture_Pointing_Up": "Point Up ☝",
    "gesture_ILoveYou":    "ILY Sign 🤘",
    # ── Hand position ────────────────────────────────────────────────────────
    "hand_left":  "Hand Move Left",
    "hand_right": "Hand Move Right",
    "hand_up":    "Hand Move Up",
    "hand_down":  "Hand Move Down",
    # ── Finger extended ──────────────────────────────────────────────────────
    "finger_index": "Index Finger Extended",
    "finger_pinky": "Pinky Finger Extended",
}

# Body parts that can drive the mouse cursor
MOUSE_SOURCES = {
    "none":      "Disabled",
    "nose":      "Nose Tip",
    "forehead":  "Forehead",
    "chin":      "Chin",
    "hand":      "Hand (wrist)",
    "index_tip": "Index Fingertip",
    "head":      "Head Center",
}

MOVEMENTS = list(MOVEMENT_LABELS.keys())

MOVEMENT_GROUPS = {
    "Face": [
        "eyeBlinkLeft", "eyeBlinkRight", "eyeBlinkBoth", "jawOpen",
        "browInnerUp", "browOuterUpLeft", "browOuterUpRight",
        "mouthSmileLeft", "mouthSmileRight",
    ],
    "Head": ["yaw_left", "yaw_right", "pitch_up", "pitch_down", "roll_left", "roll_right"],
    "Hand Gestures": [
        "gesture_Thumb_Up", "gesture_Victory", "gesture_Closed_Fist",
        "gesture_Open_Palm", "gesture_Pointing_Up", "gesture_ILoveYou",
    ],
    "Hand Position": ["hand_left", "hand_right", "hand_up", "hand_down"],
    "Fingers": ["finger_index", "finger_pinky"],
}

ACTION_LABELS = {
    "tap":         "Tap Key",
    "hold":        "Hold Key",
    "mouse_left":  "Mouse Left Click",
    "mouse_right": "Mouse Right Click",
    "scroll_up":   "Scroll Up",
    "scroll_down": "Scroll Down",
}

ACTIONS = list(ACTION_LABELS.keys())


@dataclass
class SwitchDefinition:
    name:        str
    movement:    str
    threshold:   float
    action_type: str
    action_key:  str
    cooldown_ms: int  = 500
    enabled:     bool = True
    id:          str  = field(default_factory=lambda: str(uuid.uuid4()))

    def to_dict(self) -> dict:
        return {
            "id": self.id, "name": self.name, "movement": self.movement,
            "threshold": self.threshold, "action_type": self.action_type,
            "action_key": self.action_key, "cooldown_ms": self.cooldown_ms,
            "enabled": self.enabled,
        }

    @classmethod
    def from_dict(cls, d: dict) -> "SwitchDefinition":
        return cls(
            id=d.get("id", str(uuid.uuid4())), name=d["name"],
            movement=d["movement"], threshold=d["threshold"],
            action_type=d["action_type"], action_key=d["action_key"],
            cooldown_ms=d.get("cooldown_ms", 500), enabled=d.get("enabled", True),
        )


@dataclass
class SwitchEvent:
    switch:        SwitchDefinition
    current_value: float
    triggered:     bool
    active:        bool


_SMOOTH_WINDOW = 5   # frames to average (~167 ms at 30 fps)


class SwitchEngine:
    def __init__(self):
        self.switches: list[SwitchDefinition] = []
        self._last_trigger:   dict[str, float] = {}
        self._was_active:     dict[str, bool]  = {}
        self._current_values: dict[str, float] = {}
        # Per-switch rolling window of raw values for smoothing
        self._history: dict[str, deque] = {}

    def get_value(self, movement: str, face_data: dict) -> float:
        blendshapes = face_data.get("blendshapes", {})
        pose        = face_data.get("pose", {})
        gestures    = face_data.get("gestures", {})
        hand_pos    = face_data.get("hand_position", {})

        if movement in blendshapes:
            return blendshapes[movement]

        if movement == "yaw_left":
            return max(0.0, -pose.get("yaw", 0.0))   / _POSE_MAX_DEGREES
        if movement == "yaw_right":
            return max(0.0,  pose.get("yaw", 0.0))   / _POSE_MAX_DEGREES
        if movement == "pitch_up":
            return max(0.0,  pose.get("pitch", 0.0)) / _POSE_MAX_DEGREES
        if movement == "pitch_down":
            return max(0.0, -pose.get("pitch", 0.0)) / _POSE_MAX_DEGREES
        if movement == "roll_left":
            return max(0.0, -pose.get("roll", 0.0))  / _POSE_MAX_DEGREES
        if movement == "roll_right":
            return max(0.0,  pose.get("roll", 0.0))  / _POSE_MAX_DEGREES

        if movement.startswith("gesture_"):
            gesture_name = movement[len("gesture_"):]
            return gestures.get(gesture_name, 0.0)

        if hand_pos:
            x = hand_pos.get("x", 0.5)
            y = hand_pos.get("y", 0.5)
            if movement == "hand_left":
                return max(0.0, (_HAND_ZONE - x) / _HAND_ZONE)
            if movement == "hand_right":
                return max(0.0, (x - (1 - _HAND_ZONE)) / _HAND_ZONE)
            if movement == "hand_up":
                return max(0.0, (_HAND_ZONE - y) / _HAND_ZONE)
            if movement == "hand_down":
                return max(0.0, (y - (1 - _HAND_ZONE)) / _HAND_ZONE)

        fingers = face_data.get("fingers", {})
        if movement == "finger_index":
            return float(fingers.get("index", 0))
        if movement == "finger_pinky":
            return float(fingers.get("pinky", 0))

        return 0.0

    def evaluate(self, face_data: dict) -> list[SwitchEvent]:
        now = time.time()
        events = []
        for sw in self.switches:
            if not sw.enabled:
                continue
            raw = min(1.0, max(0.0, self.get_value(sw.movement, face_data)))

            # Rolling average smoothing — reduces noise and false triggers
            if sw.id not in self._history:
                self._history[sw.id] = deque(maxlen=_SMOOTH_WINDOW)
            self._history[sw.id].append(raw)
            value = sum(self._history[sw.id]) / len(self._history[sw.id])

            self._current_values[sw.id] = value

            active      = value >= sw.threshold
            was         = self._was_active.get(sw.id, False)
            last        = self._last_trigger.get(sw.id, 0.0)
            cooldown_ok = (now - last) * 1000 >= sw.cooldown_ms

            triggered = active and not was and cooldown_ok
            if triggered:
                self._last_trigger[sw.id] = now
            self._was_active[sw.id] = active
            events.append(SwitchEvent(sw, value, triggered, active))
        return events

    def current_value(self, switch_id: str) -> float:
        return self._current_values.get(switch_id, 0.0)
