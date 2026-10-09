# Implementation and acceptance status

Product contract: `EditorDoc.md`. Product name: Astra (confirmed by the user).

No GUI, release, or non-Windows platform is currently verified.

| Milestone | Scope | State | Acceptance evidence |
| --- | --- | --- | --- |
| M0 | xmake, test runner, development rules, reproducible dependencies | In progress | Repository inspected; no prior implementation |
| M1 | Persistent SumTree, UTF-8 Rope, positions, transactions, multicursor undo/redo | Pending | Random differential tests and Unicode boundary tests required |
| M2 | Workspace, safe file I/O, recovery, EditorConfig, search/replace | Pending | Real filesystem and failure-path tests required |
| M3 | libuv service, request/response/notifications, local/TCP clients, Agent API | Pending | Same scenarios on in-process and TCP transports required |
| M4 | QuickJS-NG scripting, default commands and VS Code keymap | Pending | API, exceptions, limits, command dispatch tests required |
| M5 | Tree-sitter syntax/textobjects/outline and LSP | Pending | Real grammars and language server integration required |
| M6 | SDL3 + Skia Graphite + Dawn GUI, layout, text input, themes/icons | Pending | Windows GPU rendering and input automation required |
| M7 | Complete workspace UI, git, global search, command palette | Pending | End-to-end user workflows required |
| M8 | Packaging, recovery/failure/performance acceptance, documentation | Pending | Clean Windows package smoke test required |

## Validation boundary

Only Windows tests may run in this environment. Cross-platform code paths and configuration
do not constitute Linux/macOS verification. No CI jobs for those systems are enabled here.

## Excluded by the product contract

Live collaboration, embedded AI agents, embedded terminal, run, and debug interfaces.
External AI Agent control for editor testing remains required.
