"""Windows-run provenance failure checks; never mutate downloaded source."""

from pathlib import Path
import importlib.util
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("skia_patch", ROOT / "scripts/apply-skia-patch.py")
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)
PACKAGE = ROOT / "xmake/packages/skia-graphite"
SOURCE = ROOT / ".tools/graphics/src/skia"
MANIFEST = json.loads((PACKAGE / "glyph-cache.json").read_text(encoding="utf-8"))
ORIGINALS = {
    entry["file"]: subprocess.check_output(["git", "-C", str(SOURCE), "show", "HEAD:" + entry["file"]])
    for entry in MANIFEST["files"]
}


class Provenance(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT / "build")
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.package = self.root / "package"
        self.package.mkdir()
        for name in ("glyph-cache.json", "glyph-cache.patch"):
            shutil.copyfile(PACKAGE / name, self.package / name)
        for entry in MANIFEST["files"]:
            file = self.source / entry["file"]
            file.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(SOURCE / entry["file"], file)
        self.dirty = ""
        self.revision = MANIFEST["skia"]
        self.patch_package = patch.object(adapter, "PACKAGE", self.package)
        self.patch_output = patch.object(adapter.subprocess, "check_output", side_effect=self.output)
        self.patch_run = patch.object(adapter.subprocess, "run")
        self.patch_package.start()
        self.patch_output.start()
        self.mutation = self.patch_run.start()

    def tearDown(self):
        self.patch_run.stop()
        self.patch_output.stop()
        self.patch_package.stop()
        self.temporary.cleanup()

    def output(self, arguments, **kwargs):
        if "rev-parse" in arguments:
            return self.revision + "\n"
        if "diff" in arguments:
            return self.dirty
        return ORIGINALS[arguments[-1].removeprefix("HEAD:")]

    def reject(self, message):
        with self.assertRaisesRegex(RuntimeError, message):
            adapter.apply(self.source)
        self.mutation.assert_not_called()

    def test_verified_patch_is_idempotent(self):
        self.assertEqual(adapter.apply(self.source), MANIFEST["sha256"])
        self.assertEqual(adapter.apply(self.source), MANIFEST["sha256"])
        self.mutation.assert_not_called()

    def test_changed_patch_is_rejected_before_mutation(self):
        with (self.package / "glyph-cache.patch").open("ab") as stream:
            stream.write(b"unreviewed change\n")
        self.reject("hash mismatch")

    def test_wrong_revision_is_rejected(self):
        self.revision = "0" * 40
        self.reject("pinned upstream revision")

    def test_changed_patch_and_manifest_still_require_the_lock(self):
        file = self.package / "glyph-cache.patch"
        file.write_bytes(file.read_bytes() + b"unreviewed change\n")
        manifest = dict(MANIFEST)
        manifest["sha256"] = hashlib.sha256(file.read_bytes()).hexdigest()
        (self.package / "glyph-cache.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.reject("hash mismatch")

    def test_unmanaged_source_is_rejected(self):
        self.dirty = "include/core/SkFont.h\n"
        self.reject("Unmanaged tracked")

    def test_modified_adapter_blob_is_rejected(self):
        with (self.source / MANIFEST["files"][0]["file"]).open("ab") as stream:
            stream.write(b"unreviewed change\n")
        self.reject("Unmanaged Skia shaping source")

    def test_partial_patch_is_rejected(self):
        name = MANIFEST["files"][0]["file"]
        (self.source / name).write_bytes(ORIGINALS[name])
        self.reject("Partly applied")


if __name__ == "__main__":
    unittest.main()
