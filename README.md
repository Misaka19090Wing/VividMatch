# VividMatch

VividMatch checks whether pictures or videos are visually the same across
different resolutions.

## Algorithm

Every input image is normalised to a 64x64 grayscale canvas, split into a 4x4
grid of 16 blocks, and each block is fingerprinted with a 2D DCT hash (64 bits
from the low-frequency coefficients). When two images are compared, the four
blocks with the largest Hamming distance are discarded and the remaining
twelve blocks are averaged. The resulting score is in `[0, 1]`, with `1`
meaning visually identical. This implements the visual fingerprint described
in `strategy.md`.

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

`cpp/video_fingerprint.hpp` and `cpp/audio_fingerprint.hpp` implement the visual,
temporal and audio layers of `strategy.md`. A video is reduced to one visual
fingerprint per second (the same block-DCT hash used for images); two videos are
matched frame by frame; the 时间轴单调性校验 runs on those matches; and the
soundtrack is compared only over the seconds that aligned visually:

```bash
cpp\vividmatch_video.exe compare first.mp4 second.mp4
cpp\vividmatch_video.exe compare first.mp4 second.mp4 0.78 --no-audio
cpp\vividmatch_video.exe summary clip.mp4
cpp\vividmatch_video_test.exe
```

The verdict is the 视听融合矩阵 from `strategy.md`, restricted to the cases this
implementation can tell apart:

| verdict | meaning |
| --- | --- |
| `identical` | the shorter video is matched by one monotonic chain and the soundtrack agrees: same content, only resolution / codec / bitrate differ. Also returned when the picture matches poorly but the sound matches well (the "audio veto" case for a heavily obscured picture) |
| `reencoded` | the picture matches but the soundtrack does not: 画面相同但BGM被替换 |
| `montage` | matches exist but cannot all sit on one monotonic chain (混剪拼接 or a rewind) |
| `partial` | the matched part is in order but does not cover the shorter video |
| `different` | no sampled frame matched |

`compare` exits `0` only for `identical`, so it can be used from a script. The
per-frame threshold (default `0.78`) can be passed as a third argument, and
`--no-audio` skips the audio layer. Frame sampling reads the video sequentially
rather than seeking, so two versions of a clip line up even at different frame
rates or resolutions.

Audio is decoded by the `ffmpeg` command line (found on `PATH`), because the
OpenCV build this project targets exposes no audio decoding API. The STFT uses a
small radix-2 FFT written in `audio_fingerprint.hpp` rather than FFTW, so OpenCV
stays the only library dependency. When `ffmpeg` is missing the audio layer
reports itself unavailable and the visual/temporal verdict stands alone.

## Qt GUI (VividMatchGui)

A Qt 6 GUI based on the `XMuli/myapp-template` template lets the user choose
the image-compare mode from a function-selection page, pick two images, and
see the similarity result. The home page also exposes a batch image-compare
mode for folders, multiple selections or drag-and-drop; results are grouped by
similarity in collapsible Explorer-style groups, and each duplicate group keeps
only the selected best image checked.

```bat
gui\build_gui.bat
gui\run_gui.bat
```

See `gui/README.md` for Qt/OpenCV paths and VS Code Qt extension setup.

To give the GUI to computers that do not have Qt/OpenCV installed, package a
portable copy once on a development machine:

```bat
gui\package_gui.bat
```

The portable app is written to `dist\VividMatchGui\` and can be run there
without installing Qt or OpenCV.

## Sample images

`samples/` holds generated pictures for exercising the tools by hand:

- `photo_a_800x600.png`, `photo_a_400x300.png` and `photo_tall_300x600.png` are
  the same picture at different sizes / aspect ratios, so they must land in one
  相似组 (they score 100% against each other).
- `photo_b_640x480.png` is a different picture, so it must land in the
  无相同图片 group (about 57-61% against the others).

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
