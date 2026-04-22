# OpenCV — Adaptive Vision Controller

Control your Mac with your face, head movements, and hand gestures.

Made by **Jacob Majors** and **Troy Pappas** for Ramsey Mussalum's Design for Social Good class
at **Sonoma Academy** — March 2026

---

## What it does

OpenCV uses your webcam to detect movements and map them to keypresses — works as a controller for games, accessibility, or anything that takes keyboard input.

**Supported movements:**
- 👁 Eye blinks (left, right, both)
- 🤔 Head turn left/right, tilt up/down, roll left/right
- 😮 Mouth open, eyebrow raise, smile
- 👍 Thumbs up, ✌ peace sign, 👊 fist, 🖐 open palm
- ✋ Hand move left/right/up/down

## Quick Start

```bash
bash setup.sh
source .venv/bin/activate
python main.py
```

**Global hotkey:** `Cmd + Shift + O` — toggle tracking from any app (even games)

## Requirements

- macOS 12+
- Python 3.11+ (`brew install python@3.11`)
- Camera access (macOS will prompt)
- **Accessibility access** (required for keypresses to reach other apps)
  → System Settings → Privacy & Security → Accessibility → add Terminal

## Build a standalone .app

After setup.sh, run:
```bash
bash build_app.sh
```
The app will be at `dist/OpenCV.app`. Zip it to share with others.

## For games

1. Open your game
2. Switch back to OpenCV and click **Start** (or press `Cmd+Shift+O`)
3. OpenCV stays in the **menu bar** — the game stays focused
4. Your facial movements fire keypresses directly to the game
