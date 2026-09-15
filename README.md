# VividMatch

**English** | [简体中文](README.zh-CN.md)

> [!WARNING]
> **This project was built with DeepSeek AI.**
>
> The code, tests and documentation here were written with the assistance of
> DeepSeek AI. Treat it as machine-assisted work: review it yourself before
> relying on it for anything that matters. The algorithm and its measured
> numbers are documented below so the behaviour can be checked rather than
> taken on trust.

VividMatch checks whether pictures or videos are visually the same across
different resolutions.

## Algorithm

Every input image is normalised to a 64x64 grayscale canvas, split into a 4x4
grid of 16 blocks, and each block is fingerprinted with a 2D DCT hash (64 bits
from the low-frequency coefficients). When two images are compared, the four
blocks with the largest Hamming distance are discarded and the remaining
twelve blocks are averaged. The resulting score is in `[0, 1]`, with `1`
meaning visually identical.

That is the visual layer. The design has three, and they are deliberately
separable so each can be reasoned about and tested on its own:

| layer | what it does | why |
| --- | --- | --- |
| visual | block-DCT hash per frame, worst blocks discarded | resolution- and bitrate-robust, and tolerant of a watermark, a logo or a mosaic patch over part of the picture |
| temporal | one fingerprint per second, matched positions must advance monotonically | catches a spliced video: it matches individual frames but jumps around on the timeline |
| audio | RMS energy, spectral centroid and 16 band energies per second | resolution-independent, and the tie-breaker when the picture is heavily obscured |

A frame-level decision table combines them: pictures matching with the
soundtrack agreeing is the same video; pictures matching with the soundtrack
replaced is a re-cut; and a picture that matches poorly while the sound matches
well is still the same video, because the picture was probably obscured (the
"audio veto").

## C++ implementation (OpenCV 5)

The C++ image module lives in `cpp/`:

```bash
cpp\build_msvc.bat
cpp\vividmatch_image.exe compare first.png second.png
cpp\vividmatch_image.exe hash image.png
cpp\vividmatch_image_test.exe
```

`build_msvc.bat` accepts the OpenCV root as its first argument. The default
threshold is `0.78`.

### Video comparison

`cpp/video_fingerprint.hpp` and `cpp/audio_fingerprint.hpp` implement the
visual, temporal and audio layers. A video is reduced to one visual fingerprint
per second (the same block-DCT hash used for images); two videos are matched
frame by frame; the monotonicity check runs on those matches; and the
soundtrack is compared only over the seconds that aligned visually:

```bash
cpp\vividmatch_video.exe compare first.mp4 second.mp4
cpp\vividmatch_video.exe compare first.mp4 second.mp4 0.78 --no-audio
cpp\vividmatch_video.exe summary clip.mp4
cpp\vividmatch_video.exe batch <folder-or-clip> [more...]
cpp\vividmatch_video_test.exe
cpp\vividmatch_video_batch_test.exe
```

### Batch video comparison

`cpp/video_batch.hpp` fingerprints a folder of clips, works out which of them are
the same video, and picks one to keep per group.

```bash
cpp\vividmatch_video.exe batch "D:\clips"
```

The design follows where the time actually goes, measured on real clips:

- **Extraction dominates.** Five 20-second clips take ~445 ms to decode but only
  ~1.2 ms to compare in all 10 pairs. Each clip is therefore decoded exactly once,
  on a thread pool.
- **Comparing every pair still grows quadratically**, in both the clip count and
  the clip length (sampling is once per second, so an hour-long clip is ~3600
  samples). A 128-byte per-clip signature - the majority vote of each sampled
  frame's block bits - is compared first, and only pairs above
  `kDefaultSignatureThreshold` get the full comparison. Measured separation:
  same content 0.97-1.00, different content 0.63-0.70, so the threshold sits in
  the gap and drops nothing real.
- **Audio is extracted lazily**, only for clips that take part in a visually
  matching pair, because it costs a separate ffmpeg process per clip.

A `partial` verdict deliberately does not merge a group: a shared opening or a
clip cut from a longer video is not the same video. `identical` and `reencoded`
both merge, since the latter is the same picture with the soundtrack swapped.

The verdicts, restricted to the cases this implementation can tell apart:

| verdict | meaning |
| --- | --- |
| `identical` | the shorter video is matched by one monotonic chain and the soundtrack agrees: same content, only resolution / codec / bitrate differ. Also returned when the picture matches poorly but the sound matches well (the "audio veto" case for a heavily obscured picture) |
| `reencoded` | the picture matches but the soundtrack does not: the same picture with a replaced music bed |
| `montage` | matches exist but cannot all sit on one monotonic chain (splicing or a rewind) |
| `partial` | the matched part is in order but does not cover the shorter video |
| `different` | no sampled frame matched |

`compare` exits `0` only for `identical`, so it can be used from a script. The
per-frame threshold (default `0.78`) can be passed as a third argument, and
`--no-audio` skips the audio layer. Frame sampling reads the video sequentially
rather than seeking, so two versions of a clip line up even at different frame
rates or resolutions.

`cpp/parallel_extract.hpp` runs the two halves of fingerprinting at the same
time: the audio stage overlaps the video decode, and the two videos decode on
separate threads. That measures 1.4x end to end on a 3 minute 720p pair, with
byte-identical fingerprints and the same verdict. Hardware decoding was measured
and deliberately not used - this OpenCV build cannot open MP4 through Media
Foundation or DirectShow, and asking the FFMPEG backend for hardware
acceleration makes reading *slower* than software.

Audio is decoded by the `ffmpeg` command line (found on `PATH`), because the
OpenCV build this project targets exposes no audio decoding API. The STFT uses a
small radix-2 FFT written in `audio_fingerprint.hpp` rather than FFTW, so OpenCV
stays the only library dependency.

### When the audio layer sits out

Both the CLI (`audio_skipped=`) and the GUI (`Audio: not used (...)` in English, or
the equivalent in Chinese) name the reason, so it is clear whether anything needs
fixing:

| message | cause | what to do |
| --- | --- | --- |
| `this file has no audio track` | one of the clips is silent (screen recording, muted export, GIF-style source) | nothing - the verdict falls back to picture and timing |
| `ffmpeg not found on PATH` | `ffmpeg.exe` is not installed or not on `PATH` | install ffmpeg (e.g. `winget install Gyan.FFmpeg`), reopen the app so it picks up the new `PATH`, or pass the full path to `fingerprintAudio` / `compareVideoFiles(..., ffmpegPath)` |
| `the file could not be read` | truncated or unsupported container | re-export the clip, or check it plays in a media player |
| `ffmpeg was denied access` | antivirus or folder permissions blocking ffmpeg on the file or the temp folder | allow ffmpeg in the antivirus, or fix the temp folder permissions |
| `no aligned frames to compare audio on` | the pictures never matched, so there is no time correspondence | nothing - audio is compared only over the seconds that aligned visually |

The names of the clips are not needed to diagnose this: the audio layer reports
per file, so a single silent clip in the pair is enough to trigger the first row.

## Qt GUI (VividMatchGui)

A Qt 6 GUI based on the `XMuli/myapp-template` template lets the user choose a
comparison mode from a function-selection page. There are four:

| mode | what it does |
| --- | --- |
| Image comparison | pick two images, see the similarity result |
| Batch image comparison | add a folder, several files or a drag-and-drop; results are grouped by similarity in collapsible Explorer-style groups, and each duplicate group keeps only the selected best image checked |
| Video comparison | pick two clips and compare picture, timing and sound |
| Batch video comparison | add a folder of clips, group the ones that are the same video, and pick one to keep per group |

All four are also listed in the **File** menu.

### Language

The interface is bilingual: **English** and **Simplified Chinese**. English is the
source language, so the strings in the code are English and the Chinese text lives
in `i18n/vividmatch_zh_CN.ts` compiled to `vividmatch_zh_CN.qm`. That ordering is
deliberate: if the `.qm` is missing or cannot be loaded the application stays in
English rather than falling back to Chinese, which a reader may not know.

On first run the language follows the operating system (`QLocale::system()`); a
`zh*` system gets Chinese, everything else English. **File → Language** overrides
it with *Follow the system*, *English* or *Simplified Chinese*, and the choice is
remembered in `QSettings`.

The new language takes effect on the **next start**: switching shows a notice
saying so. Every page does implement `changeEvent()` and rebuilds its own strings
when Qt sends `LanguageChange`, but that is not dependable across all panels, so
the notice asks for a restart rather than claiming the interface has already
changed.

```bat
gui\build_gui.bat
```

builds the translation as part of the normal build. It needs `lrelease` from the
Qt Linguist tools; without it the build warns and produces an English-only
application, which is still fully usable.

Two helpers keep the translation honest (`tests/i18n/`):

| command | what it does |
| --- | --- |
| `python tests/i18n/extract_strings.py check` | every `tr()` string has a translation, and no mapping entry is stale |
| `python tests/i18n/build_translations.py` | writes `i18n/*.ts` from `tests/i18n/zh_CN.json` and compiles the `.qm` |

`lupdate` is deliberately not used: the 6.11 build available here rejects every
`.cpp` with *"has no recognized extension"*, even with `-extensions cpp`, so it
cannot produce the file at all. The extractor joins adjacent string literals the
way C++ and Qt do, which matters because several `tr()` calls span lines.

```bat
gui\build_gui.bat
gui\run_gui.bat
```

See `gui/README.md` ([简体中文](gui/README.zh-CN.md)) for Qt/OpenCV paths and
VS Code Qt extension setup.

To give the GUI to computers that do not have Qt/OpenCV installed, package a
portable copy once on a development machine:

```bat
gui\package_gui.bat
```

The portable app is written to `dist\VividMatchGui\` and can be run there
without installing Qt or OpenCV.

## Releases

The version is `0.1.0`, in three places that have to move together:
`gui/CMakeLists.txt` (`project(... VERSION ...)`), the footer label in
`gui/src/ui/main/mainwin.cpp`, and `vividmatch/__init__.py`.

To publish a release:

```bat
gui\build_gui.bat
gui\package_gui.bat
release_publish.bat
```

`package_gui.bat` writes `dist\VividMatchGui\`; for 0.1.0 that folder was zipped as
`dist\VividMatch-0.1.0-windows-x64.zip` (40 MB, and 106 MB unpacked). Then
`release_publish.bat` pushes the `v0.1.0` tag and creates the GitHub release with
the zip attached. It needs a `GITHUB_TOKEN` with `repo` scope.

```bash
python tests/release/publish_release.py --dry-run
```

reports exactly what would be sent without sending anything. The release body comes
from `dist/RELEASE_NOTES.md`, which sits next to the zip; `package_gui.bat` does not
generate it.

## Sample images

`samples/` holds generated pictures for exercising the tools by hand:

- `photo_a_800x600.png`, `photo_a_400x300.png` and `photo_tall_300x600.png` are
  the same picture at different sizes / aspect ratios, so they must land in one
  duplicate group (they score 100% against each other).
- `photo_b_640x480.png` is a different picture, so it must land in the
  no-duplicates group (about 57-61% against the others).

Regenerate them with:

```bash
python tests/fixtures/make_fixtures.py
```

`tests/test_samples.py` re-derives the same set into a temporary directory and
asserts on that grouping, so the samples cannot drift into something useless.

## Python reference implementation

An equivalent Python/OpenCV implementation is also kept for experiments:

```bash
python -m vividmatch.cli compare first.png second.png
python -m vividmatch.cli search query.png candidates_dir/
python -m vividmatch.cli hash image.png
```

## Licence

MIT; see [`LICENSE`](LICENSE).

`THIRD-PARTY-NOTICES.md` records what the project borrows and bundles: the GUI's
window/page structure and CMake layout come from
[XMuli/myapp-template](https://github.com/XMuli/myapp-template), and the portable
build in `dist/` ships Qt and OpenCV as separate dynamic libraries under their own
terms.
