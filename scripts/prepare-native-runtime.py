#!/usr/bin/env python3
"""Prepare a pinned runtime on its native Linux/macOS host (never cross-build).

Source-reviewed on Windows only. See docs/PACKAGING.md for the validation boundary.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import os
from pathlib import Path
import shutil
import uuid

from native_release import REPO, archive, check_hash, child, extract, inventory, native_binary, native_host, notices, read_json, run, sha256, verify, write_json


def source(asset, root, name, offline):
    bundle = archive(asset, offline, [REPO / ".tools/release-downloads" / asset["archive"].split("/")[-1]])
    directory = root / name
    extract(bundle, directory, asset["prefix"], source_links=True)
    return directory, bundle


def build_clangd(asset, build_root, destination, arch, cmake, ninja, jobs, offline):
    sources, bundle = source(asset, build_root, "llvm-source", offline)
    output = build_root / "llvm-build"
    run(cmake, "-S", sources / "llvm", "-B", output, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}",
        "-DCMAKE_BUILD_TYPE=Release", "-DLLVM_ENABLE_PROJECTS=clang;clang-tools-extra",
        "-DLLVM_TARGETS_TO_BUILD=" + ("X86" if arch == "x64" else "AArch64"),
        "-DLLVM_INCLUDE_TESTS=OFF", "-DLLVM_INCLUDE_BENCHMARKS=OFF", "-DCLANG_INCLUDE_TESTS=OFF",
        "-DLLVM_ENABLE_ZLIB=OFF", "-DLLVM_ENABLE_ZSTD=OFF", "-DLLVM_ENABLE_LIBXML2=OFF",
        "-DLLVM_ENABLE_TERMINFO=OFF", "-DLLVM_BUILD_LLVM_DYLIB=OFF", "-DLLVM_LINK_LLVM_DYLIB=OFF")
    run(cmake, "--build", output, "--parallel", jobs, "--target", "clangd", "clang-resource-headers")
    (destination / "bin").mkdir(parents=True)
    shutil.copy2(output / "bin/clangd", destination / "bin/clangd")
    shutil.copytree(output / "lib/clang", destination / "lib/clang")
    shutil.copy2(sources / "LICENSE.TXT", destination / "LICENSE.TXT")
    for component in ("llvm", "clang", "clang-tools-extra"):
        shutil.copy2(sources / component / "LICENSE.TXT", destination / (component + "-LICENSE.TXT"))
    return {"kind": "source", "archiveSha256": sha256(bundle)}


def build_git(manifest, build_root, runtime, host, dependency_prefix, cmake, ninja, jobs, offline):
    dependencies = build_root / "git-dependencies"
    (dependencies / "lib").mkdir(parents=True)
    (dependencies / "include").mkdir()
    # Use the configured official vcpkg libraries; do not export or rebuild
    # dependencies from an unrelated Skia checkout.
    for filename in ("libz.a", "libexpat.a"):
        shutil.copy2(dependency_prefix / "lib" / filename, dependencies / "lib" / filename)
    for filename in ("zlib.h", "zconf.h", "expat.h", "expat_external.h"):
        shutil.copy2(dependency_prefix / "include" / filename, dependencies / "include" / filename)
    source_archives = []
    openssl_source, openssl_archive = source(manifest["openssl"], build_root, "openssl-source", offline)
    openssl_target = {("linux", "x64"): "linux-x86_64", ("linux", "arm64"): "linux-aarch64",
                      ("macosx", "x64"): "darwin64-x86_64-cc", ("macosx", "arm64"): "darwin64-arm64-cc"}[host]
    run("perl", openssl_source / "Configure", openssl_target, "no-shared", "no-module", "no-tests",
        "--prefix=" + str(dependencies), "--libdir=lib", "--openssldir=/etc/ssl", cwd=openssl_source)
    run("make", "-j", jobs, cwd=openssl_source)
    run("make", "install_sw", cwd=openssl_source)
    curl_source, curl_archive = source(manifest["curl"], build_root, "curl-source", offline)
    curl_build = build_root / "curl-build"
    run(cmake, "-S", curl_source, "-B", curl_build, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_INSTALL_PREFIX=" + str(dependencies), "-DCMAKE_INSTALL_LIBDIR=lib",
        "-DBUILD_SHARED_LIBS=OFF", "-DBUILD_STATIC_LIBS=ON", "-DBUILD_CURL_EXE=OFF", "-DBUILD_TESTING=OFF",
        "-DOPENSSL_ROOT_DIR=" + str(dependencies), "-DOPENSSL_USE_STATIC_LIBS=ON", "-DCURL_USE_OPENSSL=ON",
        "-DZLIB_INCLUDE_DIR=" + str(dependencies / "include"), "-DZLIB_LIBRARY=" + str(dependencies / "lib/libz.a"),
        "-DCURL_ZLIB=ON", "-DCURL_BROTLI=OFF", "-DCURL_ZSTD=OFF", "-DCURL_USE_LIBPSL=OFF",
        "-DCURL_USE_LIBSSH2=OFF", "-DCURL_USE_LIBSSH=OFF", "-DCURL_USE_GSASL=OFF", "-DUSE_LIBIDN2=OFF",
        "-DUSE_NGHTTP2=OFF", "-DCURL_DISABLE_LDAP=ON", "-DCURL_DISABLE_LDAPS=ON",
        "-DUSE_APPLE_SECTRUST=" + ("ON" if host[0] == "macosx" else "OFF"))
    run(cmake, "--build", curl_build, "--parallel", jobs)
    run(cmake, "--install", curl_build)
    git_source, git_archive = source(manifest["git"], build_root, "git-source", offline)
    destination = runtime / "tools/git"
    # These are Git's documented build variables. Local and HTTP(S) Git remain
    # available; separate Tcl/Tk GUIs, translated output and Perl-only commands
    # (e.g. send-email) are not part of Astra's Git interface.
    curl_flags = run(dependencies / "bin/curl-config", "--static-libs", capture=True).strip()
    arguments = ["prefix=" + str(destination), "RUNTIME_PREFIX=YesPlease", "NO_GETTEXT=YesPlease",
                 "NO_TCLTK=YesPlease", "NO_PERL=YesPlease", "NO_OPENSSL=YesPlease",
                 "ZLIB_PATH=" + str(dependencies), "EXPATDIR=" + str(dependencies),
                 "CURLDIR=" + str(dependencies), "CURL_CONFIG=" + str(dependencies / "bin/curl-config"),
                 "CURL_LDFLAGS=" + curl_flags]
    run("make", "-j", jobs, *arguments, "all", cwd=git_source)
    run("make", *arguments, "install", cwd=git_source)
    for name, directory, bundle in [*source_archives, ("git", git_source, git_archive), ("openssl", openssl_source, openssl_archive), ("curl", curl_source, curl_archive)]:
        target = runtime / "licenses" / name
        target.mkdir(parents=True)
        for path in directory.rglob("*"):
            if path.is_file() and path.name.upper().startswith(("LICENSE", "COPYING", "NOTICE")):
                relative = path.relative_to(directory)
                (target / relative).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target / relative)
        # Keep corresponding fixed source archives and this build script with
        # the distribution, rather than relying exclusively on upstream links.
        destination = runtime / "provenance/sources" / bundle.name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(bundle, destination)


def main():
    host = native_host()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--clangd-source", action="store_true", help="Build fixed LLVM sources for the native CPU/ABI")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument("--cmake", type=Path, required=True)
    parser.add_argument("--ninja", type=Path, required=True)
    parser.add_argument("--dependency-prefix", type=Path, required=True)
    args = parser.parse_args()
    if args.jobs < 1 or args.jobs > 128:
        raise RuntimeError("jobs must be between 1 and 128")
    key = "-".join(host)
    manifest = read_json(REPO / "release/native-tools.json")
    languages = read_json(REPO / "languages/manifest.json")
    cmake, ninja = args.cmake, args.ninja
    if not cmake.is_file() or not ninja.is_file():
        raise RuntimeError("Use the configured CMake/Ninja toolchain")
    dependency_prefix = child(REPO / ".tools", args.dependency_prefix)
    locks = ["release/native-tools.json", "languages/manifest.json", "grammars/manifest.json",
             "dependencies.lock.json", "release/THIRD-PARTY.md", "vcpkg.json", "vcpkg-configuration.json",
             "dependencies/tools.json", ".gitmodules", "scripts/native_release.py", "scripts/prepare-native-runtime.py"]
    locks += [str(path.relative_to(REPO)).replace("\\", "/") for path in
              sorted((REPO / "dependencies/ports").rglob("*")) if path.is_file()]
    for name, patch in read_json(REPO / "dependencies.lock.json").get("patches", {}).items():
        if sha256(child(REPO, REPO / patch["path"])) != patch["sha256"]:
            raise RuntimeError("Dependency patch hash mismatch: " + name)
    hashes = {name: sha256(REPO / name) for name in locks}
    source_clangd = args.clangd_source or key not in manifest["clangd"]["assets"]
    identity = hashlib.sha256((key + json_text(hashes) + str(source_clangd)
                               + sha256(dependency_prefix / "lib/libz.a")
                               + sha256(dependency_prefix / "lib/libexpat.a")).encode()).hexdigest()
    runtime_root = child(REPO, REPO / ".tools/runtime")
    destination = runtime_root / (key + "-" + identity[:16])
    if destination.exists():
        verify(destination, "runtime-manifest.json")
    else:
        staging = child(runtime_root, runtime_root / ("staging-" + uuid.uuid4().hex))
        build_root = child(REPO, REPO / ".tools/native-runtime-build" / (key + "-" + uuid.uuid4().hex))
        staging.mkdir(parents=True)
        build_root.mkdir(parents=True)
        allowed = ("PATH", "CC", "CXX", "SDKROOT", "DEVELOPER_DIR", "MACOSX_DEPLOYMENT_TARGET")
        environment = {name: os.environ[name] for name in allowed if name in os.environ}
        for directory in (build_root / "home", build_root / "tmp"):
            directory.mkdir()
        environment.update(HOME=str(build_root / "home"), TMPDIR=str(build_root / "tmp"),
                           XDG_CACHE_HOME=str(build_root / "home/.cache"), LC_ALL="C", PYTHONDONTWRITEBYTECODE="1",
                           GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
        os.environ.clear()
        os.environ.update(environment)
        try:
            node = manifest["node"]["assets"][key]
            extract(archive(node, args.offline), staging / "tools/node", node["prefix"], ("bin/node", "LICENSE", "README.md"))
            notices(staging / "tools/node", languages["node"])
            for package in languages["packages"]:
                bundle = archive(package, args.offline)
                with bundle.open("rb") as stream:
                    integrity = "sha512-" + base64.b64encode(hashlib.file_digest(stream, "sha512").digest()).decode()
                if integrity != package["integrity"]:
                    raise RuntimeError("npm SHA-512 mismatch")
                target = staging / "tools/node_modules" / package["name"]
                extract(bundle, target, "package/")
                metadata = read_json(target / "package.json")
                if metadata["name"] != package["name"] or metadata["version"] != package["version"] or metadata.get("dependencies"):
                    raise RuntimeError("npm identity or transitive dependency mismatch")
                notices(target, package)
            if source_clangd:
                clangd_origin = build_clangd(manifest["clangd"]["source"], build_root, staging / "tools/clangd", host[1], cmake, ninja, args.jobs, args.offline)
            else:
                clangd = manifest["clangd"]["assets"][key]
                extract(archive(clangd, args.offline), staging / "tools/clangd", clangd["prefix"])
                notices(staging / "tools/clangd", languages["clangd"])
                clangd_origin = {"kind": "official-binary", "archiveSha256": clangd["sha256"]}
            # Execute only on the native host. An incompatible official binary
            # fails explicitly; rerun with --clangd-source for a native ABI build.
            for executable in ("tools/node/bin/node", "tools/clangd/bin/clangd"):
                native_binary(staging / executable, *host)
                run(staging / executable, "--version")
            build_git(manifest, build_root, staging, host, dependency_prefix, cmake, ninja, args.jobs, args.offline)
            native_binary(staging / "tools/git/bin/git", *host)
            run(staging / "tools/git/bin/git", "--version")
            for grammar in read_json(REPO / "grammars/manifest.json")["packages"]:
                target = staging / "licenses/grammars" / grammar["name"]
                extract(archive(grammar, args.offline), target, grammar["directory"] + "/", ("LICENSE",))
                check_hash(target / "LICENSE", grammar["files"]["LICENSE"])
            for name, digest in hashes.items():
                check_hash(REPO / name, digest)
                target = staging / "provenance" / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(REPO / name, target)
            shutil.copy2(REPO / "release/THIRD-PARTY.md", staging / "THIRD-PARTY.md")
            shutil.copy2(REPO / "languages/manifest.json", staging / "tools/language-manifest.json")
            write_json(staging / "provenance/native-build.json", {"clangd": clangd_origin, "compiler": run("c++", "--version", capture=True), "host": key})
            inventory(staging, "runtime-manifest.json", {"schema": 1, "platform": host[0], "architecture": host[1], "identity": identity, "locks": hashes})
            verify(staging, "runtime-manifest.json")
            staging.rename(destination)
        finally:
            if staging.exists():
                shutil.rmtree(child(runtime_root, staging))
    write_json(runtime_root / (key + ".json"), {"schema": 1, "directory": destination.relative_to(REPO).as_posix(), "manifestSha256": sha256(destination / "runtime-manifest.json")})
    print(f"Prepared native runtime: {destination}")


def json_text(data):
    import json
    return json.dumps(data, sort_keys=True)


if __name__ == "__main__":
    main()
