#!/usr/bin/env python3
"""Prepare commit-pinned parser sources without requiring a generator."""
import argparse

from native_release import REPO, archive, check_hash, child, extract, read_json


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--offline", action="store_true")
    options = parser.parse_args()
    for grammar in read_json(REPO / "grammars/manifest.json")["packages"]:
        source = child(REPO, REPO / ".tools/grammars" / grammar["directory"])
        bundle = archive(grammar, options.offline, [REPO / ".tools/grammars" / (grammar["name"] + ".zip")])
        if not source.exists():
            extract(bundle, source, grammar["directory"] + "/")
        for name, digest in grammar["files"].items():
            check_hash(child(source, source / name), digest)
        print(f"Verified {grammar['name']} {grammar['commit']}")


if __name__ == "__main__":
    main()
