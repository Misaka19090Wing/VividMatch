@echo off
setlocal

if exist "%~dp0..\dist\VividMatchGui\VividMatchGui.exe" (
    start "" "%~dp0..\dist\VividMatchGui\VividMatchGui.exe"
    exit /b 0
)

call "%~dp0env_paths.bat"
if errorlevel 1 exit /b 1

set "PATH=%QT_ROOT%\bin;%OPENCV_BIN%;%PATH%"

if not exist "%~dp0bin\VividMatchGui.exe" (
    echo [run_gui] build the GUI first with build_gui.bat, or use dist\VividMatchGui.
    exit /b 1
)

start "" "%~dp0bin\VividMatchGui.exe"
