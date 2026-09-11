# VividMatch GUI

Qt 6 Widgets desktop GUI, based on the window/page structure and CMake layout
of [XMuli/myapp-template](https://github.com/XMuli/myapp-template) (MIT).

The GUI exposes a function-selection home page. Selecting image-compare mode
opens a page where the user chooses two image files and runs the OpenCV DCT
fingerprint comparison from `../cpp/visual_fingerprint.hpp`.

Images can be selected with the buttons or dragged from the file manager and
dropped onto the matching preview panel.

Batch mode accepts a whole folder, multiple image files, or drag-and-drop.
Every row shows a thumbnail, file name, resolution, type, size, modification
date, path and bit depth. After comparison, duplicate groups are ordered by
similarity at the top; the selection policy decides which image of each group
stays checked (default: highest resolution). "不取消勾选" keeps every image in
a duplicate group checked.

Batch list controls:

- Groups behave like Explorer groups and can be collapsed or expanded.
- Click a header to cycle ascending -> descending -> unsorted.
- Ctrl+click headers to add secondary and further sort fields.
- Right-click the header to show or hide columns; drag headers to reorder them.
- Enter opens the selected image, Delete removes it from the list, and Ctrl+F
  focuses the locate/search field.
- Double-click opens an image; the right-click menu provides copy path,
  properties, open containing folder, remove, delete and compare actions.

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
