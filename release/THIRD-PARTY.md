# Astra third-party software

This distribution preserves the notices of its statically linked libraries in
`licenses/`, and those of the complete language tools and MinGit distributions in
`tools/`. Windows includes MinGit; native macOS/Linux payloads build Git from the
fixed source archives included under `provenance/sources/`. `provenance/` records fixed upstream versions, source locations and
archive hashes. `package-manifest.json` identifies every distributed file.

Skia, Dawn and their fixed dependency checkouts have individual license texts under
`licenses/graphics/`. This collection conservatively includes notices from those
source trees, including components not linked into Astra. ICU data, DXC and SDL
retain their separate notices. Tree-sitter parsers retain each grammar's license.

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
