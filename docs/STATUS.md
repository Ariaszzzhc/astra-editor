# Implementation and acceptance status

Product contract: `EditorDoc.md`. Product name: Astra (confirmed by the user).

No GUI, release, or non-Windows platform is currently verified.

| Milestone | Scope | State | Acceptance evidence |
| --- | --- | --- | --- |
| M0 | xmake, test runner, development rules, reproducible dependencies | Complete | Pinned xmake and recipe archives verified; two repository skills passed official validator |
| M1 | Persistent SumTree, UTF-8 Rope, positions, transactions, multicursor undo/redo | Complete | Windows Debug and Release: 35 cases, 185,791 assertions each |
| M2 | Workspace, safe file I/O, recovery, EditorConfig, search/replace | In progress | File storage implementation awaiting integration tests |
| M3 | libuv service, request/response/notifications, local/TCP clients, Agent API | In progress | Framing/dispatcher/service implementation awaiting integration tests |
| M4 | QuickJS-NG scripting, default commands and VS Code keymap | Pending | API, exceptions, limits, command dispatch tests required |
| M5 | Tree-sitter syntax/textobjects/outline and LSP | Pending | Real grammars and language server integration required |
| M6 | SDL3 + Skia Graphite + Dawn GUI, layout, text input, themes/icons | In progress | Pinned Skia/Dawn dependency verification and Release GN configure passed; library compilation underway |
| M7 | Complete workspace UI, git, global search, command palette | Pending | End-to-end user workflows required |
| M8 | Packaging, recovery/failure/performance acceptance, documentation | Pending | Clean Windows package smoke test required |

## Validation boundary

Only Windows tests may run in this environment. Cross-platform code paths and configuration
do not constitute Linux/macOS verification. No CI jobs for those systems are enabled here.

## Windows evidence (2026-10-09)

- Toolchain: xmake 3.1.1, Visual Studio 2026 x64, C++23, `/MT`, warnings treated as errors.
- `./scripts/xmake.ps1 f -m debug -y`, `build -y astra-core-tests`,
  `run astra-core-tests --no-version`: 35 cases / 185,791 assertions passed.
- Same commands with `-m release`: 35 cases / 185,791 assertions passed.
- Core cases include 5,000 randomized tree operations, 3,000 Unicode Rope edits,
  12,000 repeated insertions, 1,500 randomized history operations, and 400 Unicode
  multicursor rounds, with deterministic seeds and flat reference models.
- Both repository skills passed `skill-creator/scripts/quick_validate.py`.
- Resolved build infrastructure failures: Windows sandbox blocked MSVC PDB/MSYS named
  objects; normal Windows execution plus embedded CodeView records works. xmake's automatic
  partial-clone lock path recursed on this host; the checked-in manifest now locks a
  SHA-256-verified official recipe archive used through xmake's supported local repository.

## Excluded by the product contract

Live collaboration, embedded AI agents, embedded terminal, run, and debug interfaces.
External AI Agent control for editor testing remains required.
