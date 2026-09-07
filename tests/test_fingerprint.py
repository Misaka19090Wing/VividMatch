from __future__ import annotations

import unittest

import cv2
import numpy as np

from vividmatch.fingerprint import DctImageFingerprint, compare_fingerprints


def _synthetic_image(seed: int, width: int = 512, height: int = 512) -> np.ndarray:
    rng = np.random.default_rng(seed)
    image = np.zeros((height, width, 3), dtype=np.uint8)
    cv2.rectangle(image, (0, 0), (width - 1, height - 1), tuple(rng.integers(0, 255, 3).tolist()), -1)
    for _ in range(14):
        color = tuple(int(v) for v in rng.integers(0, 255, 3))
        kind = rng.integers(0, 4)
        center = (int(rng.integers(0, width)), int(rng.integers(0, height)))
        radius = int(rng.integers(10, max(20, width // 6)))
        if kind == 0:
            cv2.circle(image, center, radius, color, -1)
        elif kind == 1:
            pt2 = (
                int(min(width - 1, center[0] + radius)),
                int(min(height - 1, center[1] + radius)),
            )
            cv2.rectangle(image, center, pt2, color, -1)
        elif kind == 2:
            cv2.line(image, center, (int(center[0] + radius), int(center[1] - radius)), color, 3)
        else:
            pts = np.array(
                [
                    center,
                    (int(center[0] + radius), int(center[1] + radius // 2)),
                    (int(center[0] - radius // 2), int(center[1] + radius)),
                ],
                dtype=np.int32,
            )
            cv2.fillPoly(image, [pts], color)
    return image


def _resized_copy(image: np.ndarray, size: int, interpolation: int) -> np.ndarray:
    return cv2.resize(image, (size, size), interpolation=interpolation)


class FingerprintTests(unittest.TestCase):
    def test_same_image_across_resolutions_scores_high(self) -> None:
        base = _synthetic_image(1)
        variants = [
            base,
            _resized_copy(base, 64, cv2.INTER_AREA),
            _resized_copy(base, 96, cv2.INTER_LINEAR),
            _resized_copy(base, 256, cv2.INTER_AREA),
            _resized_copy(base, 1280, cv2.INTER_LINEAR),
            _resized_copy(base, 1920, cv2.INTER_CUBIC),
        ]
        fingerprints = [DctImageFingerprint.from_image(item) for item in variants]
        for left in fingerprints:
            for right in fingerprints:
                self.assertGreaterEqual(
                    compare_fingerprints(left, right),
                    0.80,
                    msg=f"same-image score too low: {compare_fingerprints(left, right):.3f}",
                )

    def test_different_images_score_low(self) -> None:
        base = _synthetic_image(2)
        fingerprint = DctImageFingerprint.from_image(base)
        for seed in range(3, 8):
            other = _synthetic_image(seed, width=480, height=720)
            self.assertLess(
                compare_fingerprints(fingerprint, DctImageFingerprint.from_image(other)),
                0.70,
                msg=f"different-image score too high for seed {seed}",
            )

    def test_binary_round_trip(self) -> None:
        fingerprint = DctImageFingerprint.from_image(_synthetic_image(9))
        restored = DctImageFingerprint.from_bytes(fingerprint.to_bytes(), fingerprint.source_size)
        self.assertEqual(restored.to_bytes(), fingerprint.to_bytes())


if __name__ == "__main__":
    unittest.main()
