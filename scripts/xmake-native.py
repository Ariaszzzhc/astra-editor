#!/usr/bin/env python3
"""Pinned recipes, grammars and local caches for an installed native xmake 3.1.1."""
from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import sys

from native_release import REPO, archive, check_hash, child, extract, inventory, native_host, read_json, run, verify


def main():
    host = native_host()
    arguments = sys.argv[1:]
    offline = "--offline" in arguments
    arguments = [value for value in arguments if value != "--offline"]
    lock = read_json(REPO / "dependencies.lock.json")
    local = REPO / ".tools/xmake/bin/xmake"
    executable = str(local) if local.is_file() else shutil.which("xmake")
    if not executable:
        raise RuntimeError("Install native xmake " + lock["xmake"]["version"] + " or put it in .tools/xmake/bin/xmake")
    version = run(executable, "--version", capture=True)
    if not re.search(r"\bxmake v" + re.escape(lock["xmake"]["version"]) + r"(?:\D|$)", version):
        raise RuntimeError("The native xmake version does not match dependencies.lock.json")
    revision = lock["recipes"]["revision"]
    recipe_asset = {"archive": "https://codeload.github.com/xmake-io/xmake-repo/zip/" + revision,
                    "sha256": lock["recipes"]["archiveSha256"]}
    recipe = child(REPO, REPO / ".tools/recipes" / ("xmake-repo-" + revision))
    bundle = archive(recipe_asset, offline, [REPO / ".tools/downloads" / ("xmake-repo-" + revision + ".zip")])
    if not recipe.exists():
        extract(bundle, recipe, "xmake-repo-" + revision + "/")
        inventory(recipe, ".astra-native-inventory.json", {"schema": 1, "revision": revision})
    if not (recipe / ".astra-native-inventory.json").is_file():
        raise RuntimeError("Existing recipe tree has no native inventory; prepare in a clean native checkout")
    verify(recipe, ".astra-native-inventory.json")
    for grammar in read_json(REPO / "grammars/manifest.json")["packages"]:
        source = child(REPO, REPO / ".tools/grammars" / grammar["directory"])
        bundle = archive(grammar, offline, [REPO / ".tools/grammars" / (grammar["name"] + ".zip")])
        if not source.exists():
            extract(bundle, source, grammar["directory"] + "/")
        for name, digest in grammar["files"].items():
            check_hash(child(source, source / name), digest)
    environment = dict(os.environ)
    environment.update(XMAKE_GLOBALDIR=str(REPO / ".tools/xmake-state" / "-".join(host)),
                       XMAKE_PKG_CACHEDIR=str(REPO / ".tools/package-cache"),
                       XMAKE_PKG_INSTALLDIR=str(REPO / ".tools/packages"), XMAKE_COLORTERM="nocolor")
    run(executable, *arguments, env=environment)


if __name__ == "__main__":
    main()
