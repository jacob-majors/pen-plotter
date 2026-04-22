@echo off
echo === OpenCV Controller Setup (Windows) ===

python -m venv .venv
call .venv\Scripts\activate.bat
python -m pip install --upgrade pip
pip install -r requirements.txt

echo Downloading models...
if not exist resources mkdir resources

curl -L -o resources\face_landmarker.task "https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task"
curl -L -o resources\gesture_recognizer.task "https://storage.googleapis.com/mediapipe-models/gesture_recognizer/gesture_recognizer/float16/1/gesture_recognizer.task"

echo.
echo === Done! Run: .venv\Scripts\activate.bat ^&^& python main.py ===
pause
