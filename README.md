# Astra

Astra is a native, scriptable code editor built around C++23, xmake, SDL3,
Skia Graphite, Dawn, QuickJS-NG and libuv. It implements the contract in
[EditorDoc.md](EditorDoc.md): editing, multicursor undo, a workspace tree,
search and replacement, Tree-sitter syntax, language services, Git and recovery.
The GUI, JavaScript commands and external automation share one document service.

Windows Debug and Release each pass the full 408-case suite. Four real GUI
workflows and the TCP GUI variant also pass in both configurations; exact source
snapshots and package evidence are recorded in [STATUS.md](docs/STATUS.md).
The Windows ZIP from clean source commit `afcfbac` passed inventory, archive
round-trip and isolated runtime smoke checks. Linux/macOS source and native build
scripts are present but have not been built or tested in this Windows environment.

## Build on Windows

Use Windows x64, PowerShell 7, Git, and Visual Studio C++ tools with a Windows
SDK and C++23 support. Run from the repository root. Downloads are pinned and
verified; tools and caches stay under `.tools/`, outputs under `build/`.

```powershell
./scripts/prepare-grammars.ps1
./scripts/build-graphics.ps1 -Stage Build -Configuration Release -Jobs 8
./scripts/build-graphics.ps1 -Stage Build -Configuration Debug -Jobs 8
./scripts/prepare-language-tools.ps1 -IncludeClangd
./scripts/prepare-windows-runtime.ps1
./scripts/xmake.ps1 f -m debug -y
./scripts/xmake.ps1 build -y astra astra-server
./scripts/xmake.ps1 run astra --workspace . --open README.md
```

The graphics build compiles real Skia/Dawn and takes substantial time and disk
space. Runtime preparation supplies bundled language servers and Git; the build
copies them, their licenses, ICU data, SDL3 and DXC beside the applications.
Keep that directory together when running an executable. See
[BUILD.md](docs/BUILD.md) for prerequisites, test targets and package commands.

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
| Next / previous tab | `Ctrl+Tab` / `Ctrl+Shift+Tab` |

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
./scripts/xmake.ps1 run astra-server --workspace . --port 7777
./scripts/xmake.ps1 run astra --connect 127.0.0.1:7777 --open README.md
```

An automation client can send the same JSON-RPC requests:

```powershell
./scripts/xmake.ps1 run astra-server --connect 127.0.0.1:7777 --request '{"jsonrpc":"2.0","id":"info","method":"workspace.info","params":{}}'
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
./scripts/xmake.ps1 run astra --workspace . --config astra.user.js
```

Settings merge with defaults; bindings and commands override individually.
EditorConfig supplies per-file indentation, encoding and line-ending policy.
C/C++, JavaScript/TypeScript and Python use the prepared language tools; available
actions depend on each server's capabilities. See [languages/README.md](languages/README.md).

Read [AGENTS.md](AGENTS.md) before contributing. Live collaboration, embedded AI
agents, an embedded terminal, and run/debug interfaces are outside this product's scope.
