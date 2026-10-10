"""Validate and apply only Astra's reviewed patch to the pinned Skia checkout."""

from pathlib import Path
import hashlib
import json
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "xmake/packages/skia-graphite"


def apply(source: Path) -> str:
    manifest = json.loads((PACKAGE / "glyph-cache.json").read_text(encoding="utf-8"))
    patch = PACKAGE / "glyph-cache.patch"
    digest = hashlib.sha256(patch.read_bytes()).hexdigest()
    lock = json.loads((ROOT / "dependencies.lock.json").read_text(encoding="utf-8"))
    pinned = lock["patches"]["skia-glyph-cache"]
    if pinned["path"] != "xmake/packages/skia-graphite/glyph-cache.patch" or \
            digest != manifest["sha256"] or digest != pinned["sha256"]:
        raise RuntimeError("Skia shaping patch hash mismatch")
    head = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if head != manifest["skia"]:
        raise RuntimeError("Skia shaping patch requires its pinned upstream revision")
    names = {entry["file"] for entry in manifest["files"]}
    dirty = subprocess.check_output(["git", "-C", str(source), "diff", "HEAD", "--name-only"], text=True).splitlines()
    if set(dirty) - names:
        raise RuntimeError("Unmanaged tracked modifications in Skia")
    states = []
    for entry in manifest["files"]:
        original = subprocess.check_output(["git", "-C", str(source), "show", "HEAD:" + entry["file"]])
        # Git's original blobs use LF; reject altered or partly applied copies.
        if hashlib.sha256(original).hexdigest() != entry["before"]:
            raise RuntimeError("Unexpected upstream Skia blob: " + entry["file"])
        actual = hashlib.sha256((source / entry["file"]).read_bytes().replace(b"\r\n", b"\n")).hexdigest()
        if actual not in (entry["before"], entry["after"]):
            raise RuntimeError("Unmanaged Skia shaping source: " + entry["file"])
        states.append(actual == entry["after"])
    if any(states) and not all(states):
        raise RuntimeError("Partly applied Skia shaping patch")
    if not all(states):
        subprocess.run(["git", "-C", str(source), "apply", "--check", str(patch)], check=True)
        subprocess.run(["git", "-C", str(source), "apply", str(patch)], check=True)
    for entry in manifest["files"]:
        actual = (source / entry["file"]).read_bytes().replace(b"\r\n", b"\n")
        if hashlib.sha256(actual).hexdigest() != entry["after"]:
            raise RuntimeError("Skia shaping patch output hash mismatch")
    return digest


if __name__ == "__main__":
    print(apply(Path(sys.argv[1]).resolve()))
