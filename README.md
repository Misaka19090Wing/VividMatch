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
  samples). A 128-byte per-clip signature — the majority vote of each sampled
  frame's block bits — is compared first, and only pairs above
  `kDefaultSignatureThreshold` get the full comparison. Measured separation:
  same content 0.97–1.00, different content 0.63–0.70, so the threshold sits in
  the gap and drops nothing real.
- **Audio is extracted lazily**, only for clips that take part in a visually
  matching pair, because it costs a separate ffmpeg process per clip.

A `partial` verdict deliberately does not merge a group: a shared opening or a
clip cut from a longer video is not the same video. `identical` and `reencoded`
both merge, since the latter is the same picture with the soundtrack swapped.

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

Both the CLI (`audio_skipped=`) and the GUI (`音频：未参与（...）`) name the reason,
so it is clear whether anything needs fixing:

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
