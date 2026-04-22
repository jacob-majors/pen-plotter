@echo off
:: Builds OpenCV.exe — a standalone Windows application.
:: Run this once after setup.bat.

call .venv\Scripts\activate.bat

pip install --quiet pyinstaller

echo Building OpenCV.exe...
pyinstaller ^
  --noconfirm ^
  --windowed ^
  --name "OpenCV" ^
  --icon "resources\icon.png" ^
  --add-data "resources\face_landmarker.task;resources" ^
  --add-data "resources\gesture_recognizer.task;resources" ^
  --add-data "resources\icon.png;resources" ^
  --add-data "profiles;profiles" ^
  --collect-all mediapipe ^
  --collect-all cv2 ^
  --hidden-import pynput ^
  --hidden-import pynput.keyboard ^
  --hidden-import pynput.mouse ^
  --hidden-import PyQt6 ^
  --hidden-import PyQt6.QtWidgets ^
  --hidden-import PyQt6.QtCore ^
  --hidden-import PyQt6.QtGui ^
  main.py

echo.
echo === Build complete! ===
echo App is at: dist\OpenCV\OpenCV.exe
echo.
echo To distribute: zip the dist\OpenCV folder
pause
