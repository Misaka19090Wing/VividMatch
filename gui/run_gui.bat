@echo off
setlocal

set "QT_BIN=C:\Users\EternalWing\Qt\6.8.3\msvc2022_64\bin"
set "OPENCV_BIN=C:\Users\EternalWing\opencv\build\x64\vc16\bin"
set "PATH=%QT_BIN%;%OPENCV_BIN%;%PATH%"

if not exist "%~dp0bin\VividMatchGui.exe" (
    echo error: build the GUI first with build_gui.bat
    exit /b 1
)

start "" "%~dp0bin\VividMatchGui.exe"
