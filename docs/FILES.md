# File storage and EditorConfig

`astra::io` provides synchronous filesystem primitives intended for worker threads. It
preserves LF/CRLF bytes and supports strict UTF-8, UTF-8 BOM, and UTF-16 LE/BE BOM decoding.
Latin-1 requires an explicit encoding override (for example from EditorConfig), because
unmarked bytes cannot reliably identify that encoding. Saving Latin-1 rejects unrepresentable
Unicode. Invalid input and embedded NUL fail explicitly; no replacement-character conversion
silently changes user data.

A `FileVersion` retains a shared copy of the encoded file. Saving an existing path requires
its version and compares every byte, detecting changes even when file size and timestamps
match. The retained copy consumes memory proportional to the encoded file, a deliberate
tradeoff for collision-free conflict checks. A save with no expected version creates a new
file only, never overwrites an existing path.

On Windows, saving writes a uniquely created temporary sibling and flushes it before
`ReplaceFileW`/`MoveFileExW`. Existing-file replacement keeps a recovery backup while the OS
operation completes. If an exceptional replacement failure also prevents restoration, the
original data remains at the reported recovery path. A handle denies concurrent writes
during final verification; external rename races are not claimed to be transactional CAS.

`astra::config` parses EditorConfig into ordered sections and diagnostics, resolves explicit
outer-to-inner layers, and supplies typed settings as well as raw properties. It supports
`root`, `unset`, Unicode glob matching, recursive wildcards, classes, alternatives, and numeric
ranges with computation limits. Unknown properties remain available for future consumers;
invalid typed values do not silently become defaults. Workspace integration owns directory
discovery and applying save transformations as observable document changes.

Windows tests exercise real temporary files under `build/`, encoding round trips, Unicode
paths, same-size/same-time conflicts, deletion, read-only/share conflicts, and cleanup. Glob
and inheritance tests are independent of filesystem discovery. These tests do not constitute
the upstream EditorConfig conformance suite or non-Windows platform validation.

Reference: [EditorConfig specification](https://spec.editorconfig.org/).
