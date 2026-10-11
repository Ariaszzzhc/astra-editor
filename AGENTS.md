# Astra development rules

`EditorDoc.md` is the product contract. The product name is **Astra**.

## Scope and workflow

- Work only in this repository. Do not inspect other local projects or use project memories.
- Develop autonomously within the requested scope. Preserve every requirement in EditorDoc.md.
- Validate on Windows only in this environment. Do not run Linux/macOS builds, emulators,
  cross-platform test jobs, or claim those platforms are verified.
- Track implementation and concrete acceptance evidence in `docs/STATUS.md`.
- With parallel agents, one coordinator owns CMake configure/build/CTest invocations in
  the shared checkout. Other agents submit tests and receive results from that coordinator.
- Commit coherent, tested increments. Use conventional commit subjects shorter than 50
  characters and no body. Push to `https://github.com/Ariaszzzhc/astra-editor` without
  force-pushing or overwriting remote history.
- Never label a stub, disabled feature, mock backend, or unfinished milestone complete.

## Architecture

- C++ owns the document model and services; QuickJS-NG hosts JavaScript policy and user APIs.
- Use CMake with Ninja and a commit-pinned vcpkg submodule. SDL3 owns windows/input;
  Skia Graphite and Dawn own drawing/presentation;
  libuv owns asynchronous I/O. Do not substitute a different production graphics stack.
- The service owns documents and workspace state. GUI, scripts, and Agent automation
  invoke the same command dispatcher. In-process and remote clients share protocol semantics.
- Persistent measured SumTrees underpin ordered indexed data. Rope operations must preserve
  bounded chunks, structural sharing, cached summaries, and logarithmic seeking/edit paths.
- Distinguish UTF-8 byte offsets, Unicode scalars, grapheme boundaries, UTF-16 LSP positions,
  and visual columns. No byte-wise user cursor movement through UTF-8 sequences.
- One user edit is one atomic transaction, including all cursors and selection history.
  Reject overlapping invalid edits and stale revisions before mutating any state.
- External I/O and parser work must not block input/rendering. Own handles with RAII;
  define cancellation, bounded buffering, timeouts, and shutdown for asynchronous work.
- Preserve unsaved content and detect external save conflicts. Never silently truncate,
  replace, or discard data after a failure.

## C++ and testing

- Format repository-owned C++ and JavaScript with `./scripts/format.ps1` before
  committing. `./scripts/format.ps1 -Check` is an acceptance gate. Use the pinned
  repository-local clang-format and Prettier; do not compress functions, branches,
  or multiple statements onto one line. Preserve third-party sources.
- Use modern C++23, value types, explicit ownership, const correctness, and named invariants.
  No owning raw pointers, mutable globals, shell-string construction from user paths,
  or catch-and-ignore error paths. Keep third-party APIs behind narrow adapters.
- Headers are self-contained. Place public interfaces under `include/astra/`, implementation
  under `src/`, and behavior tests under `tests/`.
- Pin fetched dependencies and verify artifact hashes. The vcpkg source submodule lives
  under `third_party/vcpkg`; tool binaries/dependency caches stay under `.tools/` and
  build outputs under `build/`. The ignored vcpkg bootstrap executable stays in
  its upstream root as required by the official toolchain. Never commit caches
  or generated outputs. Keep upstream
  source unchanged; required patches and recipes belong in `dependencies/ports/`.
- CMake install rules are the authority for distribution contents. Do not maintain a
  second payload file list in packaging scripts. Tests, fixtures and benchmarks are
  never installed. GUI/environment checks are explicit opt-ins, not silent skips.
- Add meaningful unit, randomized differential, integration, and failure-path tests. Tests
  must exercise observable invariants rather than repeat implementation details.
- Run affected tests before each implementation commit. Run Debug and Release checks at
  milestone boundaries. Record exact commands and failures honestly.

## Repository skills

- Read `skills/astra-core/SKILL.md` for document, SumTree, Rope, Unicode, and transaction work.
- Read `skills/astra-validation/SKILL.md` for build, test, GUI automation, and release work.
