# Windows release packaging

Windows x64 runtime preparation, package helper checks and isolated-directory
package acceptance have passed for the earlier snapshot. The 6,527-file ZIP from clean source commit
`afcfbac` passed archive hash verification and a fresh extraction's isolated smoke:
packaged Git, clangd, TypeScript, Python and recovery through the real service.
That archive predates the current workbench and clipboard changes; it is not
acceptance of the current source. See [STATUS.md](STATUS.md) for archive identities
and the current acceptance commands.
The current 6,549-file package from clean source `bfd122e` includes the concurrent
GPU-loss classification correction. It passed inventory, ZIP hash, fresh
extraction, isolated bundled-service checks and production GUI JS Unicode
edit/undo/redo/save/close acceptance. Artifact:
`build/packages/astra-bfd122e-windows-x64.zip`. Its exact commands, SHA-256,
source/binary comparisons and remaining acceptance limits are in `STATUS.md`.
Native Linux/macOS preparation and packaging now have source
implementations, but have never been run or tested on this Windows host.

Build the Release application and `astra-server` with the repository's xmake
wrapper. The `astra.resources` rule in `xmake/resources.lua` puts `data`, the full
prepared language/Git payload, notices, `astra-process-host.exe` and
`astra-clipboard-host.exe` next to each
application, and copies the same resources into `install/bin` on installation.
`astra.graphics-runtime` adds SDL3, ICU data and DXC next to GUI applications.
The Windows preparation step must precede the application build/install:

```powershell
./scripts/prepare-windows-runtime.ps1
./scripts/xmake.ps1 f -m release -y
./scripts/xmake.ps1 build -y astra astra-server
./scripts/package-windows.ps1 -Offline
./scripts/verify-windows-package.ps1 -PackageDirectory build/packages/astra-0.1.0-windows-x64 -RunSmoke
# On an interactive Windows desktop, also check the extracted production GUI:
./scripts/verify-windows-package.ps1 -PackageDirectory build/packages/astra-0.1.0-windows-x64 -RunSmoke -RunGuiSmoke
```

The coordinator runs shared xmake and clean-package acceptance. The package
script itself never builds targets and never modifies an existing output folder.
Use `-OutputDirectory build/packages/another-name` for a second package.
`-NoArchive` omits the ZIP; `-Offline` refuses missing download archives.

Runtime preparation uses private staging and content-addressed directories under
`.tools/runtime/`; it does not replace the `.tools/lsp` executables that may be in
use by tests. It hashes pinned archives before extracting into fresh directories,
rejects archive traversal and links, checks npm SHA-512 integrity and licenses,
and rejects undeclared npm runtime dependencies. The pointer identifies a fully
prepared inventory. All downloads remain in `.tools/`. Reusing a runtime cache
validates every file and rejects unlisted files.

The package includes four Astra executables, SDL3, ICU data, DXC, complete clangd
resources, Node with its license, TypeScript, TypeScript Language Server,
basedpyright with typeshed, and the complete official MinGit archive. Git and
language services resolve these files relative to the application. C++ dependency
notices come from the same hash-pinned source archives as the recipes; Skia's
source/dependency notices have a checked-in per-file hash list. Both Abseil versions
(Dawn's fixed revision and RE2's package version) retain their own notices.

`provenance/` records dependency pins, graphics compiler arguments, graphics
artifacts and hashes of the actual repository source snapshot. A dirty source
tree is recorded explicitly. `package-manifest.json` hashes every payload file;
the ZIP has a separate SHA-256. ZIP entries have fixed ordering and timestamps,
so the same payload produces the same ZIP. This does not assert bitwise
reproducibility of MSVC-produced binaries or cryptographic publisher identity.

Default verification is static: complete inventory, file sizes/hashes, required
resources/notices and x64 PE identities. `-RunSmoke` starts independent packaged
processes with a fresh Unicode/spaced cwd, isolated user/state/temp directories
and a PATH containing only Windows directories. It checks both application
version entry points, real TCP requests, Unicode durable save, bundled Git status,
actual clangd/TypeScript/Python diagnostics and recovery checkpoint. It preserves
the fixture and stderr after failures. No global language runtime or development
cache is added to the child PATH. This smoke test does not replace the GPU/GUI
workflow tests, physical DPI/IME checks or manual release acceptance.

`-RunGuiSmoke` additionally invokes the repository-owned
`tests/package_gui_smoke.ps1`. It starts the packaged production GUI with a fresh
workspace-relative user script, isolated profile/state/temp and Windows-only PATH.
Shipped JS opens a real file, inserts Unicode through the shared dispatcher,
undoes/redoes the atomic edit, saves and closes through the guarded lifecycle.
The fixture independently checks durable bytes after successful process exit.
Its deadline is 30 seconds; child stdout/stderr and fixture files are retained
under `build/`, including on timeout. This checks package relocation and actual
native rendering/API integration. It does not replace SDL keyboard/pointer,
default visual matrix or installed-IME tests. It is opt-in because it requires
an interactive native Windows GPU session.

The runtime includes upstream license files and component source references.
Before externally distributing MinGit or other copyleft components, preserve
their notices and fulfill their corresponding-source obligations for the exact
component versions. Package integrity validation does not certify licensing.

## Native macOS/Linux source implementation (unverified)

`release/native-tools.json` fixes Node 24.21.0 for each native x64/arm64 platform,
clangd 21.1.8 official Linux/macOS binaries, and LLVM 21.1.8 sources. Its hashes
come from Node's official SHASUMS256 and GitHub's release-asset digests. Linux
arm64 has no matching clangd asset in that release and therefore uses the fixed
LLVM source build. `--clangd-source` also selects that path on any native host
whose CPU or system ABI cannot run the official archive. It builds clangd and
Clang resource headers; it does not replace language features with placeholders.

The native preparation script builds Git 2.56.0 from the kernel.org archive with
fixed OpenSSL 3.5.9 and curl 8.22.0. Complete standalone zlib and expat static
libraries are built from the same fixed Git revisions as the graphics package,
using private exported source trees and their matching generated headers. This
avoids relying on Skia's incomplete third-party zlib archive or modifying its
shared checkout. Those libraries are linked statically into Git. The fixed Git,
OpenSSL, curl, zlib and expat source archives are kept in the native payload along with the
build script and notices. Git local and HTTP(S) operations are retained; Tcl/Tk
Git GUIs, Perl-only commands and translated Git messages are outside Astra's Git
interface. SSH, if explicitly used, uses the operating system's SSH client.
Curl uses Apple SecTrust on macOS and the system certificate store on Linux.

Run these commands **only on the corresponding native host**, after installing
the native C/C++ toolchain, xmake 3.1.1, Python 3.12+, make, Perl and the graphics prerequisites
listed in `GRAPHICS.md`. They are documentation, not commands executed here:

```sh
python3 scripts/build-graphics.py --configuration Release
python3 scripts/prepare-native-runtime.py
# Add --clangd-source for a native LLVM build or --offline to forbid downloads.
python3 scripts/xmake-native.py f -m release -y
python3 scripts/xmake-native.py build astra astra-server
python3 scripts/package-native.py
python3 scripts/package-native.py --verify build/packages/astra-0.1.0-linux-x64
```

Replace the last directory with the actual native package name. These entry points
reject Windows and derive the CPU/platform from the host; they expose no cross-build
switch. All sources, tools, compiler temporary directories and dependency outputs
stay inside `.tools/`. Downloads are checked before extraction; archive paths and
internal source links are constrained to staging. Existing outputs are not replaced.
The native xmake wrapper verifies the installed xmake version, prepares the exact
recipe archive and grammar sources from the existing locks, and directs xmake's
state, package cache and installations into `.tools/`. It accepts `--offline` for
those preparation downloads. Native toolchain prerequisites are not installed or
modified by these scripts.

The native runtime pointer is `.tools/runtime/<platform>-<architecture>.json`.
The package records file hashes, executable permissions and bounded relative
symlinks, rejects broken links and requires the SDL shared runtime. It checks
ELF/Mach-O identity and the selected CPU (including universal Mach-O slices)
without loading the binaries, and creates a deterministic-order, fixed-time tar.gz. A package targets
the host's system ABI; it does not claim one Linux binary works on all distributions.
Linux still needs its Vulkan driver, Fontconfig and X11/Wayland runtime libraries;
macOS uses its native frameworks and system fonts. The current output is a
relocatable directory, not a signed/notarized `.app` or platform installer.
Native build, relocation, ABI, display, process-lifetime and runtime smoke tests
remain required on their actual platforms before release claims can be made.
