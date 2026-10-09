# Implementation and acceptance status

Product contract: `EditorDoc.md`. Product name: Astra (confirmed by the user).

The native Windows renderer and text layout are verified in Debug and Release.
The complete GUI application and production packaging remain in progress. No
Linux/macOS functionality has been tested in this Windows environment.

| Milestone | Scope | State | Acceptance evidence |
| --- | --- | --- | --- |
| M0 | xmake, test runner, development rules, reproducible dependencies | Complete | Pinned xmake and recipe archives verified; two repository skills passed official validator |
| M1 | Persistent SumTree, UTF-8 Rope, positions, transactions, multicursor undo/redo | Complete | Windows Debug and Release: 35 cases, 185,791 assertions each |
| M2 | Workspace, safe file I/O, recovery, EditorConfig, search/replace | In progress | Real asynchronous open/save, disk conflicts, concurrent save/typing, literal search/replace and EditorConfig pass; recovery and broader search remain |
| M3 | libuv service, request/response/notifications, local/TCP clients, Agent API | In progress | Real local/TCP parity, cancellation, bounded queues, disconnect/timeout/shutdown, Windows server CLI and Unicode save pass; GUI/Agent acceptance remains |
| M4 | QuickJS-NG scripting, default commands and VS Code keymap | In progress | Runtime isolation/limits, settings merge, custom commands and bundled defaults pass; GUI keymap acceptance remains |
| M5 | Tree-sitter syntax/textobjects/outline and LSP | In progress | Nine pinned grammar parsers and incremental syntax pass in Debug/Release; language-service application and bundled tools are under acceptance |
| M6 | SDL3 + Skia Graphite + Dawn GUI, layout, text input, themes/icons | In progress | Real D3D12/Graphite pixels, SDL resize/minimize/restart, SkParagraph Unicode/caret/hit/selection/DPI tests pass in Debug and Release; editor GUI acceptance remains |
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
- Full suite before GUI/LSP/Git integration: Release 248 cases / 201,226 assertions;
  Debug 248 cases / 201,230 assertions. Both include real Tree-sitter grammars and
  the corrected Markdown inline regression. Async polling adds a variable number
  of successful assertions without changing the test cases.
- `graphics-smoke`, `renderer-smoke`, and `text-layout-tests` pass through the main
  xmake build in Debug and Release, using actual GPU surfaces and readback.
- Persistent core plus Tree-sitter acceptance: Debug 64 cases / 188,941 assertions;
  Release core 41 cases / 187,687 assertions and syntax 23 cases / 1,254 assertions.
  These include shared-subtree diff skipping with exact Unicode edge refinement,
  independently prepared document clones, real incremental parses, Markdown inline
  regressions, highlighting, outline and textobjects.
- `./tests/cli_smoke.ps1 -Mode release` passes: independent server/client processes,
  Content-Length TCP requests, Unicode/spaced workspace paths, literal backslashes,
  and durable asynchronous save. The fixture terminates only its own temporary server.
- Resolved build infrastructure failures: Windows sandbox blocked MSVC PDB/MSYS named
  objects; normal Windows execution plus embedded CodeView records works. xmake's automatic
  partial-clone lock path recursed on this host; the checked-in manifest now locks a
  SHA-256-verified official recipe archive used through xmake's supported local repository.

## Excluded by the product contract

Live collaboration, embedded AI agents, embedded terminal, run, and debug interfaces.
External AI Agent control for editor testing remains required.
