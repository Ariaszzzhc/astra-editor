# Windows development

Build tool: xmake 3.1.1, installed portably inside `.tools/xmake/`.
Compiler: a C++23-capable Windows toolchain with the Windows SDK.

Dependency artifacts and caches stay inside this checkout. In PowerShell 7 on Windows:

```powershell
./scripts/xmake.ps1 f -m debug -y
./scripts/xmake.ps1 build -y astra-tests
./scripts/xmake.ps1 test -v
./scripts/test.ps1 -Mode all
```

The wrapper downloads xmake from its official GitHub release if needed, validates its pinned
SHA-256, and sets repository-local package/cache paths. The initial configure needs network
access. Test framework: doctest 2.4.12. `dependencies.lock.json` pins both tools and the entire
official package recipe tree (including source hashes). The wrapper verifies and extracts
that tree as a local xmake repository. This avoids xmake 3.1.1's failing local partial-clone
lock path on Windows while keeping dependency versions reproducible. Do not enable automatic
recipe updates or edit downloaded recipes; update the reviewed manifest when upgrading.

Windows static libraries share the `/MT` runtime and `_ITERATOR_DEBUG_LEVEL=0` in both modes
to match Skia's Dawn build; Debug still provides symbols and unoptimized application code.

Graphite uses the verified local package described in `docs/GRAPHICS.md`. Build both
graphics configurations before configuring the full project. SDL3 is a pinned native
window/input package; its separate renderers are disabled because drawing uses Graphite.

Prepare fixed Tree-sitter grammar sources before the full syntax target:

```powershell
./scripts/prepare-grammars.ps1
./scripts/xmake.ps1 build -y astra-tests astra-server
./scripts/xmake.ps1 build -y graphics-smoke renderer-smoke text-layout-tests
./scripts/xmake.ps1 run graphics-smoke
./scripts/xmake.ps1 run renderer-smoke
./scripts/xmake.ps1 run text-layout-tests
```

See `docs/SERVICE.md` for server, local automation, and protocol usage. The GUI and
remaining language/Git/recovery integration are tracked separately in `docs/STATUS.md`.
