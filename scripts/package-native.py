#!/usr/bin/env python3
"""Assemble/verify an Astra package on its native Linux/macOS host only."""
from __future__ import annotations

import argparse
import gzip
from pathlib import Path
import re
import shutil
import tarfile
import uuid

from native_release import REPO, check_hash, child, inventory, native_binary, native_host, read_json, run, sha256, verify, write_json


def main():
    platform, architecture = native_host()
    key = platform + "-" + architecture
    xmake_architecture = "x86_64" if architecture == "x64" else "arm64"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-directory", type=Path, default=Path(f"build/{platform}/{xmake_architecture}/release"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--verify", type=Path, help="Verify an existing package without launching it")
    parser.add_argument("--no-archive", action="store_true")
    args = parser.parse_args()
    if args.verify:
        destination = child(REPO / "build", REPO / args.verify)
        manifest = verify(destination, "package-manifest.json")
        if (manifest["platform"], manifest["architecture"]) != (platform, architecture):
            raise RuntimeError("Cross-platform package verification is forbidden")
        require_payload(destination, platform, architecture)
        print(f"Native package inventory verified: {destination}")
        return
    version = re.search(r'set_version\("([0-9]+\.[0-9]+\.[0-9]+)"\)', (REPO / "xmake.lua").read_text()).group(1)
    name = "astra-" + version + "-" + key
    output = child(REPO / "build", REPO / (args.output or Path("build/packages") / name))
    if output.exists() or Path(str(output) + ".tar.gz").exists():
        raise RuntimeError("Output already exists; choose another --output")
    build = child(REPO / "build", REPO / args.build_directory)
    pointer = read_json(REPO / ".tools/runtime" / (key + ".json"))
    runtime = child(REPO / ".tools/runtime", REPO / pointer["directory"])
    check_hash(runtime / "runtime-manifest.json", pointer["manifestSha256"])
    runtime_manifest = verify(runtime, "runtime-manifest.json")
    if (runtime_manifest["platform"], runtime_manifest["architecture"]) != (platform, architecture):
        raise RuntimeError("Prepared runtime belongs to another native host")
    graphics = REPO / f".tools/graphics/install/{platform}/{architecture}/release"
    graphics_manifest = read_json(graphics / "manifest.json")
    if (graphics_manifest["platform"], graphics_manifest["architecture"]) != (platform, architecture) or graphics_manifest["configuration"] != "Release":
        raise RuntimeError("Release graphics package required")
    staging = child(REPO / "build", output.with_name(output.name + ".staging-" + uuid.uuid4().hex))
    try:
        shutil.copytree(runtime, staging, symlinks=True)
        shutil.copytree(REPO / "data", staging / "data")
        inputs = {}
        for executable in ("astra", "astra-server"):
            inputs[executable] = sha256(build / executable)
            shutil.copy2(build / executable, staging / executable)
            check_hash(build / executable, inputs[executable])
        sdl = sorted(build.glob("libSDL3*.dylib" if platform == "macosx" else "libSDL3.so*"))
        if not sdl:
            raise RuntimeError("Build the GUI with its SDL shared runtime rule first")
        for library in sdl:
            if not library.is_file():
                raise RuntimeError("SDL shared runtime is missing or a broken symlink: " + str(library))
            if library.is_symlink():
                link = library.readlink()
                if link.is_absolute() or ".." in link.parts:
                    raise RuntimeError("SDL symlink must resolve within the package directory")
            shutil.copy2(library, staging / library.name, follow_symlinks=False)
        for filename in graphics_manifest["runtime_files"]:
            record = next(item for item in graphics_manifest["artifacts"] if item["file"] == filename)
            check_hash(graphics / filename, record["sha256"])
            shutil.copy2(graphics / filename, staging / Path(filename).name)
        shutil.copy2(graphics / "manifest.json", staging / "provenance/graphics-build.json")
        shutil.copy2(graphics / "args.gn", staging / "provenance/graphics-args.gn")
        shutil.copy2(REPO / "release/README-NATIVE.md", staging / "README.md")
        source_files = run("git", "-C", REPO, "ls-files", "--cached", "--others", "--exclude-standard", "-z", capture=True).split("\0")
        hashes = {name: sha256(REPO / name) for name in source_files if name and (REPO / name).is_file()}
        write_json(staging / "provenance/source-files.json", hashes)
        inventory(staging, "package-manifest.json", {"schema": 1, "product": "Astra", "version": version,
                  "platform": platform, "architecture": architecture, "configuration": "Release",
                  "sourceCommit": run("git", "-C", REPO, "rev-parse", "HEAD", capture=True).strip(),
                  "sourceDirty": bool(run("git", "-C", REPO, "status", "--porcelain", capture=True).strip()),
                  "inputs": inputs, "runtimeIdentity": runtime_manifest["identity"]})
        verify(staging, "package-manifest.json")
        require_payload(staging, platform, architecture)
        staging.rename(output)
        if not args.no_archive:
            archive_path = Path(str(output) + ".tar.gz")
            with archive_path.open("xb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", filename="", mtime=0) as zipped, \
                    tarfile.open(fileobj=zipped, mode="w", format=tarfile.PAX_FORMAT) as bundle:
                for path in [output, *sorted(output.rglob("*"))]:
                    relative = name if path == output else name + "/" + path.relative_to(output).as_posix()
                    entry = bundle.gettarinfo(str(path), arcname=relative)
                    entry.uid = entry.gid = entry.mtime = 0
                    entry.uname = entry.gname = ""
                    entry.pax_headers = {}
                    if entry.isfile():
                        with path.open("rb") as content:
                            bundle.addfile(entry, content)
                    else:
                        bundle.addfile(entry)
            Path(str(archive_path) + ".sha256").write_text(sha256(archive_path) + "  " + archive_path.name + "\n")
        print(f"Native package assembled: {output}")
    finally:
        if staging.exists():
            shutil.rmtree(child(REPO / "build", staging))


def require_payload(directory, platform, architecture):
    for name in ("astra", "astra-server", "tools/node/bin/node", "tools/node/LICENSE", "tools/clangd/bin/clangd",
                 "tools/clangd/LICENSE.TXT", "tools/git/bin/git", "data/defaults.js", "THIRD-PARTY.md",
                 "tools/node_modules/typescript/lib/tsserver.js", "tools/node_modules/typescript/LICENSE.txt",
                 "tools/node_modules/typescript/ThirdPartyNoticeText.txt", "tools/node_modules/typescript-language-server/lib/cli.mjs",
                 "tools/node_modules/typescript-language-server/LICENSE", "tools/node_modules/basedpyright/langserver.index.js",
                 "tools/node_modules/basedpyright/LICENSE.txt", "tools/node_modules/basedpyright/dist/typeshed-fallback/LICENSE"):
        if not (directory / name).is_file():
            raise RuntimeError(f"Missing package resource: {name}")
    for name in ("astra", "astra-server", "tools/node/bin/node", "tools/clangd/bin/clangd", "tools/git/bin/git"):
        if not (directory / name).stat().st_mode & 0o111:
            raise RuntimeError(f"Native executable has no execute permission: {name}")
        native_binary(directory / name, platform, architecture)
    libraries = list(directory.glob("libSDL3*.dylib" if platform == "macosx" else "libSDL3.so*"))
    if not libraries:
        raise RuntimeError("Missing SDL shared runtime")
    for library in libraries:
        if not library.is_file() or directory.resolve() not in library.resolve().parents:
            raise RuntimeError("SDL shared runtime is broken or escapes package: " + str(library))
        native_binary(library, platform, architecture, library=True)


if __name__ == "__main__":
    main()
