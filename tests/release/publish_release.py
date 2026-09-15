"""Creates the GitHub release and uploads the portable zip.

The release body and assets cannot be sent in one call: the GitHub API creates a
release from JSON, and binary assets are uploaded afterwards to the upload_url it
returns. This does both steps, using only the standard library so nothing has to
be installed.

Run from the repository root, with GITHUB_TOKEN set:

    set GITHUB_TOKEN=ghp_xxx
    python tests/release/publish_release.py

The token needs "repo" scope. Nothing here is destructive: an existing release for
the tag is left alone unless --replace is passed.
"""

from __future__ import annotations

import argparse
import json
import mimetypes
import os
import pathlib
import sys
import urllib.error
import urllib.request

REPO = "Misaka19090Wing/VividMatch"
TAG = "v0.1.0"
RELEASE_NAME = "VividMatch 0.1.0"
API = "https://api.github.com"
API_VERSION = "2022-11-28"

ROOT = pathlib.Path(__file__).resolve().parents[2]
NOTES = ROOT / "dist" / "RELEASE_NOTES.md"
ASSETS = ["VividMatch-0.1.0-windows-x64.zip"]


def token() -> str:
    value = os.environ.get("GITHUB_TOKEN", "").strip()
    if not value:
        sys.exit("GITHUB_TOKEN is not set")
    return value


def request(method: str, url: str, bearer: str, *, data: bytes | None = None,
            content_type: str | None = None) -> tuple[int, object]:
    headers = {
        "Authorization": f"Bearer {bearer}",
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": API_VERSION,
        "User-Agent": "VividMatch-release-script",
    }
    if content_type:
        headers["Content-Type"] = content_type
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req) as response:
            body = response.read()
            return response.status, (json.loads(body) if body else None)
    except urllib.error.HTTPError as error:
        body = error.read().decode("utf-8", "replace")
        print(f"  HTTP {error.code} for {method} {url}", file=sys.stderr)
        print(f"  {body[:600]}", file=sys.stderr)
        raise


def find_release(bearer: str) -> dict | None:
    try:
        _, release = request("GET", f"{API}/repos/{REPO}/releases/tags/{TAG}", bearer)
        return release  # type: ignore[return-value]
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return None
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--replace", action="store_true",
                        help="delete and recreate an existing release for the tag")
    parser.add_argument("--dry-run", action="store_true",
                        help="report what would be sent and stop")
    args = parser.parse_args()

    bearer = token()

    if not NOTES.is_file():
        sys.exit(f"missing {NOTES}")
    body = NOTES.read_text(encoding="utf-8")

    missing = [name for name in ASSETS if not (ROOT / "dist" / name).is_file()]
    if missing:
        sys.exit("missing release assets: " + ", ".join(missing))

    print(f"tag:      {TAG}")
    print(f"release:  {RELEASE_NAME}")
    print(f"notes:    {len(body)} characters from {NOTES.name}")
    for name in ASSETS:
        size = (ROOT / "dist" / name).stat().st_size
        print(f"asset:    {name} ({size / 1024 / 1024:.1f} MB)")

    if args.dry_run:
        print("dry run: nothing sent")
        return 0

    existing = find_release(bearer)
    if existing is not None and not args.replace:
        print(f"a release for {TAG} already exists: {existing.get('html_url')}")
        print("pass --replace to delete and recreate it")
        return 0
    if existing is not None:
        print(f"deleting the existing release {existing['id']}...")
        _, deleted = request("DELETE", f"{API}/repos/{REPO}/releases/{existing['id']}",
                             bearer)
        print(f"  deleted: {deleted}")

    print("creating the release...")
    payload = json.dumps({
        "tag_name": TAG,
        "name": RELEASE_NAME,
        "body": body,
        "draft": False,
        "prerelease": False,
    }).encode("utf-8")
    _, release = request("POST", f"{API}/repos/{REPO}/releases", bearer,
                         data=payload, content_type="application/json")
    assert isinstance(release, dict)
    print(f"  {release.get('html_url')}")

    upload_url = release["upload_url"].split("{")[0]
    for name in ASSETS:
        path = ROOT / "dist" / name
        content_type = mimetypes.guess_type(name)[0] or "application/octet-stream"
        print(f"uploading {name}...")
        with path.open("rb") as handle:
            _, asset = request(
                "POST", f"{upload_url}?name={name}", bearer,
                data=handle.read(), content_type=content_type)
        assert isinstance(asset, dict)
        print(f"  {asset.get('browser_download_url')}")

    print("done")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
