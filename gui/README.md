# VividMatch GUI

**English** | [简体中文](README.zh-CN.md)

> [!WARNING]
> **This project was built with DeepSeek AI.**
>
> The code, tests and documentation here were written with the assistance of
> DeepSeek AI. Treat it as machine-assisted work and review it yourself before
> relying on it. Behaviour that is easy to get wrong (column resizing, the
> enabled state of buttons, when rows are rebuilt) is called out below so it can
> be checked rather than assumed.

Qt 6 Widgets desktop GUI, based on the window/page structure and CMake layout
of [XMuli/myapp-template](https://github.com/XMuli/myapp-template) (MIT).

The GUI opens on a function-selection home page offering four comparison modes,
all four of which are also listed in the **File** menu.

## Language

The interface is bilingual. English is the source language, so the strings in the
code are English and the Chinese text lives in `../i18n/vividmatch_zh_CN.qm`; if
that file is missing the interface stays English, which is why it is the source.

- On first run the language follows the operating system's UI language.
- **File → Language** offers *Follow the system*, *English* and
  *Simplified Chinese*; the choice is stored with `QSettings`, and a notice says it
  takes effect on the next start.
- Every page implements `changeEvent()` and rebuilds its own strings on a
  `LanguageChange` event, and strings that embed numbers (the result panels, the
  group headings, the status line) are regenerated from stored state rather than
  re-translated. That is a best effort, not a guarantee across every panel, which
  is why the notice asks for a restart instead of promising an instant switch.

`gui\build_gui.bat` builds the translation as part of the build and needs
`lrelease` from the Qt Linguist tools. Without it the build warns and produces an
English-only application, which still works.

`LanguageManager` looks for the `.qm` next to the executable (in `translations/`,
`i18n/` or the executable's own directory) and one level up, so it works both from
the build tree and from the portable package.

## Image mode

Selecting image-compare mode opens a page where the user chooses two image files
and runs the OpenCV DCT fingerprint comparison from
`../cpp/visual_fingerprint.hpp`.

Images can be selected with the buttons or dragged from the file manager and
dropped onto the matching preview panel.

## Batch image mode

Batch mode accepts a whole folder, multiple image files, or drag-and-drop.
Every row shows a thumbnail, file name, resolution, type, size, modification
date, path and bit depth. After comparison, duplicate groups are ordered by
similarity at the top; the selection policy decides which image of each group
stays checked (default: highest resolution). The "keep everything" policy keeps
every image in a duplicate group checked.

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
  the start of the Selected cell, so it never drifts into the middle of a widened
  column; the column can shrink until the checkbox just fits and stops there,
  and the group/child tree indent is kept small so almost no space is wasted to
  its left. Widening the Thumbnail column scales the thumbnails up (each keeps its
  own aspect ratio) while that row's height grows with them, so no thumbnail is
  clipped. Group header rows keep their own height and never grow with the
  thumbnails.

## Video mode

Video mode picks two clips and runs the visual, temporal and audio layers from
`../cpp/video_fingerprint.hpp` / `../cpp/audio_fingerprint.hpp` on a worker
thread, showing a poster frame and the resolution / frame rate / duration of
each clip. Fingerprinting itself is concurrent (`../cpp/parallel_extract.hpp`):
the audio stage overlaps the video decode and the two clips decode in parallel,
so the wait is the longest single clip rather than the sum of all four stages.
The result panel reports the verdict plus the numbers behind it: sampled and
matched frames, the monotonic chain length, coverage, chain completeness, mean
frame similarity, and the audio similarity over the aligned seconds. The
frame-match threshold is adjustable and the audio layer can be switched off. The
audio layer needs the `ffmpeg` command line on `PATH`; without it the verdict
falls back to the visual/temporal result.

## Batch video mode

Batch video mode adds a folder, several clips or a drag-and-drop, then groups the
clips that are the same video (`../cpp/video_batch.hpp`) and marks the one to
keep with a star. The keep policy comes from a combo box: highest or lowest
resolution, largest or smallest file, newest or oldest modification date, or keep
everything. Each clip is decoded once on a thread pool, a compact signature
prefilters the pairs, and audio is extracted only for the clips that matched
visually, so a folder of long clips stays practical.

It follows the batch image page: a Selected checkbox column, a Frame preview
column, a right-click menu on both clips and groups, a Ctrl+F locator, and the same
select-all / invert / remove-checked / delete-checked actions.

Behaviour worth knowing:

- Clips appear in the list the moment they are added. Resolution and duration
  show as a dash until the background probe has opened the clip, which supplies
  both along with the preview frame; a comparison is not needed for them.
- The Frame position spin box sets how far into each clip the preview is taken
  (default 50%, the middle, because the first frame is often a title card or a fade
  from black). It applies on **Re-grab frames** rather than on every keystroke,
  since each grab seeks and decodes.
- The preview scales with the Frame column width and the row height follows it, the
  same way thumbnails behave on the batch image page.
- **Start comparison** follows the ticks, not the list length: it is enabled only
  while at least two clips are checked, and the clips it compares are exactly the
  checked ones. Unchecking everything disables it rather than quietly re-running the
  whole list.
- Results are replaced by each run, but clips that did not take part (unchecked,
  or added afterwards) stay listed under a waiting group instead of disappearing.

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
