@echo off
rem Shared path detection for the GUI build/package scripts.
rem Override any detected path by setting QT_ROOT, OPENCV_ROOT or OPENCV_BIN.

if not defined QT_ROOT (
    for %%D in (
        "C:\Qt\6.8.3\msvc2022_64"
        "C:\Qt\6.8.2\msvc2022_64"
        "C:\Qt\6.7.3\msvc2022_64"
        "%USERPROFILE%\Qt\6.8.3\msvc2022_64"
        "%USERPROFILE%\Qt\6.8.2\msvc2022_64"
        "D:\Qt\6.8.3\msvc2022_64"
    ) do (
        if exist "%%~D\lib\cmake\Qt6\Qt6Config.cmake" set "QT_ROOT=%%~D"
    )
)

if not defined OPENCV_ROOT (
    for %%D in (
        "%USERPROFILE%\opencv"
        "C:\opencv"
        "D:\opencv"
    ) do (
        if exist "%%~D\build\OpenCVConfig.cmake" set "OPENCV_ROOT=%%~D"
    )
)

if not defined OPENCV_ROOT if defined OPENCV_DIR (
    if exist "%OPENCV_DIR%\build\OpenCVConfig.cmake" set "OPENCV_ROOT=%OPENCV_DIR%"
    if not defined OPENCV_ROOT if exist "%OPENCV_DIR%\OpenCVConfig.cmake" set "OPENCV_ROOT=%OPENCV_DIR%\.."
)

if not defined OPENCV_BIN (
    if defined OPENCV_ROOT (
        if exist "%OPENCV_ROOT%\build\x64\vc16\bin\opencv_world*.dll" (
            set "OPENCV_BIN=%OPENCV_ROOT%\build\x64\vc16\bin"
        )
        if not defined OPENCV_BIN (
            if exist "%OPENCV_ROOT%\build\x64\vc15\bin\opencv_world*.dll" (
                set "OPENCV_BIN=%OPENCV_ROOT%\build\x64\vc15\bin"
            )
        )
    )
)

if not defined QT_ROOT (
    echo [env_paths] Qt6 was not found. Set QT_ROOT to your Qt msvc2022_64 directory.
    exit /b 1
)
if not defined OPENCV_ROOT (
    echo [env_paths] OpenCV was not found. Set OPENCV_ROOT to the directory that contains its build folder.
    exit /b 1
)
if not exist "%QT_ROOT%\lib\cmake\Qt6\Qt6Config.cmake" (
    echo [env_paths] Invalid QT_ROOT: %QT_ROOT%
    exit /b 1
)
if not exist "%OPENCV_ROOT%\build\OpenCVConfig.cmake" (
    echo [env_paths] Invalid OPENCV_ROOT: %OPENCV_ROOT%
    exit /b 1
)
exit /b 0
