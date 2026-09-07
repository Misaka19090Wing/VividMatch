"""Command-line interface for VividMatch image fingerprints."""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path

from .fingerprint import DctImageFingerprint, best_match, compare_fingerprints


DEFAULT_THRESHOLD = 0.78


@dataclass(frozen=True)
class CompareResult:
    score: float
    match: bool
    threshold: float


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="vividmatch",
        description="Resolution-robust visual image matching with OpenCV DCT fingerprints.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    hash_parser = sub.add_parser("hash", help="Print the fingerprint of an image.")
    hash_parser.add_argument("image", type=Path)

    compare_parser = sub.add_parser("compare", help="Compare two images.")
    compare_parser.add_argument("first", type=Path)
    compare_parser.add_argument("second", type=Path)
    compare_parser.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD)

    search_parser = sub.add_parser("search", help="Find the closest image in a directory.")
    search_parser.add_argument("query", type=Path)
    search_parser.add_argument("directory", type=Path)
    search_parser.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD)
    search_parser.add_argument(
        "--extensions",
        nargs="+",
        default=[".jpg", ".jpeg", ".png", ".bmp", ".webp"],
        help="Candidate image extensions to scan.",
    )
    return parser


def hash_command(args: argparse.Namespace) -> int:
    fingerprint = DctImageFingerprint.from_file(args.image)
    payload = {
        "path": str(args.image),
        "source_size": fingerprint.source_size,
        "size": int(fingerprint.blocks.shape[0]),
        "hash": fingerprint.to_bytes().hex(),
    }
    print(json.dumps(payload, ensure_ascii=False, indent=2))
    return 0


def compare_command(args: argparse.Namespace) -> int:
    left = DctImageFingerprint.from_file(args.first)
    right = DctImageFingerprint.from_file(args.second)
    score = compare_fingerprints(left, right)
    result = CompareResult(
        score=score,
        match=score >= args.threshold,
        threshold=args.threshold,
    )
    print(
        json.dumps(
            {
                "first": str(args.first),
                "second": str(args.second),
                "score": round(result.score, 4),
                "threshold": result.threshold,
                "match": result.match,
            },
            ensure_ascii=False,
            indent=2,
        )
    )
    return 0


def search_command(args: argparse.Namespace) -> int:
    query = DctImageFingerprint.from_file(args.query)
    paths = [
        path
        for path in sorted(args.directory.iterdir())
        if path.suffix.lower() in {ext.lower() for ext in args.extensions}
    ]
    candidates: list[tuple[Path, DctImageFingerprint]] = []
    errors: list[str] = []
    for path in paths:
        try:
            candidates.append((path, DctImageFingerprint.from_file(path)))
        except ValueError as exc:
            errors.append(str(exc))
    matched = best_match(query, [fingerprint for _, fingerprint in candidates])
    if matched is None:
        print(
            json.dumps(
                {"query": str(args.query), "candidates": 0, "match": None},
                ensure_ascii=False,
                indent=2,
            )
        )
        return 0

    index, score = matched
    is_match = score >= args.threshold
    matched_path, _ = candidates[index]
    print(
        json.dumps(
            {
                "query": str(args.query),
                "candidates": len(candidates),
                "errors": errors,
                "match": str(matched_path),
                "score": round(score, 4),
                "threshold": args.threshold,
                "result": "match" if is_match else "no-match",
            },
            ensure_ascii=False,
            indent=2,
        )
    )
    return 0 if is_match else 1


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command == "hash":
        return hash_command(args)
    if args.command == "compare":
        return compare_command(args)
    if args.command == "search":
        return search_command(args)
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
