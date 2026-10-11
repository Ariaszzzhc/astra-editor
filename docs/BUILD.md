# Windows development

Use native Windows x64, PowerShell 7, Git, and a C++23-capable Visual Studio
toolchain with the Windows SDK. The recorded checks use Visual Studio 2026.
The GUI needs a functioning D3D12 GPU/driver and a desktop session; its smoke
tests open native windows and use the real SDL3/Skia Graphite/Dawn renderer.
`scripts/test.ps1 -Gui` also runs `gui-clipboard-smoke`: its owned hidden window
briefly leases the real Windows clipboard to verify read-failure preservation.
It does not change existing clipboard contents or formats. A lease deadline
releases contention if acceptance stops pumping; acquisition/cleanup failures
fail the fixture rather than skipping it.

`gui-clipboard-async-smoke` creates its own real Windows station, desktop and
clipboard. Explicit station names require an administrator token. Run this
dedicated binary elevated from the repository root; `--log` writes a unique log
beside its executable under `build/`. It never reuses an existing station or
changes the interactive clipboard. It exercises an actual delayed
`WM_RENDERFORMAT` provider, Unicode round trips, cancellation/deadlines, field
input order, responsive production rendering/input, and bounded shutdown.
`scripts/test.ps1 -Gui -IsolatedClipboard` includes it in an elevated acceptance
session; ordinary `-Gui` runs do not establish delayed-provider acceptance.
The entry point checks that prerequisite before starting any tests and never
silently elevates the entire development workflow.
The fixture takes only `--log`, with no following pathname. For a dedicated
Debug run without elevating the whole workflow:

```powershell
Start-Process -FilePath ./build/cmake/windows-debug/bin/gui-clipboard-async-smoke.exe -ArgumentList '--log' -WorkingDirectory (Get-Location).Path -Verb RunAs -WindowStyle Hidden -PassThru
```

Wait for that owned process, check its actual exit code and require a newly
generated `clipboard-isolated-*.log` in the binary directory. A failed/cancelled
launch or absent new log is not acceptance. Replace `debug` with `release` for
the other configuration. Request OS elevation only for the dedicated fixture.

For a current Windows release directory or freshly extracted package, run
`./scripts/verify-windows-package.ps1 -PackageDirectory <directory> -RunSmoke -RunGuiSmoke`.
The opt-in GUI check uses the extracted production executable, shipped JS,
isolated profile and a minimal Windows-only PATH; it verifies Unicode editing,
atomic undo/redo, durable save and guarded close. It requires this native desktop
and GPU, and preserves its owned fixture/stdout/stderr under `build/`.

Run all commands from this repository. This document gives reproduction
instructions; actual passed and failed checks are recorded in [STATUS.md](STATUS.md).
Do not run Linux/macOS builds or tests from this Windows environment.

## Configure and build

```powershell
git submodule update --init --recursive
./scripts/cmake.ps1 --preset windows-debug
./scripts/cmake.ps1 --build --preset windows-debug
./build/cmake/windows-debug/bin/astra.exe --workspace . --open README.md
```

The wrapper pins CMake/Ninja and initializes the Visual Studio environment.
vcpkg is a fixed source submodule; downloads, packages, build trees and binary
caches are under .tools/vcpkg. The bootstrap executable is generated inside the
upstream submodule as required by vcpkg's toolchain and remains ignored.
Use windows-release for Release. Each configuration has its own build tree/bin.

The official ports own dependency builds, including Skia's internal GN and
Dawn's CMake build. Only Skia's glyph-cache interface and QuickJS host async
context require project overlays. Source artifacts are hash-verified. Static
C++ libraries use /MD or /MDd; SDL3 stays shared. CMake install owns distribution
membership, and the developer runtime is staged once in each bin directory.
There is no global Node/Git/language-server prerequisite. No graphics fallback
is substituted when preparation fails. See GRAPHICS.md and dependencies/README.md.

Grammar sources are fixed by commit/hash, downloaded automatically during
configuration and compiled from their generated parsers. They are not downloaded
at editor runtime. ASTRA_OFFLINE requires cached grammar/runtime archives;
vcpkg still uses its own cache/download policy. First-time setup needs network.

## Acceptance

```powershell
./scripts/format.ps1 -Offline
./scripts/format.ps1 -Check -Offline
./scripts/test.ps1 -Mode all -Gui
./scripts/test.ps1 -Mode release -Benchmarks
```

Behavior and CLI tests run by default. Native GUI tests require explicit -Gui,
which checks GPU readback, text layout, actual editing, services, recovery,
presentation, clipboard failure preservation and the TCP GUI variant. CTest
labels distinguish behavior, integration, gui and environment acceptance.
Benchmarks are separate executables and are never installed.

Installed-IME acceptance additionally needs an enabled Chinese text service and
an interactive desktop: scripts/test.ps1 -Mode all -Gui -InstalledIme. Its
--probe checks only prerequisites; actual acceptance sends OS scan codes to an
owned window, verifies preedit/candidate metadata, exact committed text,
transaction history and save, then restores the process-local IME state. Native
captures remain under build/graphics. It does not synthesize SDL text packets.

The isolated delayed-clipboard fixture needs an administrator token. Ordinary
-Gui does not establish its acceptance. Use -Gui -IsolatedClipboard from an
already elevated acceptance session, or launch only its dedicated executable
with OS elevation as described above. Neither option silently elevates builds.
Physical DPI and adapter-removal behavior still require separate evidence.

## Install and package

```powershell
./scripts/package-windows.ps1 -OutputDirectory build/packages/astra-review
./scripts/verify-windows-package.ps1 -PackageDirectory build/packages/astra-review -RunSmoke -RunGuiSmoke
```

The package entry point configures/builds a Release production preset, installs
only the Runtime component into fresh staging, records source/dependency
provenance and a complete SHA-256 inventory, verifies it, then uses CPack to
archive that verified installation. Tests/fixtures/benchmarks and Debug CRTs
are rejected. Outputs are never overwritten. See PACKAGING.md.

Linux/macOS sources are maintained for native hosts, but no non-Windows build,
test or packaging command is run or considered verified in this environment.
