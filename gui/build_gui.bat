@echo off
setlocal

set "QT_ROOT=C:\Users\EternalWing\Qt\6.8.3\msvc2022_64"
set "OPENCV_ROOT=C:\Users\EternalWing\opencv"
set "GUI_DIR=%~dp0"
if "%GUI_DIR:~-1%"=="\" set "GUI_DIR=%GUI_DIR:~0,-1%"

if not exist "%QT_ROOT%\lib\cmake\Qt6\Qt6Config.cmake" (
    echo error: Qt not found at "%QT_ROOT%"
    exit /b 1
)
if not exist "%OPENCV_ROOT%\build\OpenCVConfig.cmake" (
    echo error: OpenCV not found at "%OPENCV_ROOT%"
    exit /b 1
)

python -m cmake -S "%GUI_DIR%" -B "%GUI_DIR%\build" -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="%QT_ROOT%" -DOpenCV_DIR="%OPENCV_ROOT%\build"
if errorlevel 1 exit /b 1

python -m cmake --build "%GUI_DIR%\build" --config Release
if errorlevel 1 exit /b 1

echo build ok: %GUI_DIR%\bin\VividMatchGui.exe
