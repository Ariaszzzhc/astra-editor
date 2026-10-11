# Astra for Windows x64

Extract the complete directory, then run `astra.exe --workspace PATH`.
The `resources`, `tools`, `licenses` and `provenance` directories belong with the
executables. No global Node.js, Git or language-server installation is required.
A Direct3D 12 capable Windows graphics driver is required for the native renderer.

`astra-server.exe --workspace PATH --port 0` starts the same document/service
engine without a GUI. The first output line reports its localhost TCP port.
`astra-server.exe --connect 127.0.0.1:PORT --request JSON` sends a JSON-RPC request.
Use `--help` for other options. The default listener is localhost; do not expose
the unauthenticated development protocol to an untrusted network.

Defaults and key bindings live in `resources/defaults.js`. A workspace-relative user
script can be supplied with `--config PATH`. File-level EditorConfig settings
override applicable defaults. Bundled language services cover C/C++, JavaScript,
TypeScript and Python. Project dependencies, compilers and Python installations
still determine which external libraries those servers can resolve.

Recovery writes to the user's local state directory by default. Use
`--state-directory PATH` to choose a different location. Recovery offers saved
sessions explicitly; restoring a buffer does not overwrite its file on disk.

See `THIRD-PARTY.md` for dependency notices and source references. The SHA-256
inventory in `package-manifest.json` detects missing, modified or unexpected
payload files. It is an integrity inventory, not a publisher signature.
