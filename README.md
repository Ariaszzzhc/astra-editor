# Astra

Astra is a native, scriptable code editor built around C++23, CMake/Ninja, vcpkg, SDL3,
Skia Graphite, Dawn, QuickJS-NG and libuv. It is developed against the contract in
[EditorDoc.md](EditorDoc.md): editing, multicursor undo, a workspace tree,
search and replacement, Tree-sitter syntax, language services, Git and recovery.
The GUI, JavaScript commands and external automation share one document service.

The workbench is being revised toward JS-owned product policy, asynchronous user
APIs and shared native presentation components. Windows acceptance exercises
Debug/Release, real GPU rendering, GUI editing/services/recovery/presentation and
the TCP GUI variant; current results and remaining work are recorded in
[STATUS.md](docs/STATUS.md). A previous Windows ZIP from source commit `afcfbac`
passed inventory, archive round-trip and isolated runtime smoke checks. The
current workbench revision still requires final visual and distribution acceptance.
Linux/macOS source and native build scripts are present but have not been built
or tested in this Windows environment.

## Build on Windows

Use Windows x64, PowerShell 7, Git, and Visual Studio C++ tools with a Windows
SDK and C++23 support. Run from the repository root. Downloads are pinned and
verified; tools and caches stay under `.tools/`, outputs under `build/`.

```powershell
git submodule update --init --recursive
./scripts/cmake.ps1 --preset windows-debug
./scripts/cmake.ps1 --build --preset windows-debug
./build/cmake/windows-debug/bin/astra.exe --workspace . --open README.md
./scripts/test.ps1 -Mode all -Gui
```

Configuration prepares fixed grammars, official vcpkg dependencies, bundled
language servers and Git. The required Skia shaping/QuickJS context patches
remain in reviewed overlays. /MD is the Windows default, /MDd in Debug.
Keep the staged runtime directory together. See [BUILD.md](docs/BUILD.md) for
prerequisites and test options, and [PACKAGING.md](docs/PACKAGING.md) for the
install-based Windows distribution pipeline. Results are recorded in STATUS.md.

## Editing

Open an explicit workspace with `--workspace DIR`; file arguments and `--open`
paths are relative to it. The command palette exposes reload, recovery, outline,
syntax-object selection, language actions and Git operations.

| Action | Default key |
| --- | --- |
| New / open / save / Save As | `Ctrl+N` / `Ctrl+P` / `Ctrl+S` / `Ctrl+Shift+S` |
| Command palette | `Ctrl+Shift+P` or `F1` |
| Undo / redo | `Ctrl+Z` / `Ctrl+Y` or `Ctrl+Shift+Z` |
| Add cursor above / below | `Ctrl+Alt+Up` / `Ctrl+Alt+Down` |
| Select next occurrence | `Ctrl+D` |
| Find / replace in the document | `Ctrl+F` / `Ctrl+H` |
| Search / replace across the workspace | `Ctrl+Shift+F` / `Ctrl+Shift+H` |
| Case / regex / whole-word search options | `Alt+C` / `Alt+R` / `Alt+W` in the workspace search prompt |
| Completion / definition / references / rename | `Ctrl+Space` / `F12` / `Shift+F12` / `F2` |
| Next / previous tab | `Ctrl+Tab` / `Ctrl+Shift+Tab`, `Ctrl+PageDown` / `Ctrl+PageUp` |
| Scroll a document page up / down | `PageUp` / `PageDown` |

Workspace search includes unsaved and untitled buffers. Replacement requires
confirmation; edits to those buffers remain dirty and undoable. External file
changes reload clean buffers, while conflicts preserve dirty text and display
a persistent notice. Reloading dirty content requires confirmation and refuses
intervening edits. Recovery previews require an explicit restore action; restored
text stays unsaved until Save. [RECOVERY.md](docs/RECOVERY.md) describes storage.

## Shared local/TCP service

The normal GUI uses an in-process service. To run a separate local service and
attach a GUI, use two terminals:

```powershell
./build/cmake/windows-debug/bin/astra-server.exe --workspace . --port 7777
./build/cmake/windows-debug/bin/astra.exe --connect 127.0.0.1:7777 --open README.md
```

An automation client can send the same JSON-RPC requests:

```powershell
./build/cmake/windows-debug/bin/astra-server.exe --connect 127.0.0.1:7777 --request '{"jsonrpc":"2.0","id":"info","method":"workspace.info","params":{}}'
```

The listener accepts loopback addresses only. Remote use goes through an
authenticated SSH tunnel. [SERVICE.md](docs/SERVICE.md) documents framing,
notifications, revision checks, asynchronous operations and the JavaScript API.

## User JavaScript

Defaults are supplied by [data/defaults.js](data/defaults.js). Put overrides in a
workspace-relative file such as `astra.user.js` and load it explicitly:

```javascript
astra.configure({editor: {tabSize: 2, insertSpaces: true}});
astra.registerCommand("custom.comment", parameters =>
    astra.request("document.command", {...parameters, command: "toggle_line_comment"}));
astra.bind("Ctrl+Alt+C", "custom.comment");
```

```powershell
./build/cmake/windows-debug/bin/astra.exe --workspace . --config astra.user.js
```

Settings merge with defaults; bindings and commands override individually.
EditorConfig supplies per-file indentation, encoding and line-ending policy.
C/C++, JavaScript/TypeScript and Python use the prepared language tools; available
actions depend on each server's capabilities. See [languages/README.md](languages/README.md).

Read [AGENTS.md](AGENTS.md) before contributing. Live collaboration, embedded AI
agents, an embedded terminal, and run/debug interfaces are outside this product's scope.
