# Astra third-party software

This distribution preserves the notices of its statically linked libraries in
`licenses/`, and those of the complete language tools and MinGit distributions in
`tools/`. Windows includes MinGit; native macOS/Linux payloads build Git from the
fixed source archives included under `provenance/sources/`. `provenance/` records fixed upstream versions, source locations and
archive hashes. `package-manifest.json` identifies every distributed file.

Skia, Dawn and the installed C++ dependency graph retain the official vcpkg port
copyright collections under `licenses/vcpkg/`. Exact package versions/features
are recorded in `provenance/vcpkg-status.txt`; the vcpkg source commit is recorded
in `package-manifest.json`. Tree-sitter parsers retain each grammar's license.

The Node.js license includes its bundled component notices. TypeScript's third-party
notice, basedpyright's typeshed license, LLVM's license and MinGit's complete
`ucrt64/share/licenses/` directory remain at their upstream locations.

On Windows, MinGit is an independent executable distribution from Git for Windows. Its Git
sources are at https://github.com/git-for-windows/git/tree/v2.56.0.windows.2;
component build recipes and source references are maintained at
https://github.com/git-for-windows/MINGW-packages and
https://github.com/git-for-windows/MSYS2-packages. The exact binary release and its
official checksum are recorded in `provenance/release/windows-tools.json`.
Redistributors must preserve the applicable licenses and satisfy corresponding
source obligations for the components they redistribute; the file inventory is
an integrity record, not a replacement for those license terms.

Astra loads system fonts through DirectWrite. No font files are redistributed.

QuickJS-NG v0.15.1 includes Astra's host async context extension. Its verified
patch, recipe and rationale are included under
`provenance/dependencies/ports/astra-quickjs/`; the source and patch hashes are recorded
in `provenance/dependencies.lock.json`. The upstream MIT notice remains in
`licenses/vcpkg/astra-quickjs/copyright`. The Skia glyph-cache adapter and its
official-based recipe are preserved under `provenance/dependencies/ports/skia/`.
