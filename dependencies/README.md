# Dependency ownership

`third_party/vcpkg` is an unmodified, commit-pinned Git submodule. `vcpkg.json`
and `vcpkg-configuration.json` select the baseline and local overlays. Update
the gitlink, baseline and tool pin together in a reviewed dependency change.

`tools.json` pins the CMake, Ninja and formatting tools. Download archives,
installed dependencies, build trees and binary caches stay under `.tools/`.
The Windows wrapper prevents global CMake/Ninja versions from choosing the
project toolchain; vcpkg helper tools are also downloaded from its pinned metadata.

The Astra triplet uses static C++ libraries with a dynamic CRT: Release `/MD`,
Debug `/MDd`, standard STL iterator checking. SDL is a shared library. CRT
linkage and library linkage are independent choices: static libraries reduce
the shipped DLL surface while the shared CRT provides consistent runtime state.

Dependencies use the official ports at the fixed baseline, including Dawn and
SDL3. Skia's overlay is a copy of the official port with only the required glyph
cache adapter patch added. Its internal GN build stays owned by the official
recipe. QuickJS retains its reviewed host async-context extension and fixed
0.15.1 source. The source archives and patch content participate in vcpkg's ABI
and binary-cache identity. No custom graphics builder or CRT patch is required.

Grammar sources and executable language/Git runtimes remain separately pinned;
they are application build/runtime inputs, not a globally installed SDK.
`grammars/manifest.json` owns grammar archive/input hashes; `runtime/manifest.json`
and packaging tool manifests own executable runtime assets. `patches.json` records
only necessary patch hashes. C++ versions come from the vcpkg baseline/manifest;
there is no second library-version lock or graphics dependency registry.
