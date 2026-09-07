# VividMatch GUI

Qt 6 Widgets desktop GUI, based on the window/page structure and CMake layout
of [XMuli/myapp-template](https://github.com/XMuli/myapp-template) (MIT).

The GUI exposes a function-selection home page. Selecting image-compare mode
opens a page where the user chooses two image files and runs the OpenCV DCT
fingerprint comparison from `../cpp/visual_fingerprint.hpp`.

Images can be selected with the buttons or dragged from the file manager and
dropped onto the matching preview panel.

Build:

```bat
gui\build_gui.bat
```

Run:

```bat
gui\run_gui.bat
```

## Portable package

Run once on a build machine that has Qt and OpenCV installed:

```bat
gui\package_gui.bat
```

The result is written to `dist\VividMatchGui\`. Qt DLLs/plugins and the OpenCV
runtime DLL are copied next to the executable, so the whole folder can be
copied to another Windows x64 computer that has neither Qt nor OpenCV
installed. Launch it with `VividMatchGui.exe` directly or from this repository
with `gui\run_gui.bat`.

## Paths on different machines

The scripts never assume one user profile. When `QT_ROOT`, `OPENCV_ROOT` or
`OPENCV_BIN` are set as environment variables, those values are used first.
Otherwise the scripts look at common install locations such as `C:\Qt`,
`D:\Qt`, `%USERPROFILE%\Qt`, `C:\opencv`, `D:\opencv` and
`%USERPROFILE%\opencv`.

`CMAKE_GENERATOR` and `CMAKE_GENERATOR_ARGS` can be overridden for toolchains
other than Visual Studio 2022 x64.

If CMake is not on `PATH`, the scripts fall back to `python -m cmake`.

In Visual Studio Code, register the installed Qt root with
`Qt: Register Qt installation`, then configure `gui/CMakeLists.txt` with CMake
Tools.

In Visual Studio Code, run `Qt: Register Qt installation` and point it at the
Qt directory above, then use CMake Tools to configure `gui/CMakeLists.txt`.
