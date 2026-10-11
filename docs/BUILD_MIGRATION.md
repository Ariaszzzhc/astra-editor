# CMake, dependency and distribution migration

Authorized scope: CMake + Ninja, a commit-pinned vcpkg submodule, directory/test
organization, one install-based distribution pipeline, removal of xmake. Preserve
all EditorDoc.md features and the QuickJS host async context and Skia shaping
patches. Validation on this host is Windows only.

## Increments

1. Record the contract and introduce the pinned vcpkg tool source.
2. Add official manifest dependencies and CMake production targets. Use the
   pinned official Dawn/Skia recipes, with a Skia shaping patch overlay and the
   QuickJS async-context extension only. Use dynamic CRT (Release MD, Debug MDd)
   with matching STL iterator ABI throughout the dependency graph.
3. Move entry points, resources, dependency manifests and packaging inputs;
   automatically prepare pinned grammar sources and share runtime preparation.
4. Partition behavior tests without duplicate compilation; keep integration,
   native GUI, environment and package acceptance distinct. Move benchmarks.
5. Install production targets and resources, verify the installed inventory and
   runtime relocation, package it, then remove xmake and obsolete wrappers.

Every implementation increment receives affected Windows checks, a short
Conventional Commit without a body, and a non-force push to the configured origin.
Exact outcomes and failures are appended at the top of STATUS.md. Historical
packages and logs remain available; their results do not establish new-build
acceptance. No Linux/macOS job is executed on this host.

## Dependency boundaries

The submodule pins the vcpkg source/tool snapshot. The manifest pins registry
versions; project overlays retain exact sources, patches and feature/ABI settings.
Caches are checkout-local under .tools/. Source submodules contain no Astra edits.
Grammar archives are fixed by commit and SHA-256, compiled from generated parser
and scanner sources, and never generated or downloaded at application runtime.
Node, language servers and Git remain complete, pinned runtime assets separate
from the C++ dependency graph. Their preparation is shared by development and
distribution. Install rules own payload membership; package scripts only
orchestrate verification and archive generation.

## Test retention

Keep randomized differential/snapshot tests, atomic history and failure tests,
real filesystem/process/socket/grammar/language/Git/recovery integration, native
render/input workflows and installed-package relocation. Remove duplicate build
targets and assertions tied only to removed build wrappers. Consolidation must
retain unique assertions. No fixture or benchmark enters an installed package.
