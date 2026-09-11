"""The generated sample set must keep the shape the batch mode relies on.

The samples in ``samples/`` are the images used to exercise batch mode by hand;
this test regenerates them into a temporary directory and asserts on the
grouping the matcher produces, so the set cannot silently drift into something
useless (for example all-different or all-identical images).
"""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

from vividmatch.fingerprint import DctImageFingerprint, compare_fingerprints

THRESHOLD = 0.78

_FIXTURE_MODULE = Path(__file__).resolve().parent / "fixtures" / "make_fixtures.py"


def _load_fixture_module():
    spec = importlib.util.spec_from_file_location("make_fixtures", _FIXTURE_MODULE)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class SampleFixtureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls._temp_dir = tempfile.TemporaryDirectory()
        module = _load_fixture_module()
        cls.paths = module.write_samples(cls._temp_dir.name)
        cls.fingerprints = {
            path.name: DctImageFingerprint.from_file(path) for path in cls.paths
        }

    @classmethod
    def tearDownClass(cls) -> None:
        cls._temp_dir.cleanup()

    def _score(self, left: str, right: str) -> float:
        return compare_fingerprints(self.fingerprints[left], self.fingerprints[right])

    def test_all_expected_images_are_written(self) -> None:
        self.assertEqual(
            sorted(self.fingerprints),
            [
                "photo_a_400x300.png",
                "photo_a_800x600.png",
                "photo_b_640x480.png",
                "photo_tall_300x600.png",
            ],
        )

    def test_same_picture_at_other_resolutions_groups_together(self) -> None:
        # Batch mode must report a 相似组 containing these three.
        for left, right in (
            ("photo_a_800x600.png", "photo_a_400x300.png"),
            ("photo_a_800x600.png", "photo_tall_300x600.png"),
            ("photo_a_400x300.png", "photo_tall_300x600.png"),
        ):
            score = self._score(left, right)
            self.assertGreaterEqual(
                score, THRESHOLD, msg=f"{left} vs {right} scored {score:.3f}"
            )

    def test_different_picture_stays_separate(self) -> None:
        for other in (
            "photo_a_800x600.png",
            "photo_a_400x300.png",
            "photo_tall_300x600.png",
        ):
            score = self._score("photo_b_640x480.png", other)
            self.assertLess(
                score, THRESHOLD, msg=f"photo_b vs {other} scored {score:.3f}"
            )

    def test_tall_sample_keeps_a_tall_aspect_ratio(self) -> None:
        fingerprint = self.fingerprints["photo_tall_300x600.png"]
        width, height = fingerprint.source_size
        self.assertGreater(height, width, msg=f"expected a tall image, got {width}x{height}")


if __name__ == "__main__":
    unittest.main()
