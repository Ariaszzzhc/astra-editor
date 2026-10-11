# Astra native Linux/macOS package

Run `./astra --workspace PATH` from the extracted complete directory. The server
entry point is `./astra-server --workspace PATH --port 0`. Keep the `tools`, `resources`,
`licenses` and `provenance` directories with the executables.

This directory contains CPU-specific native binaries. Build and package on the
target system; it is not a universal Linux distribution or a signed/notarized
macOS application bundle. Linux requires the desktop's Vulkan driver, X11 or
Wayland libraries and fontconfig. macOS uses Metal and system frameworks. System
fonts, operating-system libraries and certificate stores are not redistributed.

Bundled language services cover C/C++, JavaScript/TypeScript and Python. Project
SDKs and dependencies still determine resolution of external libraries. Git is
built from the exact upstream sources included in `provenance/sources`, with
static zlib, expat, curl and OpenSSL dependencies. Git's local and HTTP(S)
transports remain available. SSH uses the system's SSH client and user credentials
only when explicitly invoked; the editor does not perform automatic Git network
operations. Separate Git Tcl/Tk GUIs, Perl commands and translated Git output are
not included in this editor's Git runtime.

Recovery uses the user's local state directory; `--state-directory PATH` selects
another location. Restoring a recovery session creates unsaved buffers for review.
The package manifest records content hashes, file modes and internal symlinks.
It is an integrity inventory, not a publisher signature. See `THIRD-PARTY.md`
for notices and dependency source references.
