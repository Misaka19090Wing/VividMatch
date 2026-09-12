"""Build the Chinese translation files (i18n/vividmatch_zh_CN.ts and .qm).

English is the source language, so there is nothing to build for it: the strings
in the code are already English. Only the Chinese .ts/.qm are generated.

The .ts is written from tests/i18n/zh_CN.json rather than by lupdate, because the
lupdate build available here (6.11.0) rejects every .cpp file with "has no
recognized extension" even when -extensions cpp is passed, so it cannot produce
the file at all. tests/i18n/extract_strings.py does the extraction and checks that
the mapping covers every string and has no stale entry; this script turns that
mapping into the .ts and then calls lrelease for the .qm.

Run from the repository root:
    python tests/i18n/build_translations.py
"""

from __future__ import annotations

import json
import pathlib
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ElementTree

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT_DIR = ROOT / "i18n"
TS_NAME = "vividmatch_zh_CN.ts"
QM_NAME = "vividmatch_zh_CN.qm"

# Where lrelease may live, in the order they are tried. The build machine here
# has the Qt Linguist tools installed separately from Qt itself.
LRELEASE_CANDIDATES = [
    ROOT / "tools" / "lrelease.exe",
    pathlib.Path("E:/linguist_6.11.0/lrelease.exe"),
    pathlib.Path("C:/Qt/6.8.3/msvc2022_64/bin/lrelease.exe"),
]


def find_lrelease() -> pathlib.Path | None:
    for candidate in LRELEASE_CANDIDATES:
        if candidate.is_file():
            return candidate
    found = shutil.which("lrelease")
    return pathlib.Path(found) if found else None


def write_ts(mapping: dict[str, dict[str, str]]) -> pathlib.Path:
    """Writes the .ts from the mapping, in the order the contexts are listed."""
    root = ElementTree.Element(
        "TS",
        {
            "version": "2.1",
            "language": "zh_CN",
            "sourcelanguage": "en",
        },
    )
    for context_name in sorted(mapping):
        context = ElementTree.SubElement(root, "context")
        ElementTree.SubElement(context, "name").text = context_name
        for source in sorted(mapping[context_name]):
            message = ElementTree.SubElement(context, "message")
            ElementTree.SubElement(message, "source").text = source
            ElementTree.SubElement(message, "translation").text = mapping[context_name][source]

    # ElementTree writes no XML declaration by default; Qt's own files have one.
    body = ElementTree.tostring(root, encoding="unicode")
    text = '<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n' + body + "\n"

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    target = OUT_DIR / TS_NAME
    target.write_text(text, encoding="utf-8")
    return target


def main() -> int:
    mapping_path = pathlib.Path(__file__).with_name("zh_CN.json")
    mapping = json.loads(mapping_path.read_text(encoding="utf-8"))

    ts_path = write_ts(mapping)
    messages = sum(len(entries) for entries in mapping.values())
    print(f"wrote {ts_path.relative_to(ROOT)} ({messages} messages, "
          f"{len(mapping)} contexts)")

    lrelease = find_lrelease()
    if lrelease is None:
        # The application stays usable: English is the source language, so a
        # missing .qm only means no Chinese translation is offered.
        print("warning: lrelease was not found, so the .qm was not built.")
        print("         install the Qt Linguist tools (or put lrelease.exe in tools/)")
        print("         and run this script again to get the Chinese interface.")
        return 0

    qm_path = OUT_DIR / QM_NAME
    result = subprocess.run(
        [str(lrelease), str(ts_path), "-qm", str(qm_path)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    sys.stdout.write(result.stdout)
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        return result.returncode
    if not qm_path.is_file():
        print(f"error: {lrelease} reported success but wrote no {QM_NAME}")
        return 1
    print(f"wrote {qm_path.relative_to(ROOT)} ({qm_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
