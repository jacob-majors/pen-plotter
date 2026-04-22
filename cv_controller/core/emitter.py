from __future__ import annotations
import threading
import time
from pynput import keyboard, mouse
from pynput.keyboard import Key, Controller as KeyController
from pynput.mouse import Button, Controller as MouseController
from PyQt6.QtGui import QGuiApplication


# Map friendly key names → pynput Key enum
_SPECIAL_KEYS = {
    "space": Key.space,
    "enter": Key.enter,
    "return": Key.enter,
    "tab": Key.tab,
    "escape": Key.esc,
    "esc": Key.esc,
    "backspace": Key.backspace,
    "delete": Key.delete,
    "up": Key.up,
    "down": Key.down,
    "left": Key.left,
    "right": Key.right,
    "home": Key.home,
    "end": Key.end,
    "page_up": Key.page_up,
    "page_down": Key.page_down,
    "f1": Key.f1, "f2": Key.f2, "f3": Key.f3, "f4": Key.f4,
    "f5": Key.f5, "f6": Key.f6, "f7": Key.f7, "f8": Key.f8,
    "f9": Key.f9, "f10": Key.f10, "f11": Key.f11, "f12": Key.f12,
    "cmd": Key.cmd, "ctrl": Key.ctrl, "alt": Key.alt, "shift": Key.shift,
    "media_play_pause": Key.media_play_pause,
    "media_next": Key.media_next,
    "media_previous": Key.media_previous,
    "volume_up": Key.media_volume_up,
    "volume_down": Key.media_volume_down,
    "volume_mute": Key.media_volume_mute,
}

_MODIFIER_NAMES = {
    "cmd": Key.cmd,
    "ctrl": Key.ctrl,
    "alt": Key.alt,
    "shift": Key.shift,
    "option": Key.alt,
    "command": Key.cmd,
    "control": Key.ctrl,
}


def _parse_key(key_str: str):
    """Parse 'space', 'cmd+c', 'shift+left' etc. into (modifiers, key)."""
    parts = [p.strip().lower() for p in key_str.split("+")]
    modifiers = []
    main_key = None

    for part in parts:
        if part in _MODIFIER_NAMES:
            modifiers.append(_MODIFIER_NAMES[part])
        elif part in _SPECIAL_KEYS:
            main_key = _SPECIAL_KEYS[part]
        elif len(part) == 1:
            main_key = part
        else:
            main_key = _SPECIAL_KEYS.get(part, part)

    return modifiers, main_key


class ActionEmitter:
    def __init__(self):
        self._kb = KeyController()
        self._mouse = MouseController()
        self._held_keys: dict[str, list] = {}  # switch_id → list of pressed keys
        self._mouse_control_active = False
        self._mouse_source: str | None = None   # which body part drives the mouse
        self._mouse_thread: threading.Thread | None = None
        self._mouse_pos = (0.5, 0.5)            # normalized (x, y) from tracker
        self._mouse_lock = threading.Lock()

    def set_mouse_control(self, source: str | None):
        """Enable or disable mouse control. source is e.g. 'nose', 'hand', 'head'."""
        with self._mouse_lock:
            self._mouse_source = source
            self._mouse_control_active = source is not None
        if source and (self._mouse_thread is None or not self._mouse_thread.is_alive()):
            self._mouse_thread = threading.Thread(target=self._mouse_loop, daemon=True)
            self._mouse_thread.start()

    def update_mouse_position(self, x: float, y: float):
        """Called from tracker with normalized 0–1 coords."""
        with self._mouse_lock:
            self._mouse_pos = (x, y)

    def _mouse_loop(self):
        while True:
            with self._mouse_lock:
                active = self._mouse_control_active
                x, y = self._mouse_pos
            if not active:
                break
            try:
                screen = QGuiApplication.primaryScreen()
                if screen is not None:
                    geometry = screen.geometry()
                    self._mouse.position = (
                        geometry.left() + int(x * geometry.width()),
                        geometry.top() + int(y * geometry.height()),
                    )
            except Exception:
                pass
            time.sleep(0.016)  # ~60 Hz

    def execute(self, action_type: str, action_key: str, switch_id: str, active: bool):
        try:
            if action_type == "tap":
                self._tap(action_key)
            elif action_type == "hold":
                self._hold(action_key, switch_id, active)
            elif action_type == "mouse_left":
                self._mouse.click(Button.left)
            elif action_type == "mouse_right":
                self._mouse.click(Button.right)
            elif action_type == "scroll_up":
                self._mouse.scroll(0, 3)
            elif action_type == "scroll_down":
                self._mouse.scroll(0, -3)
        except Exception:
            pass  # Accessibility permission not granted — fail silently

    def release_all(self):
        self.set_mouse_control(None)
        for key_list in self._held_keys.values():
            for k in key_list:
                try:
                    self._kb.release(k)
                except Exception:
                    pass
        self._held_keys.clear()

    def _tap(self, key_str: str):
        modifiers, main_key = _parse_key(key_str)
        for m in modifiers:
            self._kb.press(m)
        if main_key:
            self._kb.press(main_key)
            self._kb.release(main_key)
        for m in reversed(modifiers):
            self._kb.release(m)

    def _hold(self, key_str: str, switch_id: str, active: bool):
        modifiers, main_key = _parse_key(key_str)
        all_keys = modifiers + ([main_key] if main_key else [])

        if active and switch_id not in self._held_keys:
            for k in all_keys:
                self._kb.press(k)
            self._held_keys[switch_id] = all_keys
        elif not active and switch_id in self._held_keys:
            for k in reversed(self._held_keys.pop(switch_id)):
                self._kb.release(k)
