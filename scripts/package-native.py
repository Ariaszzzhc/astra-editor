#!/usr/bin/env python3
"""Install and package Astra on its native Linux/macOS host. Unverified on Windows."""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import uuid

from native_release import REPO, child, inventory, native_binary, native_host, run, sha256, verify, write_json


def require_payload(directory, platform, architecture):
    for name in ("astra", "astra-server", "astra-clipboard-host", "tools/node/bin/node",
                 "tools/clangd/bin/clangd", "tools/git/bin/git"):
        native_binary(directory / name, platform, architecture)
    for name in ("resources/defaults.js", "README.md", "THIRD-PARTY.md",
                 "licenses/vcpkg/skia/copyright", "licenses/vcpkg/dawn/copyright",
                 "provenance/vcpkg-status.txt", "provenance/build-provenance.json"):
        if not (directory / name).is_file():
            raise RuntimeError("Missing installed payload: " + name)


def main():
    platform, architecture = native_host()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-directory", type=Path, required=True)
    parser.add_argument("--cmake", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--verify", type=Path)
    parser.add_argument("--no-archive", action="store_true")
    args = parser.parse_args()
    if args.verify:
        directory = child(REPO / "build", REPO / args.verify)
        manifest = verify(directory, "package-manifest.json")
        if (manifest["platform"], manifest["architecture"]) != (platform, architecture):
            raise RuntimeError("Cross-platform package verification is forbidden")
        require_payload(directory, platform, architecture)
        return
    build = child(REPO / "build", REPO / args.build_directory)
    cache = (build / "CMakeCache.txt").read_text()
    if "CMAKE_BUILD_TYPE:STRING=Release\n" not in cache:
        raise RuntimeError("A configured native Release build is required")
    if not args.cmake.is_file():
        raise RuntimeError("Pass the configured CMake executable")
    run(args.cmake, "--build", build, "--target", "astra-production")
    version = (build / "astra-version.txt").read_text().strip()
    name = f"astra-{version}-{platform}-{architecture}"
    output = child(REPO / "build", REPO / (args.output or Path("build/packages") / name))
    if output.exists() or Path(str(output) + ".tar.gz").exists():
        raise RuntimeError("Output already exists; choose another --output")
    staging = child(REPO / "build", output.with_name(output.name + ".staging-" + uuid.uuid4().hex))
    try:
        run(args.cmake, "--install", build, "--prefix", staging, "--component", "Runtime")
        names = run("git", "-C", REPO, "ls-files", "--cached", "--others", "--exclude-standard", "-z", capture=True).split("\0")
        write_json(staging / "provenance/source-files.json",
                   {name: sha256(REPO / name) for name in names if name and (REPO / name).is_file()})
        inventory(staging, "package-manifest.json", {"schema": 1, "product": "Astra", "version": version,
                  "platform": platform, "architecture": architecture, "configuration": "Release",
                  "sourceCommit": run("git", "-C", REPO, "rev-parse", "HEAD", capture=True).strip(),
                  "sourceDirty": bool(run("git", "-C", REPO, "status", "--porcelain", capture=True).strip()),
                  "vcpkgCommit": run("git", "-C", REPO / "third_party/vcpkg", "rev-parse", "HEAD", capture=True).strip()})
        verify(staging, "package-manifest.json")
        require_payload(staging, platform, architecture)
        staging.rename(output)
        if not args.no_archive:
            cpack = args.cmake.with_name("cpack")
            run(cpack, "--config", build / "CPackConfig.cmake", "-G", "TGZ",
                "-D", "CPACK_INSTALL_CMAKE_PROJECTS=", "-D", f"CPACK_INSTALLED_DIRECTORIES={output};/",
                "-D", f"CPACK_PACKAGE_DIRECTORY={output.parent}", "-D", f"CPACK_PACKAGE_FILE_NAME={output.name}")
            bundle = Path(str(output) + ".tar.gz")
            Path(str(bundle) + ".sha256").write_text(sha256(bundle) + "  " + bundle.name + "\n")
        print(f"Native package assembled: {output}")
    finally:
        if staging.exists():
            shutil.rmtree(child(REPO / "build", staging))


if __name__ == "__main__":
    main()
