"""Repository-scoped primitives for native macOS/Linux release preparation.

This module has no import-time downloads, process launches, or file mutations.
Entry points must call native_host() before doing any work. Never cross-build.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

REPO = Path(__file__).resolve().parent.parent


def native_host() -> tuple[str, str]:
    if sys.platform not in ("linux", "darwin"):
        raise RuntimeError("Native runtime/package scripts must run on Linux or macOS, never Windows")
    if sys.version_info < (3, 12):
        raise RuntimeError("Python 3.12 or newer is required")
    architecture = {"x86_64": "x64", "amd64": "x64", "aarch64": "arm64", "arm64": "arm64"}.get(platform.machine().lower())
    if architecture is None:
        raise RuntimeError("Only native x64/arm64 are supported")
    return ("macosx" if sys.platform == "darwin" else "linux", architecture)


def child(root: Path, path: Path) -> Path:
    root = root.absolute()
    path = path.absolute()
    if path == root or root not in path.parents:
        raise RuntimeError(f"Path escapes {root}: {path}")
    for ancestor in (path, *path.parents):
        if ancestor == REPO.parent:
            break
        if ancestor.is_symlink():
            raise RuntimeError(f"Symlink ancestor refused: {ancestor}")
    if root.resolve() not in path.resolve().parents:
        raise RuntimeError(f"Resolved path escapes {root}: {path}")
    return path


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def check_hash(path: Path, expected: str) -> None:
    if len(expected) != 64 or sha256(path) != expected:
        raise RuntimeError(f"SHA-256 mismatch: {path}")


def native_binary(path: Path, target_platform: str, architecture: str, *, library=False) -> None:
    """Check native file identity without loading or executing any binary."""
    size = path.stat().st_size
    with path.open("rb") as stream:
        header = stream.read(64)
        if target_platform == "linux":
            # Only the supported 64-bit little-endian ELF CPUs, with System V
            # or Linux OSABI. PIE executables also use the ET_DYN file type.
            if len(header) < 64 or header[:7] != b"\x7fELF\x02\x01\x01" or header[7] not in (0, 3):
                raise RuntimeError(f"Not a supported Linux ELF binary: {path}")
            file_type, machine, version = struct.unpack_from("<HHI", header, 16)
            if machine != {"x64": 62, "arm64": 183}[architecture] or version != 1 or file_type not in ((3,) if library else (2, 3)):
                raise RuntimeError(f"ELF CPU or file type mismatch: {path}")
            return
        if target_platform != "macosx":
            raise RuntimeError("Unsupported native binary platform")
        cpu = {"x64": 0x01000007, "arm64": 0x0100000C}[architecture]

        def check_slice(offset, length):
            if length < 32 or offset < 0 or offset + length > size:
                raise RuntimeError(f"Invalid Mach-O slice bounds: {path}")
            stream.seek(offset)
            thin = stream.read(32)
            byte_order = {b"\xcf\xfa\xed\xfe": "<", b"\xfe\xed\xfa\xcf": ">"}.get(thin[:4])
            if byte_order is None:
                raise RuntimeError(f"Not a 64-bit Mach-O slice: {path}")
            actual_cpu, _, file_type = struct.unpack_from(byte_order + "III", thin, 4)
            if actual_cpu != cpu or file_type != (6 if library else 2):
                raise RuntimeError(f"Mach-O CPU or file type mismatch: {path}")

        # Fat headers are normally big endian; also recognize the swapped
        # encodings and fat_arch_64 offsets. Validate the selected slice itself.
        fat = {b"\xca\xfe\xba\xbe": (">", False), b"\xbe\xba\xfe\xca": ("<", False),
               b"\xca\xfe\xba\xbf": (">", True), b"\xbf\xba\xfe\xca": ("<", True)}.get(header[:4])
        if fat is None:
            check_slice(0, size)
            return
        if len(header) < 8:
            raise RuntimeError(f"Truncated Mach-O universal header: {path}")
        byte_order, wide = fat
        count = struct.unpack_from(byte_order + "I", header, 4)[0]
        record_size = 32 if wide else 20
        if not 1 <= count <= 64 or 8 + count * record_size > size:
            raise RuntimeError(f"Invalid Mach-O universal slice table: {path}")
        stream.seek(8)
        records = stream.read(count * record_size)
        for index in range(count):
            entry = struct.unpack_from(byte_order + ("IIQQII" if wide else "IIIII"), records, index * record_size)
            if entry[0] == cpu:
                check_slice(entry[2], entry[3])
                return
        raise RuntimeError(f"Mach-O universal binary has no {architecture} slice: {path}")


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write_json(path: Path, data) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def run(*arguments, cwd: Path | None = None, env=None, capture=False) -> str:
    argv = [str(argument) for argument in arguments]
    print("+", " ".join(argv), flush=True)
    return subprocess.run(argv, cwd=cwd or REPO, env=env, check=True, text=True,
                          stdout=subprocess.PIPE if capture else None).stdout or ""


def archive(asset: dict, offline: bool, candidates=()) -> Path:
    name = asset["archive"].split("/")[-1]
    destination = child(REPO, REPO / ".tools/release-downloads" / (asset["sha256"][:16] + "-" + name))
    for candidate in (*candidates, destination):
        candidate = Path(candidate)
        if candidate.is_file():
            child(REPO, candidate)
            check_hash(candidate, asset["sha256"])
            return candidate
    if offline:
        raise RuntimeError(f"Missing verified archive: {asset['archive']}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + ".download")
    with urllib.request.urlopen(asset["archive"], timeout=180) as source, temporary.open("wb") as output:
        shutil.copyfileobj(source, output)
    check_hash(temporary, asset["sha256"])
    temporary.replace(destination)
    return destination


def extraction_path(root: Path, name: str) -> Path:
    relative = PurePosixPath(name)
    if relative.is_absolute() or ".." in relative.parts or "\\" in name or ":" in name:
        raise RuntimeError(f"Unsafe archive path: {name}")
    return child(root, root.joinpath(*relative.parts))


def extract(source: Path, destination: Path, prefix="", only=None, source_links=False) -> None:
    child(REPO, destination)
    if destination.exists():
        raise RuntimeError(f"Extraction requires a new directory: {destination}")
    destination.mkdir(parents=True)
    wanted = set(only) if only is not None else None
    links = []

    def selected(name: str):
        if not name.startswith(prefix):
            return None
        relative = name[len(prefix):].rstrip("/")
        return relative if relative and (wanted is None or relative in wanted) else None

    if zipfile.is_zipfile(source):
        with zipfile.ZipFile(source) as bundle:
            for member in bundle.infolist():
                name = selected(member.filename)
                if name is None:
                    continue
                target = extraction_path(destination, name)
                mode = member.external_attr >> 16
                if stat.S_ISLNK(mode):
                    raise RuntimeError("ZIP links are not supported in binary runtime archives")
                if member.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                with bundle.open(member) as data, target.open("xb") as output:
                    shutil.copyfileobj(data, output)
                target.chmod(mode & 0o777 if mode & 0o777 else 0o644)
    else:
        with tarfile.open(source, "r:*") as bundle:
            for member in bundle:
                name = selected(member.name)
                if name is None:
                    continue
                target = extraction_path(destination, name)
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                elif member.isfile():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with bundle.extractfile(member) as data, target.open("xb") as output:
                        shutil.copyfileobj(data, output)
                    target.chmod(member.mode & 0o777)
                elif source_links and (member.issym() or member.islnk()):
                    links.append((target, member.linkname, member.islnk()))
                else:
                    raise RuntimeError(f"Archive special entry refused: {member.name}")
    for target, link, hard in links:
        if hard:
            if not link.startswith(prefix):
                raise RuntimeError("Hardlink escapes source archive")
            source_file = extraction_path(destination, link[len(prefix):])
        else:
            source_file = (target.parent / link).resolve()
            if Path(link).is_absolute() or destination.resolve() not in source_file.parents:
                raise RuntimeError("Symlink escapes source archive")
        target.parent.mkdir(parents=True, exist_ok=True)
        if hard:
            os.link(source_file, target)
        else:
            target.symlink_to(link)
    if wanted is not None:
        for name in wanted:
            if not extraction_path(destination, name).is_file():
                raise RuntimeError(f"Archive missing {name}")


def notices(directory: Path, asset: dict) -> None:
    for name, expected in asset["licenseFiles"].items():
        check_hash(extraction_path(directory, name), expected)


def inventory(directory: Path, filename: str, metadata: dict) -> None:
    entries = []
    for path in sorted(directory.rglob("*")):
        relative = path.relative_to(directory).as_posix()
        if path.is_symlink():
            target = path.resolve()
            if Path(os.readlink(path)).is_absolute() or directory.resolve() not in target.parents or not target.exists():
                raise RuntimeError(f"Distribution symlink is broken or escapes package: {relative}")
            entries.append({"path": relative, "link": os.readlink(path)})
        elif path.is_file():
            entries.append({"path": relative, "bytes": path.stat().st_size, "sha256": sha256(path),
                            "mode": stat.S_IMODE(path.stat().st_mode)})
    write_json(directory / filename, {**metadata, "files": entries})


def verify(directory: Path, filename: str) -> dict:
    child(REPO, directory)
    manifest = read_json(directory / filename)
    actual = {path.relative_to(directory).as_posix(): path for path in directory.rglob("*") if path.is_file() or path.is_symlink()}
    expected = {filename}
    for entry in manifest["files"]:
        name = entry["path"]
        if name in expected or name not in actual:
            raise RuntimeError(f"Duplicate or missing payload: {name}")
        expected.add(name)
        path = actual[name]
        if "link" in entry:
            if (not path.is_symlink() or os.readlink(path) != entry["link"] or Path(entry["link"]).is_absolute()
                    or directory.resolve() not in path.resolve().parents or not path.resolve().exists()):
                raise RuntimeError(f"Invalid distribution symlink: {name}")
        else:
            if path.is_symlink() or path.stat().st_size != entry["bytes"] or stat.S_IMODE(path.stat().st_mode) != entry["mode"]:
                raise RuntimeError(f"Changed file metadata: {name}")
            check_hash(path, entry["sha256"])
    if set(actual) != expected:
        raise RuntimeError("Unmanifested payload files")
    return manifest
