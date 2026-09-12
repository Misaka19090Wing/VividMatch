"""Resolution-robust visual fingerprints built on OpenCV DCT.

This mirrors `cpp/visual_fingerprint.hpp` for still images:
1. Normalise every image to a fixed 64x64 grayscale canvas.
2. Split the canvas into a 4x4 grid of 16 non-overlapping blocks.
3. Run a 2D DCT on every block and encode the low-frequency coefficients
   into a 64-bit hash.
4. Compare two images block-by-block, discard the four blocks with the
   largest Hamming distance, and average the remaining twelve blocks.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

import cv2
import numpy as np

IMAGE_SIZE = 64
GRID = 4
BLOCK_SIZE = IMAGE_SIZE // GRID
HASH_SIZE = 64
DISCARD_BLOCKS = 4


def _as_gray(image: np.ndarray) -> np.ndarray:
    if image.ndim == 2:
        gray = image
    elif image.ndim == 3 and image.shape[2] in (3, 4):
        gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    else:
        raise ValueError(f"Unsupported image shape: {image.shape}")
    return gray.astype(np.float32, copy=False)


def _resize_for_fingerprint(gray: np.ndarray) -> np.ndarray:
    """Normalise input pixels to the fixed fingerprint canvas."""
    # INTER_AREA averages pixel neighbourhoods when shrinking; it is not suited
    # to enlarging very small sources, so switch to bilinear interpolation then.
    interpolation = (
        cv2.INTER_LINEAR
        if gray.shape[0] < IMAGE_SIZE and gray.shape[1] < IMAGE_SIZE
        else cv2.INTER_AREA
    )
    return cv2.resize(gray, (IMAGE_SIZE, IMAGE_SIZE), interpolation=interpolation)


def _block_dct_hash(block: np.ndarray) -> np.ndarray:
    """Encode one block as 64 bits derived from its DCT spectrum."""
    dct = cv2.dct(block.astype(np.float32))
    # Keep the low-frequency 8x8 area, where resampling changes are smallest.
    low_freq = dct[:8, :8].ravel()
    median = float(np.median(low_freq))
    bits = low_freq > median
    packed = np.packbits(bits.astype(np.uint8))
    return packed


def _unpack(hash_bytes: np.ndarray) -> np.ndarray:
    bits = np.unpackbits(hash_bytes)
    return bits.reshape((-1, HASH_SIZE))


@dataclass(frozen=True)
class DctImageFingerprint:
    """DCT fingerprint with 16 block hashes, each 64 bits wide."""

    blocks: np.ndarray
    source_size: tuple[int, int]

    @classmethod
    def from_file(cls, path: str | Path) -> "DctImageFingerprint":
        image = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if image is None:
            raise ValueError(f"Cannot decode image: {path}")
        return cls.from_image(image)

    @classmethod
    def from_image(cls, image: np.ndarray) -> "DctImageFingerprint":
        gray = _resize_for_fingerprint(_as_gray(image))
        blocks: list[np.ndarray] = []
        for row in range(GRID):
            for col in range(GRID):
                tile = gray[
                    row * BLOCK_SIZE : (row + 1) * BLOCK_SIZE,
                    col * BLOCK_SIZE : (col + 1) * BLOCK_SIZE,
                ]
                blocks.append(_block_dct_hash(tile))
        return cls(blocks=np.stack(blocks), source_size=(image.shape[1], image.shape[0]))

    def to_bytes(self) -> bytes:
        return self.blocks.tobytes()

    @classmethod
    def from_bytes(
        cls, raw: bytes, source_size: tuple[int, int] = (0, 0)
    ) -> "DctImageFingerprint":
        blocks = np.frombuffer(raw, dtype=np.uint8).reshape((GRID * GRID, HASH_SIZE // 8))
        return cls(blocks=blocks, source_size=source_size)


def _hamming(a: np.ndarray, b: np.ndarray) -> float:
    return float(np.count_nonzero(a != b))


def compare_fingerprints(
    left: DctImageFingerprint,
    right: DctImageFingerprint,
    *,
    discard: int = DISCARD_BLOCKS,
) -> float:
    """Return similarity in [0, 1], where 1 means visually identical.

    The four least similar blocks are ignored so partial occlusion or local
    encoding artefacts do not dominate the result.
    """

    left_bits = _unpack(left.blocks)
    right_bits = _unpack(right.blocks)
    distances = np.array(
        [_hamming(left_bits[i], right_bits[i]) for i in range(left_bits.shape[0])],
        dtype=np.float32,
    )
    if distances.size == 0:
        return 0.0
    kept = np.sort(distances)[: max(1, distances.size - discard)]
    # Hamming distance per block is at most 64.
    return float(np.mean(1.0 - kept / HASH_SIZE))


def fingerprint_sequence(
    paths: Iterable[str | Path],
) -> list[DctImageFingerprint]:
    return [DctImageFingerprint.from_file(path) for path in paths]


def best_match(
    query: DctImageFingerprint,
    candidates: Sequence[DctImageFingerprint],
) -> tuple[int, float] | None:
    if not candidates:
        return None
    scores = [compare_fingerprints(query, candidate) for candidate in candidates]
    index = int(np.argmax(scores))
    return index, float(scores[index])
