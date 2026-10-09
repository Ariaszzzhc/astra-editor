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

Graphite requires a Skia build that explicitly enables Graphite and Dawn. The generic xmake
Skia package does not currently expose those options and cannot satisfy the renderer contract.
The renderer will use a pinned Skia revision and its matching Dawn dependency.
