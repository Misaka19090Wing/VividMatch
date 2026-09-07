"""VividMatch: visual near-duplicate detection across resolutions."""

from .fingerprint import DctImageFingerprint, compare_fingerprints

__all__ = ["DctImageFingerprint", "compare_fingerprints"]
__version__ = "0.1.0"
