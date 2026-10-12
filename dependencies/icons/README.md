# Icon resources

Codicons provides UI/symbol graphics; Material Icon Theme provides file,
language and folder SVGs. manifest.json pins the Codicons source archive and
the official Material 5.39.0 npm release, including generated open folders and
color clones. Only images, association data and notices enter the repository.
The npm extension runtime is not a build or production dependency.

Maintenance: update reviewed versions/hashes in manifest.json, then run
`./scripts/import-icons.ps1`. `-Offline` requires verified cached archives.
The importer preserves upstream SVG bytes and extracts basename associations.
Normal CMake configuration verifies every file and embeds normalized SVG bytes
under build/, using vcpkg's pinned Python on Windows. It never downloads icons.

IconRenderer owns a 32 MiB vector-picture cache per window. Essential UI/default
graphics are prepared at startup; remaining SVGs are parsed/recorded on a
cancellable worker. Painting replays immutable pictures. Current-color UI
graphics follow the theme; language graphics retain upstream colors.

Association metadata is embedded as 128-entry module pages. JavaScript loads
those asynchronously and yields between pages, retaining the existing 8 ms
execution budget and cancellation behavior. UI configuration stores the
selected base theme and at most 256 user overrides per table; the full shipped
catalog is never passed through preference/RPC limits.

Resources, licenses and provenance are installed through cmake/Install.cmake.
See resources/icons/NOTICE.md for attribution and build-time adaptation details.
