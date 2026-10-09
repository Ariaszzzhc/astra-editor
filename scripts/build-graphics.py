#!/usr/bin/env python3
"""Build the pinned graphics stack on a native macOS/Linux host (never cross-compile).

Windows uses build-graphics.ps1. This script has only been source-reviewed on Windows;
it must be accepted on each native host before that platform is called verified.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import platform
import runpy
import shlex
import shutil
import stat
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / ".tools" / "graphics"
SKIA = "525f94ac3418fd0b0dae42caa220422a94b18b49"
SKIA_TREE = "4924cfdc267cd1fcdd7ea63981a0fcb0e10df010"
DAWN = "ab3c12305e776fa707503f7ecfab07364e9c7697"
GN = "b2afae122eeb6ce09c52d63f67dc53fc517dbdc8"
# Official CMake SHA-256.txt and Ninja GitHub release asset digests; see GRAPHICS.md.
CMAKE = {
    ("linux", "x64"): ("linux-x86_64", "630615d8e98ac33eba7fbe472626dff5c899c85af3c024585ae109166a6909d0"),
    ("linux", "arm64"): ("linux-aarch64", "609735983e3bdf24b6ab379d918458d64196fe72b98226f62dd5e9fe7b2997cc"),
    ("macosx", "x64"): ("macos-universal", "d1449f969c54d5c00886d5b643340d493dfb3c81cb39ee29b35453395c11ebf7"),
    ("macosx", "arm64"): ("macos-universal", "d1449f969c54d5c00886d5b643340d493dfb3c81cb39ee29b35453395c11ebf7"),
}
NINJA = {
    ("linux", "x64"): ("ninja-linux.zip", "0830252db77884957a1a4b87b05a1e2d9b5f658b8367f82999a941884cbe0238"),
    ("linux", "arm64"): ("ninja-linux-aarch64.zip", "740f1b9f9d8ae68438240a6a2f3f7a27fc8b1946d2024a6a6b25857ee877987b"),
    ("macosx", "x64"): ("ninja-mac.zip", "da7797794153629aca5570ef7c813342d0be214ba84632af886856e8f0063dd9"),
    ("macosx", "arm64"): ("ninja-mac.zip", "da7797794153629aca5570ef7c813342d0be214ba84632af886856e8f0063dd9"),
}


def run(*arguments: str | Path, cwd: Path = ROOT, capture: bool = False) -> str:
    result = subprocess.run([str(arg) for arg in arguments], cwd=cwd, check=True,
                            text=True, stdout=subprocess.PIPE if capture else None)
    return result.stdout.strip() if capture else ""


def sha256(file: Path) -> str:
    with file.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def download(name: str, url: str, expected: str) -> Path:
    directory = ROOT / ".tools" / "downloads"
    directory.mkdir(parents=True, exist_ok=True)
    file = directory / name
    if not file.exists():
        partial = file.with_suffix(file.suffix + ".partial")
        print(f"Downloading {name}", flush=True)
        request = urllib.request.Request(url, headers={"User-Agent": "Astra-graphics-bootstrap"})
        with urllib.request.urlopen(request, timeout=120) as response, partial.open("wb") as stream:
            shutil.copyfileobj(response, stream)
        if sha256(partial) != expected:
            raise RuntimeError(f"SHA-256 mismatch: {partial}; retained for inspection")
        partial.replace(file)
    if sha256(file) != expected:
        raise RuntimeError(f"SHA-256 mismatch in cached archive: {file}")
    return file


def unpack(archive: Path, directory: Path, executable: str) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    # Extraction uses data_filter even for trusted hash-pinned tar archives.
    # Zip tools contain only ordinary files; reject links and traversal explicitly.
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as package:
            for item in package.infolist():
                destination = (directory / item.filename).resolve()
                if not destination.is_relative_to(directory.resolve()):
                    raise RuntimeError(f"Unsafe archive member: {item.filename}")
                if stat.S_ISLNK(item.external_attr >> 16):
                    raise RuntimeError(f"Unexpected archive symlink: {item.filename}")
            package.extractall(directory)
    else:
        with tarfile.open(archive) as package:
            package.extractall(directory, filter="data")
    result = directory / executable
    if not result.is_file():
        raise RuntimeError(f"Archive did not contain {executable}")
    result.chmod(result.stat().st_mode | stat.S_IXUSR)
    return result


def checkout(directory: Path, url: str, revision: str, tree: str | None = None) -> None:
    if not (directory / ".git").is_dir():
        if directory.exists():
            raise RuntimeError(f"Unmanaged source directory: {directory}")
        directory.mkdir(parents=True)
        run("git", "init", "--quiet", directory)
        run("git", "-C", directory, "remote", "add", "origin", url)
    if run("git", "-C", directory, "remote", "get-url", "origin", capture=True) != url:
        raise RuntimeError(f"Unexpected source origin: {directory}")
    head = subprocess.run(["git", "-C", str(directory), "rev-parse", "--verify", "HEAD"],
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    if head.returncode:
        run("git", "-C", directory, "fetch", "--depth=1", "origin", revision)
        run("git", "-C", directory, "checkout", "--quiet", "--detach", revision)
    if run("git", "-C", directory, "rev-parse", "HEAD", capture=True) != revision:
        raise RuntimeError(f"Checkout does not match pinned revision {revision}: {directory}")
    if tree and run("git", "-C", directory, "rev-parse", "HEAD^{tree}", capture=True) != tree:
        raise RuntimeError(f"Source tree checksum mismatch: {directory}")
    if run("git", "-C", directory, "status", "--porcelain", "--untracked-files=no", capture=True):
        raise RuntimeError(f"Tracked source was modified: {directory}")


def synchronize(source: Path, marker: Path, resync: bool) -> None:
    previous = Path.cwd()
    os.chdir(source)
    try:
        upstream = runpy.run_path("tools/git-sync-deps")
        if resync or not marker.exists():
            print("Synchronizing pinned DEPS (several GB on the first build)", flush=True)

            def bounded_sync(function, arguments):
                def attempt(args):
                    for retry in range(4):
                        try:
                            return function(*args)
                        except subprocess.CalledProcessError:
                            if retry == 3:
                                raise
                            time.sleep(3 * 2 ** retry)
                with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                    list(pool.map(attempt, arguments))

            sync = upstream["git_sync_deps"]
            sync.__globals__["multithread"] = bounded_sync
            sync(upstream["DEFAULT_DEPS_PATH"], [], True, True)
        locked = {}
        for relative, spec in upstream["parse_file_to_dict"]("DEPS")["deps"].items():
            if not isinstance(spec, str):
                continue
            url, expected = spec.rsplit("@", 1)
            directory = source / relative
            commit = run("git", "-C", directory, "rev-parse", expected + "^{commit}", capture=True)
            if run("git", "-C", directory, "rev-parse", "HEAD", capture=True) != commit:
                raise RuntimeError(f"Dependency is not at its pinned commit: {relative}")
            if run("git", "-C", directory, "status", "--porcelain", "--untracked-files=no", capture=True):
                raise RuntimeError(f"Tracked dependency source was modified: {relative}")
            locked[relative] = {"url": url, "revision": expected, "commit": commit}
        marker.write_text(json.dumps(locked, indent=2) + "\n", encoding="utf-8")
        print(f"Verified {len(locked)} pinned dependency checkouts", flush=True)
    finally:
        os.chdir(previous)


def gn_string(value: str | Path) -> str:
    value = str(value)
    if any(char in value for char in ('"', "$", "\n", "\\")):
        raise RuntimeError(f"Unsupported character in GN argument: {value!r}")
    return '"' + value + '"'


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--configuration", choices=("Debug", "Release"), default="Release")
    parser.add_argument("--stage", choices=("Bootstrap", "Configure", "Build"), default="Build")
    parser.add_argument("--architecture", choices=("x64", "x86_64", "arm64", "aarch64"))
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument("--resync", action="store_true")
    args = parser.parse_args()
    if sys.platform not in ("linux", "darwin"):
        parser.error("Only a native Linux/macOS host is supported; Windows uses build-graphics.ps1")
    if sys.version_info < (3, 12):
        parser.error("Python 3.12 or newer is required")
    native = "macosx" if sys.platform == "darwin" else "linux"
    aliases = {"x86_64": "x64", "amd64": "x64", "arm64": "arm64", "aarch64": "arm64", "x64": "x64"}
    arch = aliases.get(platform.machine().lower())
    if not arch or (args.architecture and aliases[args.architecture] != arch):
        parser.error("Cross-compilation is disabled; target architecture must match the native host")
    if not 1 <= args.jobs <= 128:
        parser.error("--jobs must be between 1 and 128")
    # Upstream GN bootstrap emits unquoted tool commands. Fail clearly before download.
    if any(char.isspace() or char in "\"'$`\\" for char in str(ROOT)):
        parser.error("The native GN bootstrap requires a checkout path without whitespace or shell metacharacters")

    host = TOOLS / "native" / f"{native}-{arch}"
    output = ROOT / "build" / "graphics" / native / arch / args.configuration.lower()
    install = TOOLS / "install" / native / arch / args.configuration.lower()
    for directory in (host, output, host / "tmp", host / "home", host / "bin"):
        directory.mkdir(parents=True, exist_ok=True)
    # build_dawn.py prints its environment: allow only toolchain settings, never
    # arbitrary ambient credentials. All generated tool caches stay in this checkout.
    allowed = ("PATH", "CC", "CXX", "SDKROOT", "DEVELOPER_DIR", "MACOSX_DEPLOYMENT_TARGET", "PKG_CONFIG_PATH")
    environment = {name: os.environ[name] for name in allowed if name in os.environ}
    environment.update(HOME=str(host / "home"), TMPDIR=str(host / "tmp"),
                       XDG_CACHE_HOME=str(host / "home" / ".cache"), LC_ALL="C",
                       PYTHONDONTWRITEBYTECODE="1", GIT_CONFIG_NOSYSTEM="1",
                       GIT_CONFIG_GLOBAL=os.devnull)
    os.environ.clear()
    os.environ.update(environment)
    for tool in ("git", "clang", "clang++", "ar"):
        if not shutil.which(tool):
            raise RuntimeError(f"Install the native toolchain prerequisite: {tool}")
    cc = shutil.which(os.environ.get("CC", "clang"))
    cxx = shutil.which(os.environ.get("CXX", "clang++"))
    if not cc or not cxx or "clang" not in run(cxx, "--version", capture=True).lower():
        raise RuntimeError("Use native Clang/Apple Clang for both CC and CXX")
    if any(char.isspace() or char in "\"'$`\\" for char in cc + cxx):
        raise RuntimeError("The native GN bootstrap requires compiler paths without shell metacharacters")
    macros = subprocess.run([cxx, "-std=c++23", "-dM", "-E", "-x", "c++", "-"],
                            input="#include <version>\n", text=True, check=True,
                            stdout=subprocess.PIPE).stdout
    library_macro = "_LIBCPP_VERSION" if native == "macosx" else "__GLIBCXX__"
    if library_macro not in macros:
        raise RuntimeError(f"The native C++ standard library must define {library_macro}")
    os.environ.update(CC=cc, CXX=cxx)
    if native == "linux":
        if not shutil.which("pkg-config"):
            raise RuntimeError("Install pkg-config and the Fontconfig/X11/Wayland development packages")
        run("pkg-config", "--exists", "fontconfig", "x11", "wayland-client")

    suffix, digest = CMAKE[native, arch]
    name = f"cmake-3.31.8-{suffix}.tar.gz"
    cmake_relative = f"cmake-3.31.8-{suffix}/" + ("CMake.app/Contents/bin/cmake" if native == "macosx" else "bin/cmake")
    cmake = unpack(download(name, f"https://github.com/Kitware/CMake/releases/download/v3.31.8/{name}", digest), host / "cmake", cmake_relative)
    name, digest = NINJA[native, arch]
    ninja = unpack(download(name, f"https://github.com/ninja-build/ninja/releases/download/v1.13.1/{name}", digest), host / "ninja", "ninja")
    # Dawn launches an inner ninja without -j; use a transparent, bounded wrapper.
    wrapper = host / "bin" / "ninja"
    wrapper.write_text(f"#!/bin/sh\nexec {shlex.quote(str(ninja))} -j {args.jobs} \"$@\"\n", encoding="utf-8")
    wrapper.chmod(0o755)
    python_alias = host / "bin" / "python3"
    if python_alias.is_symlink():
        python_alias.unlink()
    if python_alias.exists():
        raise RuntimeError(f"Unmanaged Python alias: {python_alias}")
    python_alias.symlink_to(Path(sys.executable).resolve())
    os.environ["PATH"] = os.pathsep.join((str(host / "bin"), str(cmake.parent), os.environ["PATH"]))

    source = TOOLS / "src" / "skia"
    checkout(source, "https://skia.googlesource.com/skia.git", SKIA, SKIA_TREE)
    if f"dawn.git@{DAWN}" not in (source / "DEPS").read_text(encoding="utf-8"):
        raise RuntimeError("Skia DEPS does not name the pinned Dawn revision")
    gn_source = host / "src" / "gn"
    checkout(gn_source, "https://gn.googlesource.com/gn", GN)
    if args.stage == "Bootstrap":
        print(f"Native tools and pinned Skia/GN source verified: {host}")
        return
    marker = host / "dependencies-verified.json"
    synchronize(source, marker, args.resync)
    gn_output = ROOT / "build" / "graphics" / native / arch / "gn"
    gn_output.mkdir(parents=True, exist_ok=True)
    # A shallow immutable checkout lacks GN's initial-commit tag. This position is
    # the official pinned binary's `gn --version`: 2175 (b2afae122eeb).
    (gn_output / "last_commit_position.h").write_text(
        '#pragma once\n#define LAST_COMMIT_POSITION_NUM 2175\n'
        '#define LAST_COMMIT_POSITION "2175 (b2afae122eeb)"\n', encoding="utf-8")
    run(sys.executable, gn_source / "build" / "gen.py", "--out-path", gn_output,
        "--no-last-commit-position", "--no-static-libstdc++", cwd=gn_source)
    # Upstream's regeneration command assumes its default repo/out directory.
    # Adjust the generated Ninja file only; keep the pinned source tree untouched.
    ninja_file = gn_output / "build.ninja"
    generated = ninja_file.read_text(encoding="utf-8")
    generated = generated.replace("../build/gen.py", str(gn_source / "build" / "gen.py"))
    ninja_file.write_text(generated, encoding="utf-8")
    run(wrapper, "-C", gn_output, "gn")
    gn = gn_output / "gn"
    run(gn, "--version")

    settings = {
        "target_os": "mac" if native == "macosx" else "linux", "target_cpu": arch,
        "cc": cc, "cxx": cxx, "is_debug": args.configuration == "Debug",
        "is_official_build": False, "is_component_build": False, "is_trivial_abi": False,
        "skia_enable_graphite": True, "skia_use_dawn": True, "skia_enable_ganesh": False,
        "dawn_enable_d3d12": False, "dawn_enable_d3d11": False, "dawn_enable_opengles": False,
        "dawn_enable_metal": native == "macosx", "dawn_enable_vulkan": native == "linux",
        "dawn_use_agility_sdk": False, "skia_enable_tools": False, "skia_enable_android_utils": False,
        "skia_use_gl": False, "skia_use_direct3d": False, "skia_enable_pdf": False,
        "skia_enable_skottie": False, "skia_enable_svg": False, "skia_use_icu": True,
        "skia_use_system_icu": False, "skia_use_harfbuzz": True, "skia_use_system_harfbuzz": False,
        "skia_use_system_libjpeg_turbo": False, "skia_use_system_libpng": False,
        "skia_use_system_libwebp": False, "skia_use_system_zlib": False, "skia_use_system_expat": False,
        "skia_use_partition_alloc": False, "skia_enable_vello_shaders": False,
        "skia_use_fontations": False, "skia_build_rust_targets": False,
    }
    if native == "linux":
        settings["skia_use_system_freetype2"] = False
    lines = [f"{key}={str(value).lower() if isinstance(value, bool) else gn_string(value)}"
             for key, value in settings.items()]
    lines.append('extra_cflags=["-fPIC"]')
    (output / "args.gn").write_text("\n".join(lines) + "\n", encoding="utf-8")
    run(gn, "gen", output, f"--script-executable={sys.executable}", "--fail-on-unused-args", cwd=source)
    if args.stage == "Configure":
        print(f"Native Graphite/Dawn build configured: {output}")
        return
    run(wrapper, "-C", output, "skia", "skparagraph", "skshaper", "skunicode_core", "skunicode_icu")

    include = install / "include" / "skia"
    shutil.copytree(source / "include", include / "include", dirs_exist_ok=True)
    for module in ("skparagraph", "skshaper", "skunicode"):
        shutil.copytree(source / "modules" / module / "include", include / "modules" / module / "include", dirs_exist_ok=True)
    (include / "modules" / "skcms" / "src").mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / "modules" / "skcms" / "skcms.h", include / "modules" / "skcms")
    for file in (source / "modules" / "skcms" / "src").glob("*.h"):
        shutil.copy2(file, include / "modules" / "skcms" / "src")
    (include / "src" / "core").mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / "src" / "core" / "SkUTF.h", include / "src" / "core")
    for directory in (source / "third_party" / "externals" / "dawn" / "include", output / "gen" / "third_party" / "dawn" / "include"):
        shutil.copytree(directory, install / "include" / "dawn", dirs_exist_ok=True)
    libraries = sorted(output.glob("lib*.a"))
    required = {"lib" + name + ".a" for name in ("skia", "dawn_combined", "skparagraph", "skshaper", "skunicode_core", "skunicode_icu")}
    if not required.issubset({file.name for file in libraries}):
        raise RuntimeError("Native graphics build did not produce all required static libraries")
    (install / "lib").mkdir(parents=True, exist_ok=True)
    artifacts = []
    for file in libraries:
        destination = install / "lib" / file.name
        shutil.copy2(file, destination)
        artifacts.append({"file": f"lib/{file.name}", "sha256": sha256(destination)})
    (install / "licenses").mkdir(exist_ok=True)
    shutil.copy2(source / "LICENSE", install / "licenses" / "Skia-LICENSE.txt")
    shutil.copy2(source / "third_party" / "externals" / "dawn" / "LICENSE", install / "licenses" / "Dawn-LICENSE.txt")
    manifest = {
        "schema": 1, "platform": native, "architecture": arch, "configuration": args.configuration,
        "skia": SKIA, "skia_tree": SKIA_TREE, "dawn": DAWN, "gn": GN, "trivial_abi": False,
        "runtime": "libc++" if native == "macosx" else "libstdc++",
        "compiler": run(cxx, "--version", capture=True),
        "libraries": [file.name[3:-2] for file in libraries],
        "syslinks": ["fontconfig", "dl", "pthread", "m"] if native == "linux" else ["objc"],
        "frameworks": ["AppKit", "ApplicationServices", "CoreFoundation", "CoreGraphics", "CoreText", "ImageIO",
                       "Foundation", "IOSurface", "IOKit", "Metal", "QuartzCore", "Cocoa"] if native == "macosx" else [],
        "includes": ["include/skia", "include/dawn"],
        "defines": ["SK_GRAPHITE", "SK_DAWN", "SK_DAWN_HAS_METAL" if native == "macosx" else "SK_DAWN_HAS_VULKAN",
                    "SK_DEBUG" if args.configuration == "Debug" else "SK_RELEASE"],
        # Unlike Windows, ICU embeds its data into the native archive.
        "runtime_files": [], "artifacts": artifacts,
    }
    shutil.copy2(output / "args.gn", install / "args.gn")
    shutil.copy2(marker, install / "dependencies.json")
    (install / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Native Graphite/Dawn package built and installed at {install}")


if __name__ == "__main__":
    main()
