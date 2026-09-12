@echo off
setlocal

set "OPENCV_ROOT=%~1"
if "%OPENCV_ROOT%"=="" set "OPENCV_ROOT=C:\Users\EternalWing\opencv"

set "VCVARS=D:\Visual Studio\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo error: Visual Studio vcvars64.bat not found: "%VCVARS%"
    exit /b 1
)
if not exist "%OPENCV_ROOT%\build\include\opencv2\opencv.hpp" (
    echo error: OpenCV include directory not found under "%OPENCV_ROOT%"
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

set "SOURCE_DIR=%~dp0"
set "OPENCV_INC=%OPENCV_ROOT%\build\include"
set "OPENCV_LIB=%OPENCV_ROOT%\build\x64\vc16\lib"

pushd "%SOURCE_DIR%"

cl /nologo /std:c++17 /O2 /EHsc /utf-8 /I"%OPENCV_INC%" image_tool.cpp /Fe:vividmatch_image.exe /link /LIBPATH:"%OPENCV_LIB%" opencv_world500.lib
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /O2 /EHsc /utf-8 /I"%OPENCV_INC%" test_visual_fingerprint.cpp /Fe:vividmatch_image_test.exe /link /LIBPATH:"%OPENCV_LIB%" opencv_world500.lib
if errorlevel 1 exit /b 1

rem video_tool.cpp defines wmain() so Windows hands it UTF-16 arguments; that
rem needs the wide console entry point.
cl /nologo /std:c++17 /O2 /EHsc /utf-8 /I"%OPENCV_INC%" video_tool.cpp /Fe:vividmatch_video.exe /link /LIBPATH:"%OPENCV_LIB%" /SUBSYSTEM:CONSOLE /ENTRY:wmainCRTStartup opencv_world500.lib
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /O2 /EHsc /utf-8 /I"%OPENCV_INC%" test_video_fingerprint.cpp /Fe:vividmatch_video_test.exe /link /LIBPATH:"%OPENCV_LIB%" opencv_world500.lib
if errorlevel 1 exit /b 1

popd

echo build ok
