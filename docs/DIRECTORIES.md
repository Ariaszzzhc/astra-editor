# Repository directories

| Directory | Responsibility |
| --- | --- |
| `apps/astra/` | GUI and server entry point, compiled with explicit GUI capability |
| `apps/process/`, `apps/clipboard/` | Production process and clipboard helpers |
| `include/astra/` | Public, self-contained C++ interfaces |
| `src/` | Core, services, adapters and native presentation implementation |
| `resources/` | Shipped JavaScript policy, defaults and workbench modules |
| `dependencies/` | Pinned tools, grammar/runtime manifests, patch hashes and necessary vcpkg overlays |
| `third_party/vcpkg/` | Unmodified, commit-pinned official vcpkg submodule |
| `cmake/` | Dependency imports, explicit targets, grammar preparation, CTest and install rules |
| `tests/unit/` | 15 source files / 279 component behavior cases |
| `tests/integration/` | 17 source files / 355 integration cases and CLI acceptance |
| `tests/gui/` | Explicit native rendering, geometry and editor workflow acceptance |
| `tests/environment/` | Installed IME and elevated isolated clipboard acceptance |
| `tests/package/` | Archive/path/hash helper checks and installed GUI acceptance |
| `tests/fixtures/`, `tests/support/` | Test-only sibling processes and shared harness |
| `benchmarks/` | Explicit correctness-checked performance executables |
| `packaging/` | Distribution documentation, runtime asset pins and notices |
| `scripts/` | Thin preparation, formatting, build/test and verified-package entry points |
| `docs/`, `skills/` | Product evidence, contracts and repository workflows |
| `.tools/`, `build/` | Ignored dependency/tool caches and generated outputs/evidence |

Root CMake files and vcpkg manifests select the build/dependency graph. CMake
install rules exclusively select distribution contents. No test, fixture or
benchmark is installed. `astra-tests` groups the two behavior executables;
each behavior source and the shared doctest main are compiled once per build.
The original 632 cases remain, with two migration-discovered scheduler regressions.

Windows commands are in [BUILD.md](BUILD.md); concrete results and limits are in
[STATUS.md](STATUS.md). Native non-Windows sources have not been validated here.
