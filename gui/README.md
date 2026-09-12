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

Video mode picks two clips and runs the visual, temporal and audio layers from
`../cpp/video_fingerprint.hpp` / `../cpp/audio_fingerprint.hpp` on a worker
thread, showing a poster frame and the resolution / frame rate / duration of
each clip. Fingerprinting itself is concurrent (`../cpp/parallel_extract.hpp`):
the audio stage overlaps the video decode and the two clips decode in parallel,
so the wait is the longest single clip rather than the sum of all four stages.
The result panel reports the verdict plus the numbers behind it:
sampled and matched frames, the monotonic chain length, coverage, chain
completeness, mean frame similarity, and the audio similarity over the aligned
seconds. The frame-match threshold is adjustable and the audio layer can be
switched off. The audio layer needs the `ffmpeg` command line on `PATH`; without
it the verdict falls back to the visual/temporal result.

Batch video mode adds a folder, several clips or a drag-and-drop, then groups the
clips that are the same video (`../cpp/video_batch.hpp`) and marks the one to
keep with a star. The keep policy comes from a combo box: highest or lowest
resolution, largest or smallest file, newest or oldest modification date, or keep
everything. Each clip is decoded once on a thread pool, a compact signature
prefilters the pairs, and audio is extracted only for the clips that matched
visually, so a folder of long clips stays practical.

Batch list controls:

- Groups behave like Explorer groups and can be collapsed or expanded.
- Click a header to cycle ascending -> descending -> unsorted.
- Ctrl+click headers to add secondary and further sort fields.
- Right-click the header to show or hide columns; drag headers to reorder them.
- Enter opens the selected image, Delete removes it from the list, and Ctrl+F
  focuses the locate/search field.
- Double-click opens an image; the right-click menu provides copy path,
  properties, open containing folder, remove, delete and compare actions.
- Right-click a group row for the group menu: expand/collapse (this group or
  all), check/uncheck/invert the whole group, keep only the policy-best image,
  open every image in the group, open its folder, remove the group, remove or
  delete just the checked images, and start the comparison.
- Drag a column edge to resize it. The checkbox indicator stays left-aligned at
  the start of the 选中 cell, so it never drifts into the middle of a widened
  column; the column can shrink until the checkbox just fits and stops there,
  and the group/child tree indent is kept small so almost no space is wasted to
  its left. Widening 缩略图 scales the thumbnails up (each keeps its own aspect
  ratio) while that row's height grows with them, so no thumbnail is clipped.
  Group header rows keep their own height and never grow with the thumbnails.

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
