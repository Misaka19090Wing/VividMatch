@echo off
setlocal

call "%~dp0env_paths.bat"
if errorlevel 1 exit /b 1

set "CMAKE_CMD="
where cmake >nul 2>nul
if not errorlevel 1 set "CMAKE_CMD=cmake"
if not defined CMAKE_CMD (
    python -m cmake --version >nul 2>nul
    if not errorlevel 1 set "CMAKE_CMD=python -m cmake"
)
if not defined CMAKE_CMD (
    echo [build_gui] cmake was not found. Install CMake or put python on PATH.
    exit /b 1
)

set "GUI_DIR=%~dp0"
if "%GUI_DIR:~-1%"=="\" set "GUI_DIR=%GUI_DIR:~0,-1%"

if not defined CMAKE_GENERATOR set "CMAKE_GENERATOR=Visual Studio 17 2022"
if not defined CMAKE_GENERATOR_ARGS set "CMAKE_GENERATOR_ARGS=-A x64"

echo [build_gui] Qt:     %QT_ROOT%
echo [build_gui] OpenCV: %OPENCV_ROOT%
echo [build_gui] CMake:  %CMAKE_CMD%

%CMAKE_CMD% -S "%GUI_DIR%" -B "%GUI_DIR%\build" -G "%CMAKE_GENERATOR%" %CMAKE_GENERATOR_ARGS% -DCMAKE_PREFIX_PATH="%QT_ROOT%" -DOpenCV_DIR="%OPENCV_ROOT%\build"
if errorlevel 1 exit /b 1

%CMAKE_CMD% --build "%GUI_DIR%\build" --config Release
if errorlevel 1 exit /b 1

echo build ok: %GUI_DIR%\bin\VividMatchGui.exe
