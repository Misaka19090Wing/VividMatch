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

## Qt GUI (VividMatchGui)

A Qt 6 GUI based on the `XMuli/myapp-template` template lets the user choose
the image-compare mode from a function-selection page, pick two images, and
see the similarity result.

```bat
gui\build_gui.bat
gui\run_gui.bat
```

See `gui/README.md` for Qt/OpenCV paths and VS Code Qt extension setup.

## Python reference implementation

An equivalent Python/OpenCV implementation is also kept for experiments:

```bash
python -m vividmatch.cli compare first.png second.png
python -m vividmatch.cli search query.png candidates_dir/
python -m vividmatch.cli hash image.png
```
