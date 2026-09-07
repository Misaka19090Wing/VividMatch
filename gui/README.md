# VividMatch GUI

Qt 6 Widgets desktop GUI, based on the window/page structure and CMake layout
of [XMuli/myapp-template](https://github.com/XMuli/myapp-template) (MIT).

The GUI exposes a function-selection home page. Selecting image-compare mode
opens a page where the user chooses two image files and runs the OpenCV DCT
fingerprint comparison from `../cpp/visual_fingerprint.hpp`.

Build:

```bat
gui\build_gui.bat
```

Run:

```bat
gui\run_gui.bat
```

Expected paths used by the scripts:

- Qt: `C:\Users\EternalWing\Qt\6.8.3\msvc2022_64`
- OpenCV: `C:\Users\EternalWing\opencv`

In Visual Studio Code, run `Qt: Register Qt installation` and point it at the
Qt directory above, then use CMake Tools to configure `gui/CMakeLists.txt`.
