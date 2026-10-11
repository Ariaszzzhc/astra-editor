# Distribution pipeline

CMake install is the authority for payload membership. Packaging scripts build
production targets, install the Runtime component into fresh staging, record
source provenance and a complete inventory, verify it, then ask CPack to archive
that same installation. They do not maintain another list of files to copy.
Verification has a required-file list to detect incomplete distributions.

## Windows x64

```powershell
./scripts/package-windows.ps1 -OutputDirectory build/packages/astra-review
./scripts/verify-windows-package.ps1 -PackageDirectory build/packages/astra-review -RunSmoke -RunGuiSmoke
Expand-Archive build/packages/astra-review.zip build/package-roundtrip-review
./scripts/verify-windows-package.ps1 -PackageDirectory build/package-roundtrip-review/astra-review -RunSmoke -RunGuiSmoke
```

The default production preset is windows-package: Ninja, Release, /MD, no tests
or benchmarks. -BuildDirectory can select another configured Release CMake tree
from this checkout. -Offline requires cached grammar/runtime archives; vcpkg has
its own download and binary cache policy. -NoArchive produces only the verified
directory. Existing directories or archives are rejected rather than overwritten.

The Runtime component includes the GUI/server, production process/clipboard
hosts, SDL3, DXC, app-local redistributable Visual C++ runtime, JS resources,
complete language tools, Git, grammar/vcpkg licenses and dependency provenance.
ICU data comes from the official static ICU dependency. Test executables,
fixtures, benchmarks and Debug CRT DLLs are rejected by package verification.

package-manifest.json records every payload path, size and SHA-256, the source
commit/dirty state, vcpkg commit and CRT policy. provenance/source-files.json
records actual source bytes, including uncommitted changes; the submodule gitlink
is represented by its explicit commit. provenance/vcpkg-status.txt records
installed dependency versions, features and ABI records. Port recipes, patches
and manifests are included in provenance. Each installed dependency's official
copyright collection is preserved under licenses/vcpkg. CPack produces a ZIP
and the script writes its detached SHA-256. This establishes file integrity,
not publisher authentication or bitwise reproducibility of MSVC binaries.

Static verification checks the complete inventory, required tools/resources,
notices, CRT DLLs and x64 PE executable identities. -RunSmoke starts the installed
server in a fresh Unicode/spaced workspace with an isolated profile/state/temp
and Windows-only PATH. It checks version entry points, real TCP dispatch,
durable Unicode save, bundled Git, real C++/TypeScript/Python diagnostics and
recovery. A global language tool or the development cache cannot enter that PATH.

-RunGuiSmoke invokes tests/package_gui_smoke.ps1 against the installed production
GUI. Shipped JS opens a file, performs Unicode editing and atomic undo/redo,
saves and closes through the shared dispatcher/lifecycle. The independent fixture
checks durable file bytes and exit status. It requires a native desktop/GPU and
preserves failure artifacts under build/. It does not establish physical DPI,
IME-profile or adapter-removal acceptance. Actual outcomes belong in STATUS.md.

## Native macOS/Linux source (unverified)

Only native hosts may run prepare-native-runtime.py and package-native.py. Both
reject Windows and unsupported host architectures before preparation. No native
build/test/emulator/cross-platform package operation is executed in the Windows
acceptance workflow, and source review does not establish native acceptance.

The same CMake target/install rules are used. Native runtime preparation receives
the configured CMake/Ninja executables and vcpkg dependency prefix, prepares fixed
Node/language tools, and builds its pinned Git/HTTP runtime sources. It uses
official installed zlib/expat rather than exporting dependencies from a graphics
checkout. Source archives/notices for its Git runtime stay in the distribution.
package-native.py takes --build-directory and --cmake, invokes the Runtime
installation and verifies a native inventory before using CPack TGZ. Native
relocation, runtime ABI, signing/notarization and platform release acceptance
remain unverified. Do not infer them from Windows success.
