"""Generate the sample images used for manual GUI and batch-mode testing.

The set is deliberately shaped so that the batch mode has something to group:

* ``photo_a_800x600.png`` and ``photo_a_400x300.png`` are the *same* picture at
  two resolutions, so the matcher must put them in one 相似组.
* ``photo_tall_300x600.png`` is that same picture at a tall aspect ratio, so it
  belongs in the same 相似组 while also exercising thumbnail scaling and the
  per-row height in the batch list.
* ``photo_b_640x480.png`` is a clearly different picture, so it must end up in
  the 无相同图片 group.

The images land in ``samples/`` at the repository root; that folder is meant to
be handed straight to the GUI's "选择文件夹" button.

Only the project's own dependencies (OpenCV + NumPy) are used, so
``tests/test_samples.py`` can call :func:`write_samples` directly.

Run from the repository root:

    python tests/fixtures/make_fixtures.py
"""

from __future__ import annotations

import argparse
from pathlib import Path

import cv2
import numpy as np

DEFAULT_OUTPUT_DIR = Path(__file__).resolve().parents[2] / "samples"

# OpenCV draws in BGR order.
BLUE = (214, 108, 38)
SKY = (214, 108, 38)
SUN = (62, 206, 248)
HILL_NEAR = (84, 132, 46)
HILL_FAR = (68, 106, 34)
BLOCK = (60, 52, 198)
PAPER = (228, 236, 238)
INK = (32, 26, 24)
WINDOW = (86, 74, 70)
PAVEMENT = (138, 146, 150)


def _resize(image: np.ndarray, size: tuple[int, int]) -> np.ndarray:
    """Resize a drawn scene to the target (width, height)."""
    return cv2.resize(image, size, interpolation=cv2.INTER_AREA)


def _photo_a(size: tuple[int, int]) -> np.ndarray:
    """A recognisable scene: sky, sun, two hills, a red block."""
    width, height = size
    image = np.full((height, width, 3), BLUE, dtype=np.uint8)

    horizon = int(height * 0.55)
    cv2.rectangle(image, (0, 0), (width - 1, horizon), SKY, -1)

    radius = int(min(width, height) * 0.125)
    center = (int(width * 0.70), int(height * 0.20))
    cv2.circle(image, center, radius, SUN, -1)

    far_hill = np.array(
        [
            [0, height - 1],
            [int(width * 0.38), int(height * 0.46)],
            [int(width * 0.78), height - 1],
        ],
        dtype=np.int32,
    )
    cv2.fillPoly(image, [far_hill], HILL_FAR)

    near_hill = np.array(
        [
            [int(width * 0.45), height - 1],
            [int(width * 0.76), int(height * 0.60)],
            [width - 1, height - 1],
        ],
        dtype=np.int32,
    )
    cv2.fillPoly(image, [near_hill], HILL_NEAR)

    cv2.rectangle(
        image,
        (int(width * 0.10), int(height * 0.72)),
        (int(width * 0.34), int(height * 0.92)),
        BLOCK,
        -1,
    )
    return image


def _photo_b(size: tuple[int, int]) -> np.ndarray:
    """A clearly different picture: a dark band of windows above pavement."""
    width, height = size
    image = np.full((height, width, 3), PAPER, dtype=np.uint8)

    cv2.rectangle(image, (0, 0), (width - 1, int(height * 0.30)), INK, -1)
    for index in range(6):
        left = int(width * (index + 1) / 8) - int(width * 0.045)
        cv2.rectangle(
            image,
            (left, int(height * 0.30)),
            (left + int(width * 0.09), int(height * 0.68)),
            WINDOW,
            -1,
        )
    cv2.rectangle(
        image,
        (0, int(height * 0.86)),
        (width - 1, height - 1),
        PAVEMENT,
        -1,
    )
    return image


# name -> (scene builder, source size, output size)
SAMPLES = {
    "photo_a_800x600.png": (_photo_a, (800, 600), (800, 600)),
    "photo_a_400x300.png": (_photo_a, (800, 600), (400, 300)),
    "photo_tall_300x600.png": (_photo_a, (800, 600), (300, 600)),
    "photo_b_640x480.png": (_photo_b, (640, 480), (640, 480)),
}


def write_samples(output_dir: str | Path) -> list[Path]:
    """Write every sample image into output_dir and return the written paths."""
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    written: list[Path] = []
    for name, (builder, source_size, output_size) in SAMPLES.items():
        scene = builder(source_size)
        image = scene if source_size == output_size else _resize(scene, output_size)
        path = output_dir / name
        if not cv2.imwrite(str(path), image):
            raise RuntimeError(f"Could not write sample image: {path}")
        written.append(path)
    return written


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=DEFAULT_OUTPUT_DIR,
        help="Directory the sample images are written to (default: samples/).",
    )
    args = parser.parse_args()

    for path in write_samples(args.output):
        image = cv2.imread(str(path), cv2.IMREAD_COLOR)
        height, width = image.shape[:2]
        print(f"wrote {path} ({width}x{height})")


if __name__ == "__main__":
    main()
