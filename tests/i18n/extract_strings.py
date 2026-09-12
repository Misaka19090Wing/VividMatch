"""Extract translatable strings from the GUI sources.

lupdate is not used: the 6.11 build available here rejects every .cpp file with
"has no recognized extension" even when -extensions cpp is passed, so it cannot
produce the .ts at all. The extraction is therefore done here.

Two things are produced:
  * a report of every (context, source) pair found, and
  * a check that each one has a Chinese translation in the given mapping file.

Run from the repository root:
    python tests/i18n/extract_strings.py report
    python tests/i18n/extract_strings.py check
"""

from __future__ import annotations

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
GUI_SRC = ROOT / "gui" / "src"

# File stem -> translation context. Qt's tr() resolves to the enclosing QObject
# class, which for these files is the file's class, so the stem is the context.
CONTEXT_BY_FILE = {
    "mainwin": "MainWin",
    "imagecomparepage": "ImageComparePage",
    "batchcomparepage": "BatchComparePage",
    "videocomparepage": "VideoComparePage",
    "videobatchpage": "VideoBatchPage",
}

# Strings reached through an explicit prefix rather than a bare tr(). Each entry
# maps the prefix to the context that prefix produces.
EXPLICIT_PREFIX_CONTEXTS = {
    "BatchComparePage::tr": "BatchComparePage",
    "VideoComparePage::tr": "VideoComparePage",
    "VideoBatchPage::tr": "VideoBatchPage",
    "ImageComparePage::tr": "ImageComparePage",
    "MainWin::tr": "MainWin",
}

# QCoreApplication::translate("ctx", "...") and its short forms. Only the two
# prefixes are matched; the string run after them is read separately so adjacent
# literals can be joined the way C++ and Qt join them.
TRANSLATE_RE = re.compile(
    r'(?:QCoreApplication|QObject)::translate\(\s*"([^"]+)"\s*,\s*',
    re.S,
)
# A bare tr("..."), optionally with a class prefix.
TR_RE = re.compile(
    r'(?<![A-Za-z0-9_:])(?:(?P<prefix>[A-Za-z_][A-Za-z0-9_]*)::)?\btr\(\s*',
    re.S,
)
NOOP_RE = re.compile(r'QT_TR_NOOP\(\s*', re.S)


def unescape(text: str) -> str:
    """Turn a C++ string literal body into the runtime string."""
    out = []
    i = 0
    while i < len(text):
        char = text[i]
        if char != "\\":
            out.append(char)
            i += 1
            continue
        i += 1
        if i >= len(text):
            break
        esc = text[i]
        mapping = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "'": "'"}
        if esc in mapping:
            out.append(mapping[esc])
            i += 1
        elif esc == "x":
            hex_digits = ""
            i += 1
            while i < len(text) and text[i] in "0123456789abcdefABCDEF" and len(hex_digits) < 2:
                hex_digits += text[i]
                i += 1
            out.append(chr(int(hex_digits, 16)) if hex_digits else "x")
        else:
            out.append(esc)
            i += 1
    return "".join(out)


def join_adjacent(text: str, start: int) -> tuple[str, int]:
    """Reads a run of adjacent C++ string literals starting at `start`.

    C++ joins `"a" "b"` into "ab", and Qt indexes the translation by the joined
    string, so the extractor has to join them the same way or the .ts would carry
    a truncated source and never match at run time.

    `start` must point at the opening quote of the first literal. Returns the
    joined body and the index just past the run.
    """
    parts: list[str] = []
    i = start
    while i < len(text) and text[i] == '"':
        i += 1  # skip the opening quote
        body_start = i
        while i < len(text):
            if text[i] == "\\":
                i += 2
                continue
            if text[i] == '"':
                break
            i += 1
        parts.append(text[body_start:i])
        i += 1  # skip the closing quote
        # Skip whitespace (including newlines) to see whether another literal
        # follows immediately, which is what makes it a single string.
        while i < len(text) and text[i] in " \t\r\n":
            i += 1
    return "".join(parts), i


def _literal_run(text: str, quote_index: int) -> str:
    joined, _ = join_adjacent(text, quote_index)
    return unescape(joined)


def strings_by_context() -> dict[str, set[str]]:
    """Every translatable string, grouped by the context Qt will look it up in."""
    found: dict[str, set[str]] = {}

    def add(context: str, source: str) -> None:
        if source:
            found.setdefault(context, set()).add(source)

    for path in sorted(GUI_SRC.rglob("*")):
        if path.suffix not in {".cpp", ".h"}:
            continue
        stem = path.stem
        default_context = CONTEXT_BY_FILE.get(stem)
        text = path.read_text(encoding="utf-8")

        # Strip comments so a commented-out tr() is not counted. Block comments
        # first, then line comments; neither form nests meaningfully here.
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        text = re.sub(r"//[^\n]*", "", text)

        for match in TRANSLATE_RE.finditer(text):
            add(match.group(1), _literal_run(text, match.end()))

        for match in NOOP_RE.finditer(text):
            if default_context:
                add(default_context, _literal_run(text, match.end()))

        for match in TR_RE.finditer(text):
            prefix = match.group("prefix")
            source = _literal_run(text, match.end())
            if prefix:
                context = EXPLICIT_PREFIX_CONTEXTS.get(f"{prefix}::tr")
                if context is None:
                    # Some other class's tr(), e.g. a header's helper.
                    continue
                add(context, source)
            elif default_context:
                add(default_context, source)

    return found


def report() -> int:
    found = strings_by_context()
    total = sum(len(v) for v in found.values())
    for context in sorted(found):
        print(f"{context}: {len(found[context])}")
    print(f"total: {total} unique strings")
    out = ROOT / "build-tmp" / "ts" / "extracted.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(
        json.dumps({k: sorted(v) for k, v in sorted(found.items())}, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    print(f"written: {out.relative_to(ROOT)}")
    return 0


def check() -> int:
    """Every extracted string must have a translation in the mapping file."""
    mapping_path = pathlib.Path(__file__).with_name("zh_CN.json")
    mapping = json.loads(mapping_path.read_text(encoding="utf-8"))
    found = strings_by_context()

    missing: list[str] = []
    for context, sources in sorted(found.items()):
        for source in sorted(sources):
            if not mapping.get(context, {}).get(source):
                missing.append(f"{context}: {source!r}")
    if missing:
        print(f"missing translations: {len(missing)}")
        for line in missing:
            print(f"  {line}")
        return 1

    # And the reverse: a mapping entry that no longer exists in the code.
    stale: list[str] = []
    for context, entries in sorted(mapping.items()):
        for source in sorted(entries):
            if source not in found.get(context, set()):
                stale.append(f"{context}: {source!r}")
    if stale:
        print(f"stale mapping entries: {len(stale)}")
        for line in stale:
            print(f"  {line}")
        return 1

    total = sum(len(v) for v in found.values())
    print(f"all {total} strings have translations, and no mapping entry is stale")
    return 0


if __name__ == "__main__":
    action = sys.argv[1] if len(sys.argv) > 1 else "report"
    if action == "report":
        raise SystemExit(report())
    if action == "check":
        raise SystemExit(check())
    raise SystemExit(f"unknown action: {action}")
