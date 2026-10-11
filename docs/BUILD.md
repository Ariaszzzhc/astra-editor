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

Run all commands from this repository. This document gives reproduction
instructions; actual passed and failed checks are recorded in [STATUS.md](STATUS.md).
Do not run Linux/macOS builds or tests from this Windows environment.

## Prepare dependencies

```powershell
./scripts/prepare-grammars.ps1
./scripts/build-graphics.ps1 -Stage Build -Configuration Release -Jobs 8
./scripts/build-graphics.ps1 -Stage Build -Configuration Debug -Jobs 8
./scripts/prepare-language-tools.ps1 -IncludeClangd
./scripts/prepare-windows-runtime.ps1
```

`prepare-grammars.ps1` verifies fixed upstream Tree-sitter archives and generated
parser sources from `grammars/manifest.json`. The graphics script bootstraps its
fixed tools, configures and compiles Skia/Dawn, and writes a verified local
package for each configuration. Source, downloads and installed libraries require
several GB; see [GRAPHICS.md](GRAPHICS.md) for the exact pins, ABI and runtime data.
There is no fallback graphics backend.

The graphics source uses the reviewed, hash-verified shaping adapter under
`xmake/packages/skia-graphite/`. Build preparation verifies upstream/output blobs,
rejects unrelated/partial edits, and records the patch digest. xmake rejects
graphics packages without that exact adapter. Original upstream Git objects
remain pinned; formatter tools do not touch the downloaded checkout. The
directory's README records this integration boundary and reproduction commands.

`prepare-language-tools.ps1 -IncludeClangd` prepares the repository language-tool
cache used by real-server tests. `prepare-windows-runtime.ps1` prepares a separate
verified runtime inventory used by application build/install and packaging. It
includes clangd, Node, TypeScript Language Server, basedpyright and Git, plus
licenses. Its `-Offline` option requires every needed archive to be cached.
The two preparation paths have different consumers; neither installs tools globally.

The xmake wrapper bootstraps **xmake 3.1.1** from a hash-verified official archive
under `.tools/xmake/`. `dependencies.lock.json` also pins the official package
recipe archive and its source hashes. The wrapper registers that verified local
recipe tree and keeps dependency caches in this checkout. First-time preparation
and configuration need network access. Update reviewed manifests when upgrading;
do not edit downloaded recipes or select moving branches.

The script target uses `astra-quickjs` (aliased to `quickjs-ng`), based on upstream
v0.15.1 with the hash-verified host async context patch under
`xmake/packages/astra-quickjs/`. The patch digest participates in its cache
identity. Configuration and package tests reject a missing extension rather than
using an unmodified upstream binary. Its rationale and reproduction boundary are
in that directory's README. Runtime preparation verifies the local patch and
includes it, the recipe and README in distribution provenance.

Windows libraries share `/MT` and `_ITERATOR_DEBUG_LEVEL=0` in both modes to match
Skia/Dawn. Debug retains symbols and unoptimized application code. Keep the same
CRT, STL ABI and graphics configuration across all linked targets.

## Build and run

Format repository-owned C++ and JavaScript before committing:

```powershell
./scripts/format.ps1
./scripts/format.ps1 -Check -Offline
```

The formatter uses `.clang-format` and `.prettierrc.json`. Its fixed versions and
upstream artifact hashes are in `tools/formatters.json`; tools stay in `.tools/format/`.
The first run downloads verified archives and requires the pinned Node runtime
prepared above. `-Offline` only uses cached archives. `scripts/test.ps1` checks
formatting before building. Downloaded dependencies and build outputs are excluded.

```powershell
./scripts/xmake.ps1 f -m debug -y
./scripts/xmake.ps1 build -y astra astra-server astra-tests
./scripts/xmake.ps1 run astra --workspace . --open README.md
```

Executables are under `build/windows/x64/debug/` (or `release/`). The resource
rules stage `data/`, `tools/`, notices, `astra-process-host.exe`, SDL3, ICU data
and DXC beside the appropriate executables. Building the GUI executable alone
without these resources does not create a runnable distribution.

Real installed-IME acceptance additionally requires an interactive Windows
desktop and an already-enabled Chinese keyboard text service:

```powershell
./scripts/test.ps1 -Mode all -Gui -InstalledIme
# Or build/run the installed-input fixture independently:
./scripts/xmake.ps1 build -y gui-ime-smoke
./build/windows/x64/debug/gui-ime-smoke.exe --probe
./build/windows/x64/debug/gui-ime-smoke.exe
```

`--probe` establishes only the enabled-profile prerequisite. The actual fixture
activates that profile for its own process, sends real OS scan codes only to its
owned foreground window, and restores the profile and conversion/open state.
It rejects held modifiers/test keys. It verifies app preedit when offered, or
an actual owned native composition popup near the caret for native-only IMEs,
then verifies native candidate metadata and compares the exact native IME result
string with the SDL commit and shared document. One transaction, selection-history
undo/redo and durable save are checked. Native composition-window PNGs remain
under `build/graphics/` after successful fixture cleanup. The fixture also uses
the production JS API to open editor/sidebar forms and modal input, verifies
native caret placement inside each actual field, Chinese commit values and
Escape cancellation, and rejects mutations of the shared document. Native
Unicode `WM_CHAR` is observed independently when no IMM result packet is offered.
Every editable surface obtains owned native focus; input guards stop on focus
loss. Caption fallback uses actual window DPI and native hit testing. The entire
editing session uses one process-local profile activation and verifies original
profile restoration on exit. It never injects SDL text packets.

Use `astra --server --workspace DIR --port 7777` or the headless `astra-server`
for a loopback service, and `astra --connect 127.0.0.1:7777` for a GUI client.
`--config` loads a workspace-relative user JS file after defaults.
`--state-directory` explicitly chooses recovery storage. [SERVICE.md](SERVICE.md)
documents CLI requests and the shared protocol; [RECOVERY.md](RECOVERY.md)
documents restoration and save-conflict behavior.

## Acceptance targets

```powershell
./scripts/test.ps1 -Mode debug -Gui
./scripts/test.ps1 -Mode release -Gui
# Or run both modes, optionally including the Release core benchmark:
./scripts/test.ps1 -Mode all -Gui -Benchmarks
```

The test entry point builds the selected targets, runs the doctest suite and CLI
smoke, and checks packaging helpers. `-Gui` additionally runs GPU readback,
renderer/text-layout and all editor GUI smoke targets, including the TCP variant
of `gui-smoke`. These are separate acceptance executables, not doctest cases.
`-Benchmarks` runs the 1, 8 and 64 MiB core benchmark and the 1,000 / 10,000 /
100,000-row view lifetime benchmark only in Release. The latter compares actual
source destruction with owner-thread cancellation/publication handoff; it also
checks complete retirement after shutdown. Its latency numbers do not measure
GPU frames, service search completion, parsing or external snapshot holders.
Do not infer results for a new source revision from an older target run.

For a focused check after configuring the desired mode:

```powershell
./scripts/xmake.ps1 build -y astra-tests gui-workspace-smoke
./scripts/xmake.ps1 run astra-tests --no-version --duration=true
./scripts/xmake.ps1 run gui-workspace-smoke
```

| Target | Observable behavior |
| --- | --- |
| `astra-tests` | Integrated core, service, workspace, script, language, process, Git, recovery and watcher cases |
| `astra-core-tests`, `astra-syntax-tests` | Persistent data structures/document history and genuine grammar parsing |
| `astra-platform-tests` | Real child processes, Git repositories and language-server transport |
| `astra-recovery-tests`, `astra-watch-tests` | Durable checkpoints/crash recovery and real filesystem events |
| `graphics-smoke`, `renderer-smoke`, `text-layout-tests` | GPU pixels, native window lifecycle and Unicode shaping/hit testing |
| `viewport-layout-tests` | Native-font paragraph pixels/geometry against fresh shaping, every style option, LRU entry/byte admission, failures and retained layouts; `--dialogs` isolates modal checks; `--forms` isolates nonmodal font/DPI glyph, caret, pointer and clipping checks |
| `gui-smoke` | SDL editing, multicursor undo, clipboard, syntax, save/close, viewport scroll/edit layout reuse and long-line paging; `--tcp` uses the remote channel |
| `gui-services-smoke` | Actual clangd and Git through production GUI commands |
| `gui-recovery-smoke` | Restore review, dirty preservation, durable exit and checkpoint-error handling |
| `gui-workspace-smoke` | Dirty/untitled search navigation, search options, confirmed replacement, external conflict and guarded reload |
| `gui-presentation-smoke` | Indexed file-tree collapse, resizable sidebar, Outline, adaptive tabs/reorder/overflow, logical resize and same-batch prompt focus |
| `gui-body-smoke` | JS welcome actions, notice dismissal/replacement, original Unicode at large fonts/narrow resize, provider-failure diagnostics and restoration |

Screenshots and acceptance fixtures stay under `build/`; GUI failures preserve
their fixture for diagnosis. Timers and bounded polling wait for observable
completion. Git fixture cleanup permits only bounded sharing/lock-violation
retries; the process lifecycle tests still require immediate handle release.

## Package preparation

After successful Release acceptance:

```powershell
./scripts/xmake.ps1 f -m release -y
./scripts/xmake.ps1 build -y astra astra-server
./scripts/package-windows.ps1 -Offline
./scripts/verify-windows-package.ps1 -PackageDirectory build/packages/astra-0.1.0-windows-x64 -RunSmoke
```

Packaging refuses to replace an existing output directory. Choose a fresh
`-OutputDirectory` when producing another package. Inventory verification and
independent-directory smoke are distinct checks; [PACKAGING.md](PACKAGING.md)
describes their scope and provenance. The final 6,527-file ZIP from clean source
commit `afcfbac` passed SHA-256 verification and isolated runtime smoke after
extraction into a new directory. Consult [STATUS.md](STATUS.md) for the exact
tested snapshot, archive identity and commands.

Native macOS/Linux graphics, runtime and package scripts exist as source only.
They have not been run, compiled or validated here. Follow their native-host
instructions in [GRAPHICS.md](GRAPHICS.md) and [PACKAGING.md](PACKAGING.md) only
on the corresponding platform; Windows success does not establish native support.
