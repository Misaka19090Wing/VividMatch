@echo off
setlocal

call "%~dp0env_paths.bat"
if errorlevel 1 exit /b 1

set "GUI_DIR=%~dp0"
if "%GUI_DIR:~-1%"=="\" set "GUI_DIR=%GUI_DIR:~0,-1%"
set "STAGE=%~dp0..\dist\VividMatchGui"

if not exist "%GUI_DIR%\bin\VividMatchGui.exe" (
    echo [package_gui] build the GUI first with build_gui.bat
    exit /b 1
)

if not defined OPENCV_BIN (
    echo [package_gui] OpenCV runtime bin was not found. Set OPENCV_BIN explicitly.
    exit /b 1
)

if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"

copy /Y "%GUI_DIR%\bin\VividMatchGui.exe" "%STAGE%\" >nul
if errorlevel 1 exit /b 1

rem The compiled interface translations travel with the executable. Without them
rem the app still runs, in English, because English is the source language.
if exist "%GUI_DIR%\bin\translations" (
    mkdir "%STAGE%\translations" >nul 2>nul
    copy /Y "%GUI_DIR%\bin\translations\*.qm" "%STAGE%\translations\" >nul
)

for /f "delims=" %%F in ('dir /b /a-d "%OPENCV_BIN%\opencv_world*.dll" 2^>nul ^| findstr /v /e /i "d.dll"') do (
    copy /Y "%OPENCV_BIN%\%%F" "%STAGE%\" >nul
)

if not exist "%QT_ROOT%\bin\windeployqt.exe" (
    echo [package_gui] windeployqt.exe not found in %QT_ROOT%\bin
    exit /b 1
)

set "VCVARS64_PATH="
if defined VCVARS64 set "VCVARS64_PATH=%VCVARS64%"
if not defined VCVARS64_PATH if exist "D:\Visual Studio\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64_PATH=D:\Visual Studio\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS64_PATH if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64_PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS64_PATH if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64_PATH=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS64_PATH if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64_PATH=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS64_PATH if exist "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64_PATH=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS64_PATH if exist "C:\Program Files\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64_PATH=C:\Program Files\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
if defined VCVARS64_PATH (
    echo [package_gui] Visual Studio vcvars64 found: %VCVARS64_PATH%
    call "%VCVARS64_PATH%" >nul
)
if defined VCToolsRedistDir (
    if exist "%VCToolsRedistDir%x64\Microsoft.VC143.CRT\*.dll" (
        copy /Y "%VCToolsRedistDir%x64\Microsoft.VC143.CRT\*.dll" "%STAGE%\" >nul
    )
)

"%QT_ROOT%\bin\windeployqt.exe" --release --compiler-runtime --no-translations --no-opengl-sw "%STAGE%\VividMatchGui.exe"
if errorlevel 1 exit /b 1

echo package ok: %STAGE%
