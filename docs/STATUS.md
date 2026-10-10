# Implementation and acceptance status

Product contract: `EditorDoc.md`. Product name: Astra (confirmed by the user).

## Working-tree formatting check (2026-10-10)

The new, uncommitted `layout_jobs.hpp` and `layout_jobs.cpp` drafts contained
compressed control flow before formatting. Ran `./scripts/format.ps1 -Offline`,
then `./scripts/format.ps1 -Check -Offline` on native Windows: all 139 C++ and
12 JavaScript files passed. Existing tracked source files were unchanged.
At that point the layout job drafts were not integrated or behavior-validated;
that check established formatting only. Integration evidence follows below.

## Active architecture and workbench revision (2026-10-09)

The previous Windows acceptance below covers the earlier implementation. It does
not establish acceptance of the requested JS-owned workbench, asynchronous user
API, cohesive UI, or indexed incremental presentation model. That revision is
**in progress**, and is not a completed product or milestone.

### Shared read-only text surfaces (2026-10-10)

Text panels, choice details and confirmation/progress details now use
`TextSurface`; their three native synchronous row caches were removed.
The component owns the persistent measured logical-line index, a bounded visible
page and the existing exact-style paragraph engine. It shares the libuv layout
queue with documents. Long cold paragraphs shape off the UI thread; repeated
frames coalesce pending work, overlapping pages reuse exact cached paragraphs,
and page/content/style changes discard obsolete results. Theme/tone decisions,
placement and declared actions remain in the caller. Rows following an unknown
wrapped paragraph height are prepared but withheld from drawing until preceding
geometry is known. Dialog destruction cancels its own pending work; controls and
cancellation remain active while glyph layout is pending.

Source limits are named on `TextLineIndex`: 8 MiB / 100,000 logical rows. A surface
page contains at most 256 rows; the default cache remains 256 entries / 8 MiB of
accounted text/style. Content above the byte limit is rejected before Unicode scanning.
Failed UTF-8/content/page validation preserves the current content/layout.
Native Agent view state exposes surface row/page/cache/pending observations.

Focused Windows evidence:

- `./scripts/xmake.ps1 build -y viewport-layout-tests astra
  gui-presentation-smoke`: passed in Debug.
- `./build/windows/x64/debug/viewport-layout-tests.exe`: passed with real Windows
  font/SkParagraph pixels. Additional surface checks cover mixed CR/LF/CRLF byte
  extents, immutable retained source, failed reset/page preservation, frame
  coalescing, exact wrapped long-row styles, DPI cancellation, late-page result
  rejection and a 60-page flat-row differential with seed `0xA57A5F` (10844767).
  Cache entry/byte bounds are checked throughout scrolling.
- `./build/windows/x64/debug/gui-presentation-smoke.exe`: passed on actual
  SDL3/Graphite/Dawn. A cold 70 KiB panel is cancelled before shape publication,
  followed immediately by an unrelated choice dialog whose default action stays
  intact. A second cold panel receives 40 Down events and retains the correct
  bounded page. Cold wrapped choice/review details also cancel normally. Existing
  action keys, pointer choices, indexed panels, custom views and shutdown pass.

Final integrated Windows evidence:

- `./scripts/test.ps1 -Mode all -Gui`: passed. Debug: 557 cases / 1,185,903
  assertions; Release: 557 cases / 1,185,807 assertions. Zero failures/skips.
  Both modes passed CLI/process transport, actual D3D12/Dawn/Graphite readback,
  renderer, Unicode/native surface pixel checks, local/TCP editing, real
  clangd/Git, recovery, workspace and expanded presentation checks. Package
  helpers passed 12 checks; no new distribution package was produced.
- `./scripts/format.ps1 -Check -Offline`: all 141 C++ / 12 JS files passed;
  `git diff --check` passed. Restored Debug using
  `./scripts/xmake.ps1 f -m debug -y`.
- Complete log: `build/text-surface-acceptance-all.log`; focused logs:
  `text-surface-viewport-debug.log` and `text-surface-gui-debug.log`.
- Visually inspected `build/graphics/gui-text-actions.png`: monospace semantic
  row tones, Chinese/emoji glyphs and separate action buttons render correctly.

Logs: `build/text-surface-*`. Source validation/index construction and visible
page copying still run on the UI thread. Short paragraphs, editable fields and
other remaining text components can still shape synchronously. This does not
claim constant frame latency, row-internal incremental glyph shaping, complete
native component extraction, whole-project completion or a new package.

### Background document paragraph layout (2026-10-10)

Cold document rows of at least 4096 UTF-8 bytes now shape through a bounded libuv
queue. Full SkParagraph/Unicode/tab/bidi semantics are retained. Each work item
uses a private font collection/engine, then transfers the paragraph exclusively
to the UI; no worker reuses a published paragraph's fonts. One work item runs at
a time. Default admission is 16 jobs / 64 MiB of accounted input/style storage,
16 MiB per text input, and a 30-second soft deadline. Native calls cannot be
preempted: cancellation/expiry drains and discards the result; expiry returns a
terminal error. Large late-cancelled paragraph destruction is also scheduled on
the worker. Shutdown closes admission and keeps pumping until work drains.

`DocumentViewport` coalesces pending rows, applies completed paragraphs to the
matching publication/style, cancels invalidated work, and retains exact cache
reuse. Failed worker styles remain terminal until changed or invalidated.
Drawing and vertical movement share paragraph preparation, including adjacent
rows outside the painted area. Pointer selection waiting for geometry preserves
its captured document/revision and blocks subsequent same-document edits/reads;
presentation reads can still fetch the required rows. Revision, DPI and font-size
changes reject captured pointer geometry. Idle observation includes layout work.

Focused Windows checks passed: `viewport-layout-tests.exe` compares real worker
paragraphs against fresh Windows font/SkParagraph pixels, carets, hits, UTF-16
conversion and mixed tab/CJK/emoji/combining/bidi styles. It covers bounds,
cancel/drain, queued/running deadlines, retirement, retained font lifetime,
long-row parity, stale publication, failed styles and viewport destruction.
The real SDL/Graphite/Dawn GUI regression observes a cold 70 KiB row, sends
pointer-down/up and immediate text, and checks placement, full paginated undo
and long-line save. Earlier GUI checks passed after correcting its test oracle:
`document.get` returns bounded pages, so comparing the first page against a full
70 KiB string was invalid. The initial compiler check also exposed and corrected
an MSVC JSON/string equality overload issue. Artifacts: `build/layout-worker-*`.

Final integrated Windows evidence:

- `./scripts/test.ps1 -Mode all -Gui`: passed. Debug: 557 cases / 1,185,927
  assertions; Release: 557 cases / 1,185,690 assertions. Zero failures/skips.
  Both modes passed CLI/process transport, actual D3D12/Dawn/Graphite readback,
  renderer, native Unicode and background viewport pixel/geometry tests, local
  and TCP editing, real clangd/Git, recovery, workspace and presentation checks.
  The long-row GUI regression also queues End/Down and immediate input from a
  tab/CJK/emoji source line, comparing its placement with independently shaped
  visual geometry on a 70 KiB target row. Package helpers passed 12 checks;
  no new distribution package was produced.
- `./scripts/format.ps1 -Check -Offline`: all 139 C++ / 12 JS files passed;
  `git diff --check` passed. Configuration restored with
  `./scripts/xmake.ps1 f -m debug -y`.
- Complete integrated log: `build/layout-worker-acceptance-all.log`. Focused
  logs: `layout-worker-viewport-debug.log`, `layout-worker-pointer-debug.log`.
  Initial failure excerpts from tool output and their causes are retained in
  `build/layout-worker-initial-failures.txt`.

This moves full cold document shaping off the input/render thread. It does not
claim incremental glyph shaping within a changed long row, bounded total native
font/paragraph heap, constant frame latency, asynchronous shaping for all other
UI components, whole-project completion or new distribution-package acceptance.

### JS-owned Git comparison and generic text actions (2026-10-10)

Git comparison no longer owns a special C++ prompt, path/staged/preview state,
S/U/D command dispatch or diff-prefix color classification. The JS workflow
captures the service preview, projects semantic row tones and awaits the generic
`ui.showText` component. That component returns a declared action identity or
null; JS performs stage/unstage and the existing guarded discard confirmation.
Dismissal releases JS preview state. JS projects row tones with a streaming
newline iterator and yields through cancellable `astra.sleep(0)` every 512 rows.
Domain-neutral native buttons, shortcut validation, measured row-tone ranges,
layout/hit testing and bounded visible-row
shaping remain in C++. Code panels use monospace fonts and support horizontal
scrolling. Button geometry is shared by painting and pointer input and adapts
to narrow panels. The native read primitive no longer exports Git dialog state.

Working-tree gutter publication is a generic revision-guarded decoration update.
JS chooses matching clean open documents; a changed document revision prevents
publication. Service preview validation and disk conflict checks remain intact.

Windows focused evidence:

- `./scripts/xmake.ps1 build -y astra-tests astra gui-services-smoke
  gui-presentation-smoke`: passed in Debug.
- `./build/windows/x64/debug/astra-tests.exe --no-version
  '--source-file=*presentation_tests.cpp,*workbench_tests.cpp'`: 97 cases /
  923,094 assertions passed. Coverage includes atomic validation, immutable tone
  snapshots, flat-reference row queries (seed `0xA57A71`), action bounds and narrow
  geometry, JS diff classification/captured targets, dismissal, stage dispatch
  and confirmation cancellation. These counts precede the additional
  unadvertised-action regression in the integrated run.

The first integrated invocation passed all 553 Debug cases / 1,185,605 assertions
but failed `gui-services-smoke` with a timeout after activating a Git file row.
The row's command now awaits modal dismissal, while the old native idle check
treated its owning input task as unfinished background work. Generic dialogs now
retain their task owner; idle observation recognizes precisely those modal/fence
owners while preserving ordering and waiting for other scopes. Owner/scoped
idle checks use bounded stack storage. Focused real Git GUI and text-panel GUI
checks passed after this fix. Failure evidence is retained under
`build/text-panel-acceptance-first-failed.log`.

Integrated Windows evidence:

- `./scripts/test.ps1 -Mode all -Gui`: passed with the modal-owner fix and
  mixed-newline/yield/staged-binary regression. All 554 cases passed in each mode:
  Debug 1,185,876 assertions; Release 1,185,656. Zero failures/skips. Both modes'
  CLI, actual D3D12/Graphite/Dawn readback, renderer, Unicode/viewport layout,
  local and TCP editing, real clangd/Git, recovery, workspace and presentation
  checks passed. Package helpers passed 12 checks; no new distribution package
  was produced.
- Subsequent JS-only session cleanup removes unconditional prompt dismissal from
  Git operations. Actions close only their own cancellable comparison handles;
  pending metadata reads share that signal. Selected actions release the modal
  session before starting the service operation. A later unrelated dialog stays
  open when a delayed stage completes.
- Final `astra-tests.exe --no-version
  '--source-file=*workbench_tests.cpp,*presentation_tests.cpp'` passed in both
  Debug and Release: 101 cases / 923,177 assertions each. Three additional
  ownership regressions cover delayed stage completion preserving another
  modal, direct action abort/drain of the current comparison, and cancellation
  of a pending gutter metadata read. The other 456 cases were intentionally
  filtered in these final affected-source runs; the preceding integrated counts
  above are not inflated to claim a new full-suite run.
- Final `gui-services-smoke.exe` passed in both modes after session/read cleanup:
  real clangd navigation/editing and Git diff, stage/unstage, commit and guarded
  discard. Generic `gui-presentation-smoke.exe` also passed in both modes after
  session cleanup: shortcuts, pointer selection, cancellation, existing
  picker/choice/review behavior and shutdown are exercised.
- `./scripts/format.ps1 -Check -Offline`: all 137 C++ / 12 JS files passed;
  `git diff --check` passed. The final expanded JS guard formatting preserves
  behavior and both affected-source suites passed again. Configuration restored
  to Debug.
- `build/graphics/gui-git-diff.png` and `gui-text-actions.png` were visually
  inspected: monospace diff text, added/removed/hunk tones, Chinese/emoji and
  separate action buttons render correctly. Shape caches remain bounded.

Logs: `build/text-panel-*`. This change does not establish whole-project
completion; remaining native component extraction, incremental long-line
display and final packaging remain open.

The additional real-SDL shortcut regression initially failed with
`Text shortcut lost the action identity`: the shortcut closed the modal, then
its queued `SDL_EVENT_TEXT_INPUT` reached the editor and changed document state.
Artifact: `build/text-panel-shortcut-before-fix-debug.log`, with its preserved
presentation fixture. Read-only modal shortcuts now retain their keystroke
through release, consuming associated text/composition packets and auto-repeat
even after the panel closes. Independent keydown, pointer input and a new dialog
start a fresh text sequence; focus loss clears held shortcut state. This covers
generic text actions and choice shortcuts. The presentation regression sends
keydown/text, then repeat/text/release in another pump and verifies that document
revision/selections remain unchanged.

Final Windows acceptance of the complete change:

- `./scripts/test.ps1 -Mode all -Gui`: passed. Debug 557 cases / 1,185,790
  assertions; Release 557 cases / 1,185,636 assertions. Zero failures/skips.
- Both modes passed CLI, D3D12/Graphite/Dawn readback, renderer, Unicode text and
  viewport layout, local/TCP editing, real clangd/Git, recovery, workspace and
  presentation checks, including shortcut text/auto-repeat isolation.
- Package helpers passed 12 checks; this is not a distribution package.
- `./scripts/format.ps1 -Check -Offline`: 137 C++ / 12 JS files passed;
  `git diff --check` passed. `./scripts/xmake.ps1 f -m debug -y` restored Debug.
- Earlier passing invocations remain in
  `build/text-panel-acceptance-before-shortcut-fix.log` and
  `text-panel-acceptance-before-repeat-fix.log`; the final complete invocation
  is `build/text-panel-acceptance-all.log`.

### Persistent document viewport and layout reuse (2026-10-10)

`EditorWindow` now delegates published line storage and paragraph lifetime/reuse
to `DocumentViewport`. Complete contiguous pages of at most 256 rows use measured
persistent SumTrees. Publication validates UTF-8/extents/ordering before mutation
and retains equal runs during local changes and overlapping scroll pages.
Partially assembled long lines remain in the bounded-page transport builder and
are only published after their full logical content arrives.

Current input/hit handles are separated from an engine-bound LRU paragraph cache.
Exact text and every `TextLayoutOptions` field certify reuse; hashes only select
candidates. Relative UTF-8 color spans allow identical text/style to move to a
different row/byte origin. Syntax/DPI changes invalidate current handles and
resolve against the full new style instead of unconditionally rebuilding every
paragraph. Known-old syntax revisions remain excluded. Failed publication or
shaping preserves the current content/layout.

The default per-document admission limits are 256 entries and 8 MiB of accounted
text/style storage. Entry and byte limits independently evict least recently used
entries. Oversize paragraphs still serve their current row without entering the
cache. Current/external paragraph handles remain valid after eviction; accounting
is not total SkParagraph/font/GPU heap usage. Page validation, text-key hashing,
range-list publication, new paragraph shaping and long-line reshaping remain
explicit costs. New shaping still runs on the UI thread; constant frame latency
and incremental glyph shaping within a long line are not claimed.

Windows focused Debug evidence:

- `./build/windows/x64/debug/astra-tests.exe --no-version
  '--source-file=*viewport_index_tests.cpp'`: 3 cases / 16,251 assertions passed.
  Deterministic flat-reference edits/scrolling (seed `0xA57AD15`), summary identity
  and associativity, immutable retained pages, actual overlap/prefix/suffix
  sharing, empty/CRLF/Unicode rows, invalid input and page limits are exercised.
- `./build/windows/x64/debug/viewport-layout-tests.exe`: passed with actual
  Windows fonts/SkParagraph. Raster pixels, geometry, Unicode caret/hit and UTF-16
  conversion are compared with independently fresh paragraphs. All ten style
  fields are varied separately; 80 random publications (seed `0xA57AF07`) cover
  bidi, combining text, tabs, wrapping and DPI. LRU entry/byte admission,
  failed publication/style, oversize bypass and external lifetime also passed.
- Local and `--tcp` `gui-smoke.exe` passed the actual SDL/Graphite/Dawn viewport
  regression: cumulative layout constructions were 60 initially, 63 after a
  three-row scroll, 65 after inserting one line, and 65 after undo. Unchanged
  shifted code rows reuse exact-style paragraphs; document content/undo and the
  existing 70 KiB Unicode/tab long-line edit/save remained correct. These counts
  are engine layout calls for this fixture, not elapsed-time/GPU-frame metrics.
  `build/graphics/viewport-cache-local.png` and `viewport-cache-tcp.png` were
  visually inspected after the edit. They include an existing workspace-result
  notice; notification presentation is not changed by this increment.

Integrated Windows evidence:

- `./scripts/test.ps1 -Mode all -Gui`: Debug and Release production/acceptance
  targets built. The Debug suite passed all 550 cases / 1,101,309 assertions;
  Release passed all 550 cases / 1,101,127 assertions. Both had zero failures/skips;
  async polling changes assertion counts between configurations.
- `./scripts/format.ps1 -Check -Offline`: all 135 C++ / 12 JS files passed;
  `git diff --check` passed.
- Both modes' CLI, actual D3D12/Graphite/Dawn pixel readback, native renderer,
  Unicode text layout, viewport pixel/layout differential, local editing,
  clangd/Git services, recovery, workspace and presentation checks passed.
  TCP editing also passed in both modes, including the same 60/63/65/65 cache
  regression counts. Packaging helpers passed 12 checks. This is not a new
  distribution package or whole-project completion.

The first native-test build failed on MSVC C4244 (integer ternary assigned to a
float); explicit float constants fixed it. Artifact: `build/viewport-build-tests.log`.
Passing builds/runs and final integrated evidence are under `build/viewport-*`.
Further native component extraction, incremental long-line display work and final
new-revision acceptance/packaging remain open.

### Incremental Markdown semantic indexes (2026-10-10)

Markdown's two-stage syntax analysis now uses immutable measured semantic units
for highlights, Outline and textobjects. Each inline paragraph/table-cell group
retains its continuation exclusions as one grammar context. Changed groups parse
fresh for canonical recovery; unchanged groups reuse derived relative indexes.
The cached path no longer copies, edits or reparses every retained inline AST.
The separate full-traversal option remains the differential oracle.

The Markdown block scanner can recreate otherwise unchanged node identities.
Units of at most 1024 bytes therefore also match exact length-tagged UTF-8 text
and complete relative block shape, including field names, symbols, lexer columns
and error flags. Shape checks have their own node limit/counter, cancellation and
deadline checks. Large inline groups remain indivisible. Whole update cost still
includes shape validation, block parsing, cache publication and direct-child
enumeration; a logarithmic whole-parser update is not claimed. Lazy textobject
materialization preserves the API's block-then-inline ordering.

Initial new differential tests failed because scanner-created identities prevented
reuse and partition collection changed textobject ordering. After both fixes,
the 400-section local-edit case reused 400 units / 38,497 bytes, analyzed 44
semantic nodes and invoked the inline parser once. Exact block-context validation
still visited 9,240 nodes. The independent complete analysis visited 18,801 nodes
and invoked the inline parser 800 times. Shared highlight prefix/suffix each cover
more than one-third of the retained source. These are deterministic operation
counts for this fixture, not elapsed-time or frame-latency measurements.

New tests also cover large quoted paragraphs, independent table/paragraph
delimiters, paragraph splitting/merging, 64 random Unicode edits (seed `0xA57A4D`),
retained snapshots, cache limits/cancellation, parallel workers and transitions
between cached/full modes. A real service test edits Markdown, then reads bounded
Outline/highlight pages and selects the updated inline textobject.

Windows Debug evidence:

- `./scripts/format.ps1 -Check -Offline`: 129 C++ / 12 JS files passed;
  `git diff --check` passed.
- `./scripts/xmake.ps1 build -y astra-tests astra astra-server gui-smoke
  gui-services-smoke gui-workspace-smoke gui-presentation-smoke
  gui-recovery-smoke`: passed.
- `./build/windows/x64/debug/astra-tests.exe --no-version
  '--test-case=*Markdown*,application syntax*'`: 12 cases / 1,740 assertions
  passed. `./build/windows/x64/debug/astra-tests.exe --no-version`: all 547
  cases / 1,084,919 assertions passed, zero failures/skips.
- Actual `gui-smoke.exe` (local and `--tcp`), `gui-workspace-smoke.exe`,
  `gui-presentation-smoke.exe`, `gui-services-smoke.exe` and
  `gui-recovery-smoke.exe` all passed with SDL3/Skia Graphite/Dawn. The runs
  exercised real editing/syntax/Outline, script-dispatched views, actual clangd/Git,
  workspace replacement/conflict handling and durable recovery/exit.

Windows Release evidence:

- `./scripts/test.ps1 -Mode release -Gui` built the production service/GUI and
  all acceptance targets with the pinned xmake/toolchain. The complete suite
  passed all 547 cases / 1,084,845 assertions, zero failures/skips. Async polling
  accounts for variable assertion totals between Debug and Release.
- CLI, D3D12/Graphite/Dawn pixels, renderer lifecycle and Unicode text layout
  passed. Local/TCP `gui-smoke`, services, recovery, workspace and presentation
  GUI targets all passed; packaging helpers passed 12 checks. These checks do
  not produce a new distributable package.

Failure artifacts remain in `build/markdown-semantic-affected.log` and
`build/markdown-semantic-corrected.log`; corrected passing focused runs are in
`build/markdown-semantic-context.log` and `build/markdown-semantic-service.log`.
Incremental display layout, native component extraction and final new-revision
acceptance/packaging remain open.

### Bounded native view retirement (2026-10-10)

Published sources, cancelled/superseded builders, old fold projections and
publication restarts now transfer owning edges to a dedicated memory-only worker.
The worker does not call SDL, JavaScript, services or I/O; libuv continues to own
external I/O. Its 48 preallocated lifetime slots are reserved before construction,
and 256 MiB of serialized-page accounting bounds admission. Shared projections
are charged conservatively while retained, so this accounting is not total heap
usage. Slot/byte exhaustion preserves current builders and published content.
Malformed-page rejection rolls back its temporary charge. Round-robin retirement
prevents a repeatedly reused low slot from starving another queued source.

Ownership handoff does not allocate queue entries or destroy a large graph on the
window thread. MSVC's `std::set` move allocates a sentinel, so fold-state ownership
now moves by pointer; compile-time checks require noexcept payload construction
and assignment. The worker drops payloads outside its mutex. Normal UI idle fences
include retirement, and native close retires published sources, refuses new
builders and waits for both script/transport and retirement to drain. RAII queue
destruction joins the worker. External C++ snapshots remain immutable and keep
their own edges alive; releasing those external edges later is outside GUI-owned
retirement. Source construction, fold preparation and general display/parser
work are not claimed to have constant latency by this change.

Windows Debug evidence:

- `./scripts/format.ps1 -Check -Offline`: 129 C++ / 12 JS files passed;
  `git diff --check` passed.
- `./scripts/xmake.ps1 build -y astra-tests astra-view-benchmark astra astra-server
  gui-presentation-smoke gui-smoke gui-recovery-smoke gui-workspace-smoke
  gui-services-smoke`: passed.
- `./build/windows/x64/debug/astra-tests.exe --no-version`: 541 cases /
  1,083,413 assertions, zero failures/skips. The earlier affected
  `--test-case=presentation*,workbench*` run passed 94 cases / 838,710 assertions.
  New cases observe actual destruction on another thread, stall that destruction
  to prove capacity/byte backpressure, exercise reuse and shutdown joining, retain
  an immutable external snapshot, and prove admission/malformed-page failures
  cannot overwrite a current builder or published source.
- `gui-presentation-smoke.exe`: the actual GPU window published 20,002 streamed
  rows, navigated the final row, replaced that source with 602 rows and verified
  that the idle fence awaited real retirement. It then cancelled an unpublished
  15,000-row builder while Home input navigated the old view. Versions/content
  remained intact and no retirement remained after idle. Existing streamed
  publication, focus/fold, native input and renderer shutdown regressions passed.
- Local/TCP `gui-smoke.exe`, `gui-workspace-smoke.exe`, `gui-services-smoke.exe`
  and `gui-recovery-smoke.exe` passed with the real graphics backend; actual
  clangd/Git, retained search/replacement/conflict flows and durable exit were
  exercised. `./tests/cli_smoke.ps1 -Mode debug` also passed.
- `./build/windows/x64/debug/astra-view-benchmark.exe`: seven samples at each
  size, actual source destruction versus owner handoff, with complete queue/source
  shutdown checked each time. Nearest-rank P95 results in microseconds:

  | Rows | Inline source free | Abort owner handoff | Publish owner handoff |
  | --- | ---: | ---: | ---: |
  | 1,000 | 924.7 | 11.9 | 4.9 |
  | 10,000 | 11,740.7 | 15.1 | 7.2 |
  | 100,000 | 126,634 | 15.8 | 7.4 |

  These are measurements on this Windows host, not portable guarantees or full
  input-to-GPU-frame timings. Real destruction still runs to completion on the
  worker. `scripts/test.ps1 -Benchmarks` now includes this target in Release.

Windows Release evidence for this source revision:

- `./scripts/xmake.ps1 f -m release -y` and `./scripts/xmake.ps1 build -y
  astra-tests astra astra-server astra-view-benchmark gui-presentation-smoke
  gui-smoke gui-recovery-smoke gui-workspace-smoke gui-services-smoke
  graphics-smoke renderer-smoke text-layout-tests`: passed.
- `./build/windows/x64/release/astra-tests.exe --no-version`: 541 cases /
  1,083,451 assertions, zero failures/skips.
- `graphics-smoke.exe`, `renderer-smoke.exe`, `text-layout-tests.exe`:
  real D3D12/Graphite/Dawn readback/presentation, SDL lifetime and Unicode
  shaping/hit testing passed.
- All five GUI workflow targets and `gui-smoke.exe --tcp` passed. The same
  20,002-row publication and 15,000-row cancellation/input/retirement checks ran
  in Release, along with actual clangd/Git, workspace conflict/replacement,
  Unicode editing, save/dirty close and recovery.
- `./tests/cli_smoke.ps1 -Mode release`: passed.
- `./tests/package_scripts_tests.ps1`: 12 Windows helper invariants passed. This
  is not a newly built or isolated-smoked final application package.
- `./build/windows/x64/release/astra-view-benchmark.exe`: seven samples per size
  with complete shutdown. Nearest-rank P95 results in microseconds:

  | Rows | Inline source free | Abort owner handoff | Publish owner handoff |
  | --- | ---: | ---: | ---: |
  | 1,000 | 392.3 | 9.6 | 2.6 |
  | 10,000 | 6,938.8 | 8.2 | 3.4 |
  | 100,000 | 72,617.6 | 9.2 | 4.5 |

Initial builds rejected throwing payload moves (the MSVC set sentinel) and three
discarded nodiscard results in negative tests. These were corrected without
weakening the ownership checks. Initial/final build and test evidence is preserved
under `build/view-retirement-*.log`. Broader incremental display/syntax, contextual
search output, native window extraction and current final package/product
acceptance remain open.

### Backpressured asynchronous view providers (2026-10-10)

View publication now consumes arrays, synchronous iterables and asynchronous
iterables, including a direct iterable provider result. The wrapper retains one
bounded JSON page plus one candidate row and awaits native acknowledgement before
pulling further rows. Each yielded row is snapshotted, so reusing/mutating a source
object cannot change already prepared rows. JS stops infinite sources at 100,000
rows or 32 MiB and retains the 128-row / 60,000-byte page limits. Native validation
and the measured source/projection remain the publication boundary.

Optional async `complete(options)` supplies final subtitle/empty/UI metadata only
after the iterator finishes. Iterator exceptions, malformed hierarchy, oversized
rows, metadata failures and supersession abort the unpublished builder and retain
the old interactive snapshot. Signal-aware stalled generators and completion
callbacks drain on cancellation. External provider awaits must honor the signal;
an arbitrary unrelated Promise is not forcibly interrupted.

Default Explorer and Source Control refreshes transform `operationPages` directly
into rows. Workspace Search similarly transforms each completed result page,
validates stable global file/match counts and pagination, and retains at most 200
distinct labels for confirmation. A successful `workbench.search` now returns a
bounded summary rather than a complete JS match array; full matches remain
available through its retained operation identity. Service search results add
the same global `fileCount` on every page, including dirty and untitled buffers.
Missing/changed pagination, malformed collections, nonadvancing pages and
inconsistent final file totals preserve the old view, acknowledge operation
cancellation, report an error and cannot offer replacement confirmation.

Windows Debug evidence:

- `./scripts/format.ps1 -Check -Offline`: 126 C++ / 12 JS files passed;
  `git diff --check` passed.
- `./scripts/xmake.ps1 build -y astra-tests astra astra-server gui-smoke
  gui-presentation-smoke gui-workspace-smoke gui-services-smoke gui-recovery-smoke`:
  final source/staged runtime build passed.
- `./build/windows/x64/debug/astra-tests.exe --no-version`: 538 cases /
  1,083,348 assertions, zero failures/skips. The focused final
  `--test-case=workbench*,application search overlays*,application tree pagination*`
  run passed 69 cases / 4,584 assertions. This covers backpressure while a page
  acknowledgement is withheld, reused row objects, iterator cleanup, old snapshot
  retention, cancellation during row/metadata awaits, bounded infinite sources,
  530 search rows across three pages with a 200-label sample, malformed pages and
  real disk/dirty/untitled file counts. Capacity-only adapters isolate the JS
  failure path; they do not establish native acceptance of oversized row labels.
- `gui-presentation-smoke.exe`: the real 602-row custom view now uses an async
  generator. While it waits after row 256, Down still navigates the old published
  view without changing its version. Releasing the producer atomically publishes
  the replacement and preserves that cursor. Existing 1,505-file Explorer,
  scrolling/folding, focus, stale guards and shutdown checks also passed. The
  real `build/graphics/gui-custom-view.png` capture was visually inspected.
- `gui-smoke.exe` and `gui-smoke.exe --tcp`: actual SDL editing through JS policy
  and the shared dispatcher, Unicode, multicursor undo, document/workspace search,
  replacement and save/dirty close passed.
- `gui-workspace-smoke.exe`: streamed dirty/untitled navigation, retained input
  options, replacement confirmation, atomic undo, external conflicts and guarded
  concurrent reload passed.
- `gui-services-smoke.exe`: streamed Source Control activation, real clangd
  completion/navigation/hover/references/rename/formatting and real Git diff,
  stage/unstage/commit/discard passed.
- `gui-recovery-smoke.exe`: real Unicode recovery, durable exit and failure
  retention/retry/confirmation passed.
- `./tests/cli_smoke.ps1 -Mode debug`: production server/TCP CLI and Unicode
  asynchronous find/replace/undo/save passed.

The initial focused run incorrectly checked `Workbench::awaiting` for an
intrinsic sleep, whose native wait uses a timer rather than a transport future.
That fixture now waits for an observable UI marker issued after starting the
signal-aware timer; row and completion cancellation both pass. The original
failure is retained in `build/stream-views-focused-initial.log`. Final build/unit/
affected/presentation logs use the `build/stream-views-` prefix.

This completes the JS provider streaming path, not end-to-end progressive search:
C++ still retains completed service results and native replacement plans, and Git
mutation commands still receive bounded status results. Large native source/index
reclamation, latency measurements, contextual search display, incremental display/
syntax work and native window extraction remain open. Current Release/package
acceptance and overall architecture/UI completion are not established.

### Indexed keyboard focus for JS views (2026-10-10)

Sidebar rows now have a retained cursor separate from the provider's active-file
marker. Stable row identities survive reordered refreshes; folding moves hidden
cursors to visible ancestors and deleting a row falls back to its old visible
index. The measured projection seeks source ordinals without flattening. Immutable
row parent links span page boundaries, with traversal bounded by the 64-level
hierarchy limit. Randomized fold checks compare visible identities, parents and
hidden-row ancestor lookup against a flat oracle while retaining old snapshots.

`astra.ui.views` exposes asynchronous read, focus, guarded focus return, select and
atomic navigation. C++ owns widget geometry, cursor/fold mechanics and focus;
overridable JS commands own dispatch and provider activation. A separate bounded
per-view platform queue preserves arbitrary JS awaits and captured view/version/
focus guards. Late navigation cannot change a view after focus moves. A handler's
idle fence excludes itself and successors that cannot start until it returns;
other scopes still have to settle. Shutdown cancels script work and clears both
platform queues. The queue retains the existing 256-input / 8 MiB bounds.

Arrows, Home/End, PageUp/PageDown, Enter and Space operate the focused widget;
F6 reveals/focuses the sidebar, while Tab/Escape return editor focus. Explorer's
shortcut focuses its rows. Text/IME and editor/clipboard shortcuts cannot mutate
the document while rows own focus. Forms, editor clicks and modal input transfer
focus. Default JS file/search activation returns editor focus using a captured
guard, preserving a newer focus choice. Focused rows have an accent marker.

Windows Debug evidence:

- `./scripts/format.ps1 -Check -Offline`: 126 C++ / 12 JS files passed.
- `./scripts/xmake.ps1 build -y astra-tests astra astra-server gui-smoke
  gui-presentation-smoke gui-recovery-smoke gui-services-smoke gui-workspace-smoke`:
  passed. Final GUI fixture edits were rebuilt with their individual targets.
- `./build/windows/x64/debug/astra-tests.exe --no-version`: 531 cases /
  1,079,927 assertions, zero failures/skips. After final native focus guards and
  built-in JS focus-return changes, affected source-loading tests were rerun with
  `--test-case=workbench*,presentation cursor*,presentation*view*`: 65 cases /
  611,599 assertions passed. The earlier focused navigation run passed 8 cases /
  610,605 assertions.
- `gui-presentation-smoke.exe`: real 602-row custom view and 1505-file workspace;
  ten queued Down keys through a delayed JS override, handler-owned idle fence
  followed by another key, actual viewport paging, parent/child/fold navigation,
  refresh retention, delayed navigation after focus change, stale focus return,
  F6/Tab/Escape, hidden-sidebar reveal and text/shortcut document isolation passed.
  Explorer End/Enter used the provider and returned editor focus.
- `gui-smoke.exe` and `gui-smoke.exe --tcp`: production SDL input, JS policy and
  shared dispatcher editing/search/replace/save/close regressions passed.
- `gui-recovery-smoke.exe`: Unicode recovery, durable exit and failure retention
  passed with the real Windows graphics backend.
- `gui-workspace-smoke.exe`: pointer and F6/Home/Down/Enter search-result activation
  preserved dirty/untitled identities and selections; retained query/replacement
  fields, confirmation, atomic undo, external conflict and second-client stale
  reload rejection passed.
- `gui-services-smoke.exe`: Source Control F6/Home/Enter activated its indexed
  `main.cpp` row through JS, transferred focus into the real Git comparison, and
  stage/unstage/commit/discard passed. Real clangd completion/navigation/hover/
  references/rename/formatting also passed.
- `./tests/cli_smoke.ps1 -Mode debug`: production server/TCP CLI passed.
- `git diff --check`: passed. The real custom-view capture was visually inspected.

Failures are preserved in `build/view-navigation-*.log`: initial compilation found
a duplicate test event variable and discarded nodiscard return values; these were
fixed. The first presentation run assumed a fixed viewport size and was corrected
to derive the page from actual geometry. The broader workspace fixture still
assumed the old sequential modal search/replace input and subsequently tried to
move the body cursor while the retained search field had focus. It now explicitly
fills/verifies both real fields and transfers editor focus before moving/verifying
the document cursor; replacement and concurrent-edit invariants remain asserted.
Final logs use `presentation-final`, `workspace`, `services`, `gui`, `tcp`,
`recovery`, `units`, `affected` and `cli` suffixes. This increment does not establish
current Release/package acceptance or completion of the architecture/workbench
revision. Provider streaming, large-snapshot reclamation and incremental display
work remain open.

### JS-overridable ordered platform editing (2026-10-10)

All default editing commands now have client JS handlers, including text input,
movement, word selection and deletion. SDL text entry, editing shortcuts and
double-click word selection dispatch those handlers. The captured document id
survives arbitrary JS awaits; one bounded platform handler runs per document while
other documents remain eligible. At most 256 accepted inputs and 8 MiB of
serialized parameters/identities may wait. Invalid/capacity failures leave the
queue unchanged.

Normal `ui.read` and native edit primitives fence prior accepted platform input,
including its JS prelude and completion work. A handler reading/editing its own
captured document excludes itself and later input from that fence, preventing a
self-deadlock. Same-batch save, direct native undo and tab/window close therefore
observe earlier input. Default vertical movement measures shaped targets when its
native intent is dispatched and awaits the shared `document.setSelections`
dispatcher; missing/stale shaping and background documents use service movement.
Explicit revision guards retain their failure semantics.

The first real GUI run found that a bare Promise resolved by another command
resumed with the resolver's task identity. A handler's own read then waited on
itself. The fixed QuickJS-NG v0.15.1 source does not emit PromiseHook BEFORE/AFTER
around ordinary reaction jobs; [upstream issue 1740](https://github.com/quickjs-ng/quickjs/issues/1740)
describes the same boundary. The verified local host-context extension captures
one numeric token when reactions are registered and when ordinary jobs are
enqueued, enters it for that job and restores the prior token after execution.
There is no growing Promise/token registry. Retired/cancelled task continuations
reject new host RPC requests, including a cancelled bare-Promise handler resumed
after its result has already been collected.

The dependency recipe asserts the patch hash/cache identity and both extension
symbols. Installation initially failed those checks: xmake's `git apply` skipped
every patch within this repository's ignored dependency cache. The installer now
checks the local patch again and stops Git discovery above the extracted source
before applying it. Runtime preparation includes the patch, recipe and rationale
in provenance and refuses a changed patch before staging/downloads. The native
runtime helper has corresponding source changes only; no macOS/Linux execution
or validation is claimed.

Initial validation also caught an MSVC variable-shadowing warning, a unit adapter
that did not handle successful native action cleanup, and a GUI fixture command
missing its required title. These were corrected before final acceptance. Failure
logs, including the real Promise-context deadlock, remain in `build/input-policy-*.log`.
The staged whitespace check also caught the unified diff's required space markers
on empty context lines. Patch artifacts now disable that whitespace category in
`.gitattributes`, while preserving their pinned LF bytes; C++/JS formatting gates
remain unchanged.

Final Windows Debug evidence:

- `./scripts/format.ps1 -Check -Offline`: 126 C++ and 12 JS files passed.
- `./scripts/xmake.ps1 f -c -m debug -y -vD`: passed with the verified extension;
  `./scripts/xmake.ps1 build -y astra-tests astra astra-server gui-smoke
  gui-presentation-smoke gui-recovery-smoke`: passed, including final resource
  provenance staging after `./scripts/prepare-windows-runtime.ps1 -Offline`.
- Focused platform/default-command/shared-Promise/context/cancellation checks:
  7 cases / 89 assertions passed.
- `./build/windows/x64/debug/astra-tests.exe --no-version`: 529 cases /
  628,724 assertions passed, zero failures or skips.
- `./build/windows/x64/debug/gui-smoke.exe` and the same binary with `--tcp`:
  passed with the real SDL3/Skia Graphite/Dawn renderer. Fixtures suspend client
  input before its native primitive, fence/cancel reads, continue editing another
  tab, read inside the suspended original handler, then verify captured identity,
  input/history order and service override dispatch. They additionally exercise
  same-batch direct native undo, save/copy, JS-overridden Shift+Down shaped column
  and default Up, as well as existing multicursor, search, close and shutdown.
- `./tests/package_scripts_tests.ps1`: 12 checks passed, including rejection of
  a modified dependency patch by the actual isolated preparation entry point.
- The staged Debug provenance patch SHA-256 matches `dependencies.lock.json`.
- `./build/windows/x64/debug/gui-presentation-smoke.exe`: passed with 1,505
  indexed files, custom JS views, bounded publication, native focus/layout and
  shutdown checks.
- `./build/windows/x64/debug/gui-recovery-smoke.exe`: passed with the real GPU,
  durable recovery storage and checkpoint failure retention/retry/confirmation.
- `./tests/cli_smoke.ps1 -Mode debug`: passed with independent server/TCP
  processes, Unicode arguments and asynchronous find/regex replace/undo/save.
- `git diff --check`: passed.

This increment does not complete the architecture/UI revision or
establish new Release/package acceptance. Generic row keyboard navigation,
contextual search output, streaming providers, incremental display/syntax work,
large-snapshot latency/reclamation and native window extraction remain open.

### JS-owned mounted workspace search (2026-10-10)

`astra/workspace-search` now owns workspace query/replacement input, options,
debounced readonly previews, paged service requests, result projection/navigation,
replacement confirmation and cancellation. The C++ window no longer stores
workspace search matches, query/options, replacement plans, search generations
or its dedicated search/replacement prompt variants. Provider-specific native
view model branches were replaced with JS-projected, version-guarded generic UI
updates for status, error, branch and notices.

Native `FormRegistry` retains one editor overlay and one form per registered
sidebar with one keyboard/IME focus owner. Form-local input/history and shaping
cache identities remain independent. Sidebar controls reserve space above the
measured result rows; clicking the editor releases focus without discarding the
controls. `forms.setValues` validates every requested input/toggle against one
captured version before atomically publishing a new generation. Document find
and workspace search can stay visible together.

Replacement searches afresh and refuses incomplete/unreadable previews. JS
rechecks the actual native form version after confirmation, including delayed
input events. Accepted writes continue to be observed when input changes or a
form closes; explicit cancellation requests service cancellation and reads the
final partial-success report rather than discarding applied counts. Shutdown
cancels readonly work and waits for accepted writes before recovery/teardown.

The first real GUI run passed replacement but failed during subsequent save/close
with `Operation expired or does not exist`. `build/mounted-gui-diagnostic.log`
records the transition through operation ids 99, 100, 101 and 103. Both service
entry paths had reclaimed the first completed string-map key, evicting newer
decimal identities ahead of older ones. Reclamation now uses an explicit numeric
creation sequence and never selects pending operations. A deterministic 150-step
regression holds one pending request while checking recent completed results and
fast script commands across decimal boundaries.

Initial validation also exposed missing callback arguments in new tests, a
missing UTF-8 adapter include and an unregistered initial view in the unit host;
these were corrected. A filtered run against the stale executable reported zero
cases and was not accepted as evidence. After rebuilding, the focused form,
workspace-policy and operation-retention checks passed: 12 cases / 24,629
assertions. The final suite additionally covers superseded startup failures
without overwriting newer controls or status.

The additional unsaved-buffer GUI fixture initially invoked
`ui.execute('editor.insert_text')`, which is not yet registered in the client
command host. It now uses the existing asynchronous `ui.native` editing adapter;
the service still receives the production command dispatcher request. Completing
client editing-command coverage and routing native text/vertical input through
client overrides remains part of the architecture work, rather than being
claimed complete by this search change.

Final Windows Debug evidence:

- `./scripts/format.ps1 -Check -Offline`: 124 C++ and 12 JS files passed.
- `./scripts/xmake.ps1 build -y astra-tests astra astra-server gui-smoke
  gui-presentation-smoke gui-recovery-smoke`: passed; `gui-smoke` was rebuilt
  after correcting the additional unsaved-buffer fixture.
- `./build/windows/x64/debug/astra-tests.exe --no-version`: 522 cases /
  628,609 assertions passed, zero failures or skips.
- `./build/windows/x64/debug/gui-smoke.exe` and the same binary with `--tcp`:
  passed with the real SDL3/Skia Graphite/Dawn renderer. Both exercise separate
  editor/sidebar forms, shaped pointer refocus, body editing/undo with controls
  retained, atomic immediate input/cancellation, disk replacement only after
  confirmation, and replacement of an unsaved buffer that stays dirty and is
  undone as one edit. Existing document editing, close, save/reopen, palette and
  shutdown acceptance also passed.
- `./build/windows/x64/debug/gui-presentation-smoke.exe`: passed with 1,505
  indexed files, custom JS views, bounded publication, layout and shutdown.
- `./build/windows/x64/debug/gui-recovery-smoke.exe`: passed with the real GPU,
  recovery store, durable exit and failure retention/retry/confirmation.
- `./tests/cli_smoke.ps1 -Mode debug`: passed with independent server/TCP CLI
  processes, Unicode arguments and async find/replace/undo/save.
- `git diff --check`: passed. The staged runtime copy of
  `workspace-search.js` matches the source SHA-256. The real renderer screenshot
  `build/graphics/gui-workspace-search.png` was inspected.

Final logs are in `build/mounted-*.log`; the diagnostic failure log is preserved.
These checks do not establish new Release or package acceptance.

This work does not complete the overall architecture/UI revision. Generic row
keyboard navigation, contextual search result presentation, streaming providers
without retained whole JS result arrays, large-snapshot reclamation/latency,
incremental display/syntax work and final Release/package acceptance remain open.

### JS-defined retained sidebar views (2026-10-10)

The activity bar now reads registered JS view descriptors. Explorer, workspace
search, Git and custom views publish the same generic row schema and use the same
native clipped renderer, measured range traversal and fold operations. Default
labels, icons, hierarchy and row actions are projected in JavaScript; native
pointer activation queues a versioned event back to JS. Search-result navigation
now calls the shared document dispatcher from JS with captured revision guards.
The window no longer owns a FileTreeIndex or separate Git/search row drawing and
click branches.

`ViewBuilder` validates whole pages before appending persistent measured rows.
`ViewStore` fences begin/append/commit/abort by unique owner-local identities;
cancelled, superseded and invalid replacements retain the published snapshot.
The projection takes at most 128 rows of filtering work per pump and preserves
fold changes made while the old view remains visible. JS pagination yields at
128 rows or 60,000 UTF-8 bytes; the native boundary enforces page and total bounds.
Pending native commit futures are cancelled on shutdown and view cancellation.
`docs/SERVICE.md` documents the public async view API and limits.

Windows Debug evidence:

- `./scripts/format.ps1 -Check -Offline`: 122 C++ and 11 JS files passed.
- `./scripts/xmake.ps1 build -y astra astra-server astra-tests gui-smoke
  gui-presentation-smoke gui-recovery-smoke`: passed.
- `./build/windows/x64/debug/astra-tests.exe --no-version`: 512 cases /
  605,719 assertions passed, zero failures or skips on the final descriptor-snapshot
  revision. The full suite and GUI/CLI checks were repeated after that correction.
- Focused view/provider/Git checks: 8 cases / 159,318 assertions passed. Coverage
  includes 800 hierarchical fold rounds against a flat oracle (seed 0xA57BA22),
  immutable retained snapshots, atomic invalid pages across hierarchy boundaries,
  fold changes during publication, stale generations and shutdown cancellation.
  Final provider checks also mutate the caller's descriptor after registration and
  verify that publication still uses its captured identity.
- The unresolved-native-publication cancellation case passed separately:
  1 case / 8 assertions; the cancelled JS workflow and native stage both drained.
- Real GUI presentation passed for 1,505 default file rows and a 602-row custom
  JS view, including group collapse/expand, row callbacks, malformed replacement
  retention and renderer shutdown. `build/graphics/gui-custom-view.png` was
  inspected using the real SDL/Skia Graphite/Dawn output.
- Real local/TCP `gui-smoke`, `gui-recovery-smoke` and
  `./tests/cli_smoke.ps1 -Mode debug` passed after the complete Debug suite.
- Initial builds exposed nodiscard returns in negative tests and a range
  callback mismatch (SumTree visits values; ViewIndex exposes index/value).
  Those failures were corrected and the affected targets rebuilt successfully.

Logs are in `build/views-*.log`. At this revision, workspace search inputs and
replacement confirmation still contained native product policy; the mounted
workspace search work above subsequently moved them to JS/sidebar controls.
Keyboard focus/navigation for generic rows, streaming providers without retained
whole JS service arrays, large-snapshot reclamation/latency measurements and
incremental display/syntax work remain open. This revision is not a completed
milestone and does not establish new Release or package acceptance.

### Code formatting and acceptance gate (2026-10-10)

The current mounted-form work initially failed
`./scripts/format.ps1 -Check -Offline` because its new C++ edits were compressed
and had not been formatted. After `./scripts/format.ps1 -Offline`, the same check
passed for 124 C++ and 11 JS files, and `git diff --check` passed. These checks
cover source formatting only. At that point, the mounted-form work had not yet
been built or accepted; its later functional evidence is recorded above.

Repository-owned C++ and JavaScript now use checked-in `.clang-format` and
`.prettierrc.json`. C++ uses four-space indentation, a 100-column limit and expanded
functions, lambdas, branches and loops. JavaScript uses Prettier's consistent
spacing and wrapping. This is a formatting-only increment; architecture and UI
work remain in progress.

`scripts/format.ps1` verifies pinned clang-format 21.1.8 and Prettier 3.6.2 archives
against `tools/formatters.json`, prepares them in `.tools/format/`, and uses the
repository-pinned Node runtime. It covers `include/`, `src/`, `tests/` and `data/`,
excluding dependencies and build output. `scripts/test.ps1` runs `-Check` before
building, and `AGENTS.md` requires formatting before each commit.

Windows Debug evidence:

- `./scripts/format.ps1`: passed after correcting the initial wrapper's scalar
  `-i` splatting to an explicit array. That initial invocation reported
  `i: no such file or directory` and did not write C++ files in place.
- `./scripts/format.ps1 -Check -Offline`: passed for 120 C++ and 11 JS files;
  118 C++ files are tracked in this formatting increment and two belong to
  unfinished view work. A deliberately compressed temporary C++ file was rejected;
  after removing only that fixture, the check passed again.
- `./scripts/xmake.ps1 build -y astra-tests astra astra-server gui-smoke
  gui-presentation-smoke gui-recovery-smoke`: passed.
- `./build/windows/x64/debug/astra-tests.exe --no-version`: 506 cases /
  446,886 assertions passed, zero failures or skips.
- `./build/windows/x64/debug/gui-smoke.exe`, `gui-presentation-smoke.exe` and
  `gui-recovery-smoke.exe`: passed with the real Windows graphics backend.
- `./build/windows/x64/debug/gui-smoke.exe --tcp` and
  `./tests/cli_smoke.ps1 -Mode debug`: passed, including real remote SDL input
  and independent server/client processes.

Logs are in `build/format-*.log`. This increment does not establish new Release
or package acceptance, or completion of the remaining architecture revision.

### Nonmodal document find and replace (2026-10-10)

The document search workflow now uses a retained inline form in the editor's
upper-right corner. C++ provides a generic bounded component with independent
Unicode input/history, shaped mouse selection, clipboard, focus traversal,
IME preedit/candidate positioning and DPI-aware geometry. JS supplies the
fields, options and actions, debounces nonselecting previews, serializes explicit
navigation, handles service errors in the component and cancels obsolete work.
The visible component does not hold a modal input task: clicking the editor
allows ordinary editing while a preview is pending.

Form identities have native opening epochs and input generations. Status updates
require both to match; closing/reopening the same extension id cannot accept a
previous session's result. Events carry captured values and are deferred to the
owner loop to avoid reentering the JS runtime during a native request. Their queue
is bounded and consecutive input changes are coalesced. Input values reject
oversized, multiline, NUL and malformed UTF-8 content before mutation.

`document.find` now supports backward navigation through the same local/TCP
dispatcher. It starts strictly before the primary selection beginning and wraps
to the last match. The worker retains one match, preserves original regex
anchors/Unicode word boundaries and checks cancellation/work bounds. It does not
download the full document to the GUI or add an undo entry for selection changes.
See `docs/SERVICE.md` for the asynchronous form API and direction semantics.

This increment does **not** complete the workbench revision. General retained
extension views, sidebar search controls, remaining native workspace-search
policy, incremental display mapping and complete current Release/package
acceptance remain outstanding.

Intermediate failures and corrections:

- Warnings-as-errors rejected discarded `nodiscard` results in exception tests
  and three local input-option names that shadowed the window options. A doctest
  string/JSON inequality also failed MSVC overload resolution. The tests now
  explicitly discard the throwing call results, compare typed strings, and use
  distinct local names; compiler checks were preserved.
- The first real GUI run failed with `Nonmodal query did not start an asynchronous
  preview`. Its fixture typed the same query already remembered from the preceding
  programmatic search, so there was no value change to debounce. The fixture now
  types a different query before withholding the preview; its editing/cancellation
  assertions remain intact.
- Review found that repeated explicit navigation aborted the previous action,
  Ctrl+Tab was intercepted by widget focus traversal, and IME preedit could appear
  in both widget and document. Navigation now cancels previews and sequences actions;
  plain Tab traverses the component, while Ctrl+Tab retains the workbench binding.
  Only the focused input paints preedit and owns the native candidate area.
- Same-batch typing followed immediately by Esc could close the component before
  JS observed its queued change event. Cancellation now captures the current native
  values before closing, with a regression for reopening the remembered query.

Windows Debug checks (completed serially):

```powershell
./scripts/xmake.ps1 build -y astra-tests gui-smoke gui-presentation-smoke gui-recovery-smoke
./build/windows/x64/debug/astra-tests.exe --no-version
./scripts/xmake.ps1 build -y astra astra-server
./build/windows/x64/debug/gui-smoke.exe
./build/windows/x64/debug/gui-smoke.exe --tcp
./build/windows/x64/debug/gui-presentation-smoke.exe
./build/windows/x64/debug/gui-recovery-smoke.exe
./tests/cli_smoke.ps1 -Mode debug
```

- Final complete doctest suite: **506 cases / 446,785 assertions**, all pass,
  no skips. The preceding full run passed 505 cases / 446,891 assertions;
  asynchronous polling changes successful assertion counts between runs.
  The earlier affected subset passed **82 cases / 228,690 assertions**.
- Local and TCP GUI acceptance pass. The same real SDL workflow exercises
  Unicode/grapheme input and field undo, native IME placement/preedit isolation,
  shaped drag selection/clipboard, keyboard/mouse navigation, option toggles,
  invalid-regex error recovery, one-transaction replacement, and visible pending
  preview plus continued document editing/cancellation. It verifies that search
  does not download full document text to the GUI.
- Presentation and recovery GUI acceptance pass, including indexed Explorer,
  Outline paging, tabs, resize/focus, modal cancellation, unsaved Unicode restore,
  durable exit and checkpoint-error retention/retry.
- Independent CLI/server acceptance passes forward/backward Unicode find and
  no-wrap, capture replacement, selection/history-preserving undo and save.
- Actual `astra` and `astra-server` Debug executables were rebuilt. Captured
  `build/graphics/gui-inline-replace.png` and `gui-inline-replace-tcp.png` were
  visually inspected for readable inputs/options/actions, focus borders and
  placement without blocking the document. Current Release/package acceptance
  is still outstanding; these checks establish Windows Debug evidence only.

### Asynchronous document search and JS policy (2026-10-10)

`document.find` and `document.replaceAll` now use the shared application
dispatcher, immutable Rope snapshots and worker computation. Find retains one
match, seeks from a UTF-8 boundary and wraps without changing full-input regex
anchors or Unicode word boundaries. Optional selection publication checks the
captured selections and version; it prepares metadata/notifications before
mutating selections and avoids copying undo history. Replace prepares capture
expansions and one atomic history transaction on a worker, then rejects changed
revision/content, selections, saved state, closed documents and cancellation
before publication. Input/template/match/output budgets are explicit.

JS owns document query/replacement input, captured document requests, result
presentation and cancellation. The C++ window's recursive full-text download,
literal search/replacement loop and document-specific prompt branches were
removed. At that increment the generic input component was still modal: the planned
nonmodal document find widget, sidebar search controls and remaining native
workspace search/replacement policy are **not complete**. This increment is the
service/policy foundation for those components, not UI redesign acceptance.

Real GUI acceptance now counts the production document-search APIs and rejects
full-text downloads by that flow. It withholds a real search request, sends SDL
editing while the request is pending, cancels and releases the stale reply, and
checks that content, selections and undo history survive. Valid late service
rejections of already-cancelled requests emit `runtime.requestDrained` receipts;
malformed replies, transport and cancellation-RPC failures remain observable.
Receipt handlers share the existing poll budget and drain before idle.

Intermediate failures and corrections:

- The first focused run: **52/53 cases**, one failure. The invalid UTF-8 fixture
  assumed scheduling would succeed; the protocol correctly rejects malformed
  JSON strings before dispatch. The test now checks that rejection and unchanged
  document state; no production validation was weakened.
- `gui-workspace-225628841076600` failed with missing `selections`: native return
  handling mistook a search envelope containing `id`/`revision` for complete
  metadata. It now requires the document metadata fields before acceptance.
- `gui-workspace-225760368872800` exposed the cancelled late-rejection bug above.
  The strengthened real-input test is retained; service rejection receipts are
  separate from the caller's settled cancellation.
- An intermediate complete Debug run: **495/496 cases**, one idle recovery
  checkpoint timeout at its unchanged five-second assertion. That run overlapped
  GUI validation. The isolated recovery check and final serial full-suite run
  subsequently passed without changing its assertion or timeout.
- An intermediate runtime refactor build rejected a member-shadowing local name
  and an unqualified clock alias. Both were corrected under warnings-as-errors.

Final Windows Debug acceptance (all commands completed successfully):

```powershell
./scripts/xmake.ps1 build -y astra-tests gui-smoke gui-presentation-smoke gui-recovery-smoke
./build/windows/x64/debug/astra-tests.exe --no-version
./build/windows/x64/debug/gui-smoke.exe
./build/windows/x64/debug/gui-smoke.exe --tcp
./build/windows/x64/debug/gui-presentation-smoke.exe
./build/windows/x64/debug/gui-recovery-smoke.exe
./scripts/xmake.ps1 build -y astra astra-server
./tests/cli_smoke.ps1 -Mode debug
```

The final serial full suite passed **497 cases / 441,225 assertions**, no skips.
Four real GPU GUI runs passed, including the withheld-search/edit/cancel/late
reply flow over both local and TCP channels. The independent production server
and TCP CLI additionally verify Unicode byte/UTF-16 positions, wrapping, regex
capture replacement, one-transaction undo/selection restoration and actual
save. Built-in JS source/staged search-module SHA-256 hashes match. The 300-sample
scalar-boundary differential oracle uses seed `0xA57FA11`; failure-path cases
cover stale revisions, changed selections/saved baseline, closure, cancellation,
bad expressions/captures, invalid protocol UTF-8, limits and inline scheduling.
All GUI executions occurred after builds completed. Release and current-source
packaging acceptance for the overall architecture/UI revision remain outstanding.

### JS window exit and asynchronous native waits (2026-10-10)

The client `astra/lifecycle` module now owns sequential captured-document close,
the final checkpoint, retry/keep-open/bypass decisions and rechecking documents
that arrive during checkpoint I/O. It composes the currently registered close,
save and checkpoint commands, including user replacements. Cancel propagates
through close choices, save/close requests and checkpoint work; an awaited user
save result cannot dispatch a subsequent close after exit cancellation.

Native C++ owns cancellable wait handles and physical teardown. `ui.waitForIdle`
waits for accepted native input, other script jobs and client transport responses;
`ui.requestClose` refuses open tabs and grants teardown only after the same
quiescence check. Mixed wait kinds settle together, and cancellation identities
cannot cancel another attempt. The JS `ui.withProgress` API starts work only
after its generic native component opens, dismisses it on completion/cancellation
and drains late operation replies. Dedicated native checkpoint/exit policy,
failure prompts and retry/bypass branches have been removed.

Final Windows Debug commands:

```powershell
./scripts/xmake.ps1 build -y astra-tests gui-recovery-smoke gui-presentation-smoke gui-smoke
./build/windows/x64/debug/astra-tests.exe --no-version '--test-case=workbench*,presentation*,QuickJS*,JavaScript*'
./build/windows/x64/debug/gui-recovery-smoke.exe
./build/windows/x64/debug/gui-presentation-smoke.exe
./build/windows/x64/debug/gui-smoke.exe
./build/windows/x64/debug/gui-smoke.exe --tcp
```

All final invocations passed: **75 cases / 219,246 assertions**, 408 unrelated
cases skipped, and four real GUI runs. New policy cases cover component-open
failure, late operation cancellation/drain, captured close ordering, an awaited
user save after cancellation, quiescence, mixed wait kinds and explicit
retry/keep-open/bypass. The real recovery GUI cancels a withheld checkpoint,
accepts subsequent SDL Unicode editing, releases the late reply and keeps that
content open. Another actual client creates and edits a document during a later
checkpoint; the GUI preserves it, requests a new close decision, then dispatches
a fresh checkpoint. Both explicit bypass and the real `ui.retryExit` command are
checked with exact checkpoint-request counts. Presentation acceptance runs a
delayed user script immediately before exit and requires it to finish before
the user-defined checkpoint handler, and tests concurrent idle/close waits.
Actual progress/failure PNGs under `build/graphics/gui-recovery-*.png` were
inspected; the staged lifecycle JS matches its repository source. GUI checks ran
after each build completed, without copying mapped runtime resources.

Intermediate failures were preserved and resolved:

- `gui-recovery-223219871291300`: the old one-pump bypass assertion did not
  account for asynchronous teardown waits. It now waits for actual closure and
  retains checks that bypass is unavailable before failure and sends no extra
  checkpoint after confirmation.
- `gui-recovery-223561582781900`: the concurrent document fixture used initial
  baseline text and therefore was clean. It now performs an actual insert and
  asserts dirty metadata before continuing; the preserved content/close-decision
  assertions pass.
- `gui-recovery-224150531852900`: real Retry retained the previous failure text
  during its new attempt. Starting a checkpoint now clears that stale error;
  fresh checkpoint failures still propagate and open the explicit failure choice.

This increment does not establish full current Debug/Release acceptance or a
current package. Remaining native find/replace policy, general custom views,
editing policy boundaries and incremental presentation/parser requirements
remain part of the active, unfinished architecture revision.

### Earlier JS recovery review and compact confirmation

The client `astra/recovery` module now owns recovery discovery, inactive-session
filtering, confirmation, restoration presentation, manual checkpoints and storage
notifications. Startup discovery and user commands use the same asynchronous
dispatcher. A selected session is captured before confirmation; cancellation
does not restore content, issue-only sessions produce a report, and late list
results cannot overwrite newer availability notifications. Checkpoint commands
await durable service completion and propagate failures. Named confirmations
correlate automation answers with a unique native dialog identity.

C++ retains persistence services, native indexed picker/review components and
presentation state. The previous per-frame recovery JSON scan and dedicated
recovery workflow/dialog branches have been removed. Confirmation height now
follows its item count with a bounded scrolling area; the shared layout remains
the source for paint and pointer hit testing. Recovery rows put the filename and
byte count first, so long absolute paths do not obscure the document name.

Windows Debug validation for this increment:

```powershell
./scripts/xmake.ps1 build -y astra-tests gui-recovery-smoke gui-presentation-smoke gui-smoke
./build/windows/x64/debug/astra-tests.exe --no-version '--test-case=workbench*,presentation*,QuickJS*,JavaScript*'
./build/windows/x64/debug/gui-recovery-smoke.exe
./build/windows/x64/debug/gui-presentation-smoke.exe
./build/windows/x64/debug/gui-smoke.exe
./build/windows/x64/debug/gui-smoke.exe --tcp
```

All passed: **68 cases / 219,093 assertions**, with 408 unrelated cases skipped,
and the four real GUI invocations. New policy cases cover deferred confirmation,
captured session identity, dismissed recovery, damaged sessions, superseded scans,
availability races, disabled/failed initialization, checkpoint failure and storage
notifications. The recovery GUI verifies actual Unicode content and selection,
dirty state, unchanged source files before Save, explicit save, checkpoint failure,
keep-open/retry and durable exit. Presentation checks preserve scaled pointer
cancellation for a 40-item review. The final native recovery confirmation image
was inspected at `build/graphics/gui-recovery-confirm.png`; the staged JS resource
matches the repository source. Build and GUI runs were serialized to avoid
copying mapped runtime resources.

At this earlier increment (`f0c7534`), final-window checkpoint/exit policy still
resided in C++; its JS migration,
remaining native find/replace policy, general custom views, and the other active
architecture requirements remain unfinished. This affected Debug run is not a
new full Debug/Release acceptance or a current distribution-package verification.

Implemented so far: Promise-based script requests, owner-loop polling of real
service operations, paged result collection with byte limits, cancellation,
timeouts, async command operations, event subscriptions and registered ES modules.
Native document edits still validate their base revision at commit time.

Windows Debug commands:

```powershell
./scripts/xmake.ps1 f -m debug -y
./scripts/xmake.ps1 build -y astra-tests
./build/windows/x64/debug/astra-tests.exe --no-version '--test-case=QuickJS*,JavaScript*,application async*,application pending*,application user scripts*,application bundled*,application editing policy*'
```

The focused run passed 23 cases / 526 assertions. New cases use delayed real
futures and the real libuv/document/filesystem service: independent requests,
cancelled late responses, cancelled edits, stale edits across await, and durable
open/edit/save workflows. An earlier focused run failed two error-recovery cases
because unserializable results remained in the task table; they now settle as
serialization errors and both regressions pass. The first build reported an
empty linker failure; an immediate verbose retry succeeded, followed by another
successful rebuild. Neither failure is hidden by the final passing result.

The expanded affected Windows Debug run (`--test-case=QuickJS*,JavaScript*,application*`)
passed **81 cases / 3,875 assertions** after a full rebuild. The first sandboxed
run failed four real clangd/TCP cases (including a denied loopback connection);
all four passed with loopback permission, as did the expanded suite. A later
empty incremental-link failure produced an executable that crashed before test
enumeration (exit `0x80000003`). `./scripts/xmake.ps1 build -r -v -y astra-tests`
rebuilt it successfully; the full log is `build/async-rebuild-debug.log`.
Additional regressions cover scalar/array async results, async event errors,
request cancellation after a synchronous throw, shutdown, and bounded streaming
pages with early-consumer cancellation. These checks establish the async
foundation only; the active workbench revision remains in progress.

The client workbench now loads ES modules for command/menu registration, files,
search and Git. Open/new/save, sidebar/tab changes, file traversal, search paging
and Git status/stage policy execute in the client runtime over the same local/TCP
dispatcher. At this earlier checkpoint, close/recovery/find/replace workflow
migration was unfinished. Providers cancel superseded generations before publishing a model.
User client commands can replace `ui.*` actions; the native GUI smoke exercises
that override through real SDL keys. Client shutdown drains late transport
responses and forwards cancellation before closing the channel.

Windows Debug: `./scripts/xmake.ps1 build -y astra-tests gui-smoke gui-workspace-smoke`
passed. `astra-tests.exe --no-version '--test-case=workbench*,QuickJS*,JavaScript*'`
passed **22 cases / 126 assertions**. `gui-smoke.exe` and `gui-smoke.exe --tcp`
passed real editing, Unicode/IME event, clipboard, multicursor history, search,
confirmed replacement, save/close/reopen and JS policy checks. The renderer
logged `Submit called with no active command buffer` during its final idle frame;
the workflow assertions passed. `/INCREMENTAL:NO` now avoids reusing failed
Windows linker images; the affected target rebuilds completed normally.

The native workbench now uses one shared logical/DPI layout for painting and
pointer hit testing: compact header/activity bar, resizable sidebar, collapsible
Explorer root, folded Outline section, adaptive shaped tabs with overflow and
drag reorder, and responsive metadata status. UI text uses its own configurable
font size; theme roles cover chrome, inputs, selections, status and notices.
Icons are consistent Skia vector paths. File rows use a persistent measured
SumTree with source-order prefix seeking and structurally shared collapse/expand
ranges; rendering visits only the visible viewport. Encoding metadata reports
the persisted UTF-8/BOM/UTF-16/Latin-1 file format instead of a constant label.

Current Windows Debug checks:

```powershell
./scripts/xmake.ps1 build -y gui-presentation-smoke astra-tests gui-smoke gui-workspace-smoke
./build/windows/x64/debug/astra-tests.exe --no-version '--test-case=SumTree*,presentation*,workbench*,QuickJS*,JavaScript*,application file metadata*'
./build/windows/x64/debug/gui-presentation-smoke.exe
./build/windows/x64/debug/gui-smoke.exe --tcp
./build/windows/x64/debug/gui-workspace-smoke.exe
```

All passed: **35 cases / 68,556 assertions** in the focused suite, and all three
real GUI executables. Presentation differential tests retain old snapshots while
performing 300 toggles over 3,280 rows (seed `0xA57A2026`). Layout checks cover
1/1.25/1.5/2 scale factors and narrow/wide logical widths. The native GUI fixture
uses 1,505 real workspace entries, SDL folder/splitter/Outline/tab interactions,
tab overflow/reorder, resize and a shortcut followed by text in the same event
batch. Actual GPU screenshots `build/graphics/gui-workbench.png` and
`gui-workbench-narrow.png` were visually inspected. The resize fixture was fixed
to convert logical dimensions using the actual window scale. This establishes
automated scale/layout checks, not physical monitor DPI transitions or installed
IME candidate interaction. The final idle-frame Skia warning noted above remains.
Intermediate build failures (float narrowing, missing summary equality and an
incorrect test-only file-save argument order) were corrected before these runs.

Clipboard policy now lives in `data/workbench/clipboard.js`; C++ exposes only
SDL clipboard read/write and retained line-boundary metadata. JS reads selections
in revision-checked UTF-8 pages, preserves captured document identity across
await, normalizes pasted line endings and submits one atomic cut/paste transaction.
Scripted clipboard text is bounded to 8 MiB; errors do not silently truncate it.
Windows Debug `build -y astra-tests gui-smoke gui-workspace-smoke` passed;
`astra-tests.exe --no-version '--test-case=workbench*,QuickJS*,JavaScript*'`
passed **25 cases / 162 assertions**. The three clipboard regressions cover
Unicode byte paging, duplicate/multiple ranges, stale reads without write/cut,
delayed clipboard delivery, captured paste identity and CRLF normalization.
Real SDL `gui-smoke.exe`, `gui-smoke.exe --tcp` and `gui-workspace-smoke.exe`
all passed after migration. A string/JSON comparison rejected by MSVC was
corrected to compare explicit strings before the passing build.

Client JS now owns quick-open, save/Save As and guarded reload using genuine
Promise-returning native input/confirmation components. Native modal primitives
validate/bound content, retain focus, cache shaped text, support cancellation,
and return values without issuing document/file service calls. JS submits the
captured document revision after confirmation. The runtime has a separate bounded
task deadline; dialogs default to five minutes, and cancelled/timed-out dialog
requests dismiss/drain their native future. Awaited UI command composition honors
user replacements registered with `astra.ui.registerCommand`.

Windows Debug build: `./scripts/xmake.ps1 build -y astra-tests gui-smoke
gui-workspace-smoke gui-presentation-smoke`. Focused
`--test-case=workbench*,QuickJS*,JavaScript*`: **29 cases / 189 assertions PASS**;
expanded `--test-case=workbench*,QuickJS*,JavaScript*,application*`:
**92 cases / 4,054 assertions PASS** with real clangd/TCP permissions. New tests
exercise delayed input, cancellation/draining, unrelated background requests,
captured reload revision and composed user commands. Real GUI checks additionally
exercise same-batch quick-open and Save As, cancel without save, pending-input
screenshots, stale-confirmation rejection and one-step reload undo.

An initial GUI run timed out while a modal awaited input: automation's stable
UI boundary had incorrectly required every JS command to be finished. The fix
allows only a presented dialog with all remaining tasks awaiting that exact
native method; unrelated transport/timer work still prevents settling. Screenshot
review then found reversed font-size/available-width arguments hiding the dialog
title/accept label. Those arguments were corrected and screenshots regenerated;
the prior interaction-only pass was not counted as appearance acceptance. Final
`gui-presentation-smoke.exe`, `gui-workspace-smoke.exe` and `gui-smoke.exe --tcp`
all passed after the correction. Regenerated input and reload-confirmation GPU
screenshots were visually inspected and show the title and action labels.

Git diff requests, staged-change commit gating/message input and guarded discard
confirmation now execute in client JS. Diff generations cancel older requests;
discard retains the exact service preview ID across await. Saving while Source
Control is visible refreshes its status in JS and preserves the saved-result label.
C++ retains/draws the
returned comparison and gutter model and does not call Git services. The obsolete
native Git workflow branches and migrated reload/open prompt states were removed.
Ready native focus steps advance within a four-millisecond/eight-tick bound before
the next SDL event; service futures are never waited on there.

Native multi-line confirmation descriptions now respect CR/LF/CRLF logical lines.
Both descriptions and diff panels use a persistent measured text-line index;
diff scrolling seeks only its viewport and retains a bounded 256-line shape cache.
Text panels reject content over 8 MiB or 100,000 lines before replacing the old
index. The source-index tests cover Unicode byte extents, mixed line endings,
20,000-row viewport queries, retained snapshots and failed-replacement preservation.

Windows Debug `build -y astra-tests gui-services-smoke gui-workspace-smoke
gui-presentation-smoke` passed. The focused
`--test-case=presentation*,workbench*,QuickJS*,JavaScript*` run passed
**37 cases / 8,464 assertions**. All three GUI executables then passed using actual
Git, clangd, service documents and native SDL/Skia/Dawn. Diff and discard screenshots
were visually inspected. The first Git GUI run failed because a multi-line dialog
description reached the one-logical-line shaper; the indexed multi-line component
fix preceded these passing runs. The final idle-frame renderer warning remains.

Formatting and rename now execute in the client JS language module. Rename captures
document identity/revision before awaiting its name, reviews the returned service
plan with a scrollable native file list, and commits only the captured plan ID.
Formatting rejects obsolete responses before applying edits. The removed native
branches no longer orchestrate these service requests. Confirmation item painting
and pointer actions use one shared DPI-aware layout and a measured text-line index.

Windows Debug `build -y astra-tests gui-services-smoke gui-workspace-smoke
gui-presentation-smoke` passed. The focused
`--test-case=presentation*,workbench*,QuickJS*,JavaScript*` run passed
**40 cases / 8,651 assertions**. All three real GUI executables passed; clangd
format/rename, deferred rename confirmation, and a 40-item review scrolled with
real Down keys and cancelled with a pointer were exercised. The rename and review
GPU screenshots were visually inspected. The final idle-frame Skia warning remains.

Completion, hover, definition/reference navigation and explicit diagnostic review
now execute in client JS as well. Superseded interactive language requests are
cancelled; source revisions, completion caret positions and navigation target
revisions are checked across awaits. C++ retains passive diagnostic updates and
provides generic Promise-based picker/text components rather than language-specific
request/apply branches. Picker results preserve original item identity after
filtering or scrolling. Its measured source/visible trees retain snapshots, cache
the current filter and seek only visible rows during painting. Short lists and
information panels shrink to their content; text panels support horizontal and
vertical scrolling with a bounded shape cache.

Windows Debug `build -y astra-tests gui-services-smoke gui-presentation-smoke
gui-workspace-smoke` passed. Focused
`--test-case=presentation*,workbench*,QuickJS*,JavaScript*`:
**44 cases / 210,060 assertions PASS**. The picker differential test uses 2,000
Unicode labels and 150 filter changes (seed `0xA57A91`) and retains old snapshots.
New delayed-future cases check selected completion payloads, stale caret rejection
and target-revision rejection before navigation activation. All three actual GUI
executables passed after the final content-height adjustment. Native acceptance
includes a 40-item list, source identity after pointer selection, text filtering,
an 80-line text panel and Promise settlement on dismissal. Regenerated completion,
hover, picker and text screenshots were visually inspected. An intermediate build
failed on a duplicated local test variable; it was renamed before the passing
build. The final idle-frame Skia warning remains.

Non-Markdown semantic analysis now retains immutable hierarchical units with
measured byte/outline/object/node summaries. Unchanged units reuse their highlight
SumTrees; only changed units rebuild captures. Outline range reads seek the
measured hierarchy, and enclosing-object queries descend only into a containing
partition. Compatibility full-vector reads materialize lazily and safely across
threads. The service reads its bounded outline result through the new range API.
Cache publication still visits semantic units, and rebuilding a wide parent visits
its direct children; this is not a claim that the complete parser/index update is
logarithmic. Markdown's two-stage semantic indexes still use full traversal.

Windows Debug `build -y astra-tests gui-services-smoke gui-workspace-smoke
gui-presentation-smoke` passed. `--source-file=*syntax_tests.cpp` passed
**26 cases / 7,360 assertions**; `--test-case=application syntax*` passed
**2 cases / 96 assertions**. All three real GUI executables passed after integration.
New tests compare against a separate full-traversal analysis mode: a same-length
edit among 400 C++ declarations reuses over 300 units and over 75% of source bytes,
visits fewer than 10% of the oracle's analyzed nodes, and proves shared prefix and
suffix highlight runs. Forty edits inside one large function (seed `0xA57A5E`)
exercise nested units; 24 random Unicode edits in each of seven grammars
(seed `0xA57A6A`) include malformed input and compare highlights, hierarchy and
textobjects. Retained snapshots and stricter limits on cached results are checked.

Initial new tests exposed two real failures: slot-only candidate matching could
reuse an edited same-size region, and repeated ambiguous Tree-sitter error recovery
could differ from a fresh tree even without the semantic cache. Reuse now requires
an unchanged Rope interval, current origin and child/field stamps; malformed/error
recovery uses a fresh tree and honestly reports `incremental=false`. These failing
runs preceded the passing suites. A warning-as-error for a shadowed local name was
also fixed before rebuilding. The final idle-frame Skia warning remains.

Outline now requests only its visible range and scrolls independently of the
Explorer. `document.syntax` exposes bounded outline paging and an option to omit
outline rows while retaining the total count. A stale page cannot select a symbol.

Windows Debug `build -y astra-tests gui-presentation-smoke gui-services-smoke
gui-workspace-smoke` passed. `--test-case=application syntax*` passed **3 cases /
180 assertions**, including 1,105 declarations, offsets beyond the first 1,000,
empty/hidden pages and invalid parameters. All three native GUI checks passed;
the presentation check was rerun after strengthening its independent-scroll
assertion to compare Explorer viewport offsets. Actual pointer selection after
scrolling selects the intended symbol. `build/graphics/gui-workbench-outline.png`
was visually inspected. An initial build failed on a missing explicit JSON test
header, corrected before these checks. The final idle-frame Skia warning remains.

Native editing bridges now retain bounded completion futures until the actual
service command (including a deferred script operation) settles. JS wrappers
forward parameters, retain captured document identity, preserve RPC error data,
and can cancel an edit before it leaves the serial queue. Explicit revisions are
checked unchanged; ordinary rapid key intents obtain the revision when dispatched.
Committed work is not rolled back by cancellation. The existing service JS policy
still defines document commands; this increment does not claim all remaining
native UI workflows have moved to JS.

Windows Debug `build -y astra-tests gui-smoke gui-presentation-smoke` passed.
`--test-case=workbench*,QuickJS*,JavaScript*` passed **37 cases / 275 assertions**.
Real local and TCP `gui-smoke` runs passed composed await Undo/Redo, stale revision
rejection without mutation, cancelled queued editing, and six history shortcuts
in one SDL batch without stale revisions or reordering. Both variants were rerun
after adding that batch assertion. `gui-presentation-smoke` also passed. The final
idle-frame Skia warning remains; full current Debug/Release and package acceptance
still remain outstanding.

Command-palette workflow now lives in JS and uses the same retained indexed
picker as language choices. It merges client and retained service registrations,
deduplicates identities, uses readable default editing labels, hides internal
parameterized primitives, and awaits the selected client or service command.
Composition resolves the current runtime registration, including direct
`astra.registerCommand` replacements; the old native palette state and rendering
branches were removed.

Windows Debug `build -y astra-tests gui-smoke gui-presentation-smoke` passed.
`--test-case=workbench*,QuickJS*,JavaScript*` passed **40 cases / 317 assertions**.
New cases cover callback self-replacement lifetime, raw command overrides,
catalog deduplication, hidden internal commands, delayed client results, captured
service parameters and cancellation. Local and TCP GUI runs passed real filtering
and selection of both a user client command and a service caret command. The
presentation smoke passed after replacing the old palette implementation; local
and TCP checks were rerun after refining labels. The regenerated native
`build/graphics/gui-palette.png` was visually inspected. The idle-frame Skia
warning remains in these runs.

Renderer teardown no longer redundantly submits already-submitted frames.
The pinned Graphite queue manager owns waiting for outstanding submissions during
context destruction, before Dawn device/surface teardown. Unsubmitted recordings
are discarded with their owning recorder. Windows Debug
`build -y renderer-smoke gui-smoke gui-presentation-smoke` passed. Real
`renderer-smoke` passed resize/minimize/readback, eight successive submitted
frames, destruction with an unsubmitted frame, restart and verified PNG pixels.
Local/TCP `gui-smoke` and `gui-presentation-smoke` all passed afterward **without
the previously recorded empty-submit warning**. Those older warnings remain in
their historical evidence; physical GPU loss still has not been induced.

The Windows validation checkpoint at source **`0469874`** passed the complete
suite: **456 cases / 428,194 assertions in Debug** and **456 cases / 428,318
assertions in Release**. Both configurations passed actual `graphics-smoke`,
`renderer-smoke`, `text-layout-tests`, all five GUI smoke executables, the TCP
`gui-smoke` variant, and `tests/cli_smoke.ps1 -Mode <configuration>`. Async polling
causes assertion totals to vary. The full Release build included all these
targets plus `astra` and `astra-server`, after `./scripts/xmake.ps1 f -m release
-y`; every build finished before Release tests started. The full suite command
was `./build/windows/x64/<configuration>/astra-tests.exe --no-version`.

The first Debug build of the remaining GUI targets failed copying `icudtl.dat`
because a GUI check had been started before resource copying finished. Windows
reported that a user-mapped region prevented the copy. After the GUI checks
exited, the same build completed successfully and graphics/text-layout,
language/workspace/recovery GUI and CLI checks were rerun serially and passed.
No platform outside Windows was tested. This checkpoint does not establish a
new package or completion of the remaining architecture work.

Native highlight, diagnostic and Git-gutter drawing now queries persistent
measured range indexes instead of scanning JSON arrays for each visible line.
Byte, document-line and UTF-16-column units stay explicit. Whole-list publication
still validates/compares all items and may sort unordered input; unchanged edges
share their prior subtrees. This does not claim every update is logarithmic.

Windows Debug `build -y astra-tests gui-services-smoke gui-smoke
gui-presentation-smoke` passed. The affected core/presentation run
`--source-file=*sum_tree_tests.cpp,*presentation_tests.cpp,*document_tests.cpp,*rope_tests.cpp`
passed **57 cases / 401,026 assertions**. It covers 150 random overlapping-range
replacements (seed `0xA57A1D`), retained snapshots, exact shared edges, coordinate
failures and a 100,000-record query visiting fewer than 150 tree nodes, including
a range spanning the entire source, and root retention while a callback publishes
a new tree. All three real GUI executables passed after
integration, including actual clangd/Git rendering and TCP editing.

An initial test build failed on MSVC's unreachable-code warning in a throwing
predicate fixture; an explicit return type removed the dead return. A native
presentation run then failed its Outline scroll assertion. Wheel routing relied
on the previous motion position; it now uses SDL3's wheel-event coordinates.
The regression deliberately sends a preceding motion in the editor and a wheel
in Outline, retaining the same expected symbol offset and unchanged Explorer
viewport assertions. The failed fixture remains under
`build/graphics/gui-presentation-216199756990900`. Final presentation, language
service/Git and TCP GUI checks all pass without empty-submit warnings. Regenerated
completion and Git-diff GPU screenshots were visually inspected. These
focused checks extend the preceding 456-case checkpoint; the complete current
460-case Debug/Release suites and new package have not yet been run.

Native editing intents now retain per-document ordering through deferred service
script completion, failure and cancellation. Other documents and viewport
queries remain eligible; asynchronous saves do not hold this editing gate.
Windows Debug `build -y gui-smoke gui-presentation-smoke` passed, followed by
real local/TCP `gui-smoke` and `gui-presentation-smoke`, all PASS. The user-script
regression suspends the first input on an explicitly released Promise, opens and
edits another document before releasing it, and proves later input in the original
document cannot bypass the gate. It also verifies captured identity, separate
undo histories and one transaction per input. No fixed-delay timing assertion
is used for document independence.

JS document reads now await preceding native input intents for their captured
document; presentation controls can read immediately. Cancellation removes the
waiting fence and does not cancel editing, and shutdown settles pending reads.
Line-copy policy obtains revision-checked service line bounds when native text
is old, including long partial rows. Copied byte counts come from UTF-8 service
ranges; the native `astra.utf8ByteLength` primitive handles JS string measurement.

Windows Debug `build -y astra-tests gui-smoke gui-presentation-smoke
gui-workspace-smoke` passed; the affected
`--test-case=workbench*,QuickJS*,JavaScript*` run passed **44 cases / 368
assertions**. All four actual GUI checks passed: local/TCP `gui-smoke`,
`gui-presentation-smoke` and `gui-workspace-smoke`. Added regressions prove a
read remains attached to the original document and LF policy after switching
to a CRLF-configured document, read cancellation finishes while the editing
gate is still suspended, and same-batch input followed by save or line-copy
observes the committed input. Two collapsed cursors on one long logical line
copy it once across Unicode pages; stale line revisions prevent publication.

The first long-line unit run failed with `InternalError: interrupted` in the
old JS byte-count loop at the workbench's 8 ms budget. Replacing that loop with
service byte ranges/native string measurement fixed the failure; the budget
was not increased. All final focused and GUI checks above pass. These add four
cases to the suite (464 total); full current Debug/Release and package acceptance
remain outstanding.

Client JS now owns dirty-tab/captured-document close decisions and composes the
current user save handler. Save/Save As return their durable result; save-close
uses its resulting revision, retains new dirty edits during save, and does not
close on dismissal/failure. Discard retains the pre-choice revision guard.
The native close/Save As workflow, specialized close modal and close-after-save
state were removed. The generic awaited choice component has 2–4 buttons,
configured defaults/keys, original value identity, named answers, cancellation
and shared paint/hit-test layout with vertical buttons in narrow windows.

Windows Debug `build -y astra-tests gui-smoke gui-presentation-smoke
gui-recovery-smoke` passed; final
`--test-case=workbench*,presentation*,QuickJS*,JavaScript*` passed **61 cases /
218,771 assertions**. The complete suite now has **469 cases**; full current
Debug/Release acceptance remains outstanding. Actual local/TCP `gui-smoke`,
`gui-presentation-smoke` and `gui-recovery-smoke` all passed after the final
fixes. GUI regressions cover same-batch input/close, explicit durable save-close,
discard without disk writes, cancellation, untitled Save As dismissal, named
decision actions, four-option default/key/pointer identity, narrow vertical
buttons, and genuine recovery checkpoint failure/retry/explicit bypass.
`gui-close-choice.png` and `gui-choice-narrow.png` GPU screenshots were visually
inspected; title, detail and choices are visible.

An intermediate presentation run timed out on window shutdown because the
native exit adapter attempted reentrant JS execution. Exit now records intent
and starts JS close work in the outer owner loop. The failed fixture is retained
at `build/graphics/gui-presentation-219889147400300`. A subsequent recovery run
reported `Window exit skipped the recovery flush`: idle was reported between
asynchronous close continuations. Pending window close now prevents premature
idle unless a prompt is actually presented; the original checkpoint assertions
were retained and the final recovery check passed.

One TCP GUI run then reported `Stale document revision` during a background
syntax refresh. A deterministic production regression reproduces it by making
another real client insert and undo immediately before the syntax request; that
regression failed before the fix. Native syntax queries now discard known-old
queued revisions and reschedule only their `-32001` stale-read response, keeping
mutation revision errors intact. Final local/TCP GUI runs pass with this actual
concurrent-client regression, including the existing stale-edit rejection check.

Remaining work includes the rest of the public UI/provider contract,
native component extraction, incremental display layout,
final acceptance and packaging of the
new revision. Historical M0-M8 states below are not completion claims for this
active revision.

## Historical integrated implementation

The final Windows integrated suites pass in both configurations: **408 cases /
211,698 assertions in Debug** and **408 cases / 211,712 assertions in Release**.
The native renderer, text layout, four editor GUI workflows and production TCP
GUI variant also pass in both configurations. Recovery and atomic-replacement
failures from earlier runs are fixed and covered by regressions below.

The final 6,527-file Windows ZIP from clean source commit `afcfbac` passed archive
hash verification, fresh extraction, inventory and isolated runtime smoke checks.
Only documentation recording that result changed after the package snapshot.
No Linux/macOS functionality has been tested here; Windows acceptance is not an
all-platform production-release claim.

| Milestone | Scope | State | Acceptance evidence |
| --- | --- | --- | --- |
| M0 | xmake, test runner, development rules, reproducible dependencies | Complete | Pinned xmake and recipe archives verified; two repository skills passed official validator |
| M1 | Persistent SumTree, UTF-8 Rope, positions, transactions, multicursor undo/redo | Windows verified | Final Debug/Release suites pass; 1/8/64 MiB Release benchmark validates snapshots, edits, position conversion and undo |
| M2 | Workspace, safe file I/O, recovery, EditorConfig, search/replace | Windows verified | Final suites include recovery ownership and atomic-replacement regressions; CR-only, RE2, dirty-buffer overlays, watch/conflict and GUI recovery/search workflows pass |
| M3 | libuv service, request/response/notifications, local/TCP clients, Agent API | Windows verified | Local/TCP parity, cancellation, bounded queues, idle notifications and shutdown pass; independent CLI and production TCP GUI pass |
| M4 | QuickJS-NG scripting, default commands and VS Code keymap | Windows verified | Runtime limits, incremental configuration, shared commands and actual SDL editing/search shortcuts pass |
| M5 | Tree-sitter syntax/textobjects/outline and LSP | Windows verified | Nine genuine pinned parsers, incremental syntax, real language services and clangd history diagnostics pass in final Debug/Release suites and GUI workflows |
| M6 | SDL3 + Skia Graphite + Dawn GUI, layout, text input, themes/icons | Windows automated checks passed | Actual GPU rendering, Unicode layout and all GUI workflows pass both modes; physical DPI/IME/device-loss scenarios remain outside this evidence |
| M7 | Complete workspace UI, git, global search, command palette | Windows verified | Both modes cover real Git/clangd, dirty/untitled search, search options, confirmed replacement, persistent external conflict and guarded reload |
| M8 | Packaging, recovery/failure/performance acceptance, documentation | Windows verified | Final suites and Release core benchmark pass; clean-source ZIP passes hash, extraction, inventory and isolated runtime smoke |

## Validation boundary

Only Windows tests may run in this environment. Cross-platform code paths and configuration
do not constitute Linux/macOS verification. No CI jobs for those systems are enabled here.
Native graphics/runtime/package scripts are source implementations only; they were not
executed on a foreign platform, emulator or cross toolchain during this work.

## Current acceptance checkpoint (2026-10-09)

The following evidence refers to the source snapshots actually tested, not every
subsequent edit. New tests increase the suite size; historical counts below are
retained rather than presented as current full-suite totals.

| Check | Windows Debug | Windows Release |
| --- | --- | --- |
| Integrated `astra-tests` at `0469874` | 456 cases / 428,194 assertions PASS | 456 cases / 428,318 assertions PASS |
| `graphics-smoke`, `renderer-smoke`, `text-layout-tests` | PASS | PASS |
| `gui-smoke`: editing, awaited commands/cancellation, Unicode/IME events, multicursor undo, save/close, palette and long-line paging | PASS | PASS |
| `gui-services-smoke`: real clangd completion/navigation/rename/formatting and real Git actions | PASS | PASS |
| `gui-recovery-smoke`: startup review, restore, dirty preservation and durable/failing exit | PASS | PASS |
| `gui-workspace-smoke`: search-result activation, Alt+C/R/W, confirmed replace, conflict notice and guarded reload | PASS | PASS |
| `gui-presentation-smoke`: 1,505 entries, shared geometry, paged Outline, tabs and indexed dialogs | PASS | PASS |
| Real clangd historical-content/undo diagnostic regressions | Focused 7 cases / 152 assertions PASS; final suite PASS | Final suite PASS |
| Earlier independent `astra-recovery-tests`: persistent store, ownership, corruption and real crash/reopen | 15 cases / 105 assertions PASS | 15 cases / 105 assertions PASS |
| Earlier Process/Git/LSP platform snapshot | 52 cases / 378 assertions PASS | 52 cases / 378 assertions PASS |
| Two low-level LSP `clangd_force_rebuild` cases | Final suite PASS | Final suite PASS |
| RecoveryState integration, including five lifecycle/concurrency regressions | All 9 cases PASS | All 9 cases PASS |
| Production TCP GUI channel, `gui-smoke --tcp` | PASS | PASS |
| Independent-process `tests/cli_smoke.ps1` | PASS | PASS |
| `astra-core-benchmark`: 1/8/64 MiB | Not run; Release benchmark | PASS; see [PERFORMANCE.md](PERFORMANCE.md) |
| Earlier ZIP at `afcfbac` in a freshly extracted directory | Not applicable | 6,527-file inventory, archive SHA-256, clean-source identity and isolated runtime smoke PASS after final file-I/O fix |

The earlier 408-case suite logs are `build/debug-integrated-results.txt` and
`build/release-integrated-results.txt`. The corresponding configured binaries
were run with `--no-version --duration=true`. Asynchronous polling produces small
differences in successful assertion totals between configurations. The complete
acceptance entry point is `./scripts/test.ps1 -Mode all -Gui -Benchmarks`;
[BUILD.md](BUILD.md) lists individual targets and prerequisites.

The latest Release CLI command was `./tests/cli_smoke.ps1 -Mode release`.
Debug also passed with `./tests/cli_smoke.ps1 -Mode debug`. After removing four
unused default-setting declarations, the focused default/configuration/user
policy checks passed: 7 cases / 75 assertions. No advertised setting remains
for the unimplemented word-wrap, line-number, auto-save or default-encoding switches.
An earlier validation package passed before the final file-I/O fix. The final
Release executables were then rebuilt with `./scripts/xmake.ps1 f -m release -y`
and `./scripts/xmake.ps1 build -y astra astra-server`. The source tree was clean
before and after these final package checks, all of which passed:

```powershell
./scripts/package-windows.ps1 -Offline
Expand-Archive -LiteralPath build/packages/astra-0.1.0-windows-x64.zip -DestinationPath build/packages/final-zip-check
./scripts/verify-windows-package.ps1 -PackageDirectory build/packages/final-zip-check/astra-0.1.0-windows-x64 -RunSmoke
```

That smoke uses an independent Unicode/spaced workspace, isolated profile/state
and a PATH containing only Windows directories. It exercises packaged executables,
TCP, Unicode save, Git, clangd, TypeScript, Python and recovery. The final artifact is
`build/packages/astra-0.1.0-windows-x64.zip` (142,939,954 bytes). Its manifest records
source commit `afcfbacfbe0bea228eaf6fc7df469211675c30c9`, `sourceDirty: false` and
runtime identity `windows-x64-7c73a1c471a39643`. Archive SHA-256, independently
compared with the adjacent `.zip.sha256` file before extraction:

```text
a9102859236494435ebcbb5022a4f066ba41f745748326c13079a5d7b48b9fa2
```

`gui-workspace-smoke` uses real SDL keyboard/mouse events, the actual shared
service and native file-monitor events. It verifies that clicking a dirty or
untitled search result activates its existing document ID and exact selection;
global replacement remains uncommitted until confirmation, reports modified
buffers and leaves their edits dirty with one undo step each. Atomic external
replacement produces a persistent conflict notice. Reload cancel preserves text;
a second client's edit invalidates an already-visible reload confirmation.
Screenshots are `build/graphics/gui-workspace-{search,replace-confirm,conflict,reload-confirm}.png`.

Two earlier full Debug runs of 400 cases each reported two failures. The later
of those pre-fix runs reported **398 passed / 2 failed**, with **211,636 passed /
2 failed assertions**. Both failures were recovery restore operations rejected
as having a live owner. A startup announcement was scanning
and temporarily claiming abandoned sessions outside the RecoveryState worker
mutex, so it could conflict with the same host's restore operation. The fix
serializes that scan and explicitly releases failed/completed restore claims.
Five new tests cover retained scheduler callbacks, durable transfer, parallel
startup/restore and immediate ownership release at `ServiceHost::shutdown`.
All nine recovery-service cases now pass in both final suites. The rebuilt
recovery GUI also passes; the failed pre-fix runs are retained as history.

An intermediate Debug run recorded **406 cases / 405 passed / 1 failed** and
**211,889 assertions, all passed**. The exception in the Unicode watcher fixture
was Windows `ReplaceFileW` error 1175, "Unable to remove file to be replaced".
The backup path had been reserved with an existing placeholder file; another
reader holding that placeholder could prevent replacement even with delete
sharing. The fix reserves a unique directory while leaving its backup filename
absent. Two new Windows regressions reproduce the placeholder failure and verify
replacement beside shared readers, retained old handles, stale-version rejection
and cleanup. Both final 408-case suites pass after this fix, as do the repeated
Release recovery/workspace GUI and CLI checks.

The Git cleanup follow-up passed its two focused cases. Fixture teardown now
permits bounded retries only for sharing/lock violations, retains failure
diagnostics and does not weaken the production process-completion contract.
The transient external holder was not conclusively identified. Child-process
tests still require immediately usable working directories after completion.

The following coherent modules were committed and pushed during this stage:

| Commit | Increment |
| --- | --- |
| `a0fc831` | Recovery |
| `1903b69` | LSP integration |
| `c39b253` | File monitoring and external-change handling |
| `e217270` | RE2 workspace search and replacement |
| `e127246` | Explicit POSIX process-group lifetime ownership (source only on Windows) |
| `0239ace` | Windows replacement-backup ownership and regression tests |
| `a4982cd` | Integrated asynchronous editor services and production TCP channel |
| `6fcf53c` | Native workspace UI and real GUI acceptance workflows |
| `39c0691` | Pinned runtime preparation and native packaging sources |
| `e001baf` | Persistent editing performance benchmark |

The Windows package is generated and verified. The Release core benchmark passed
for 1/8/64 MiB inputs:
edit P95 was 9.2/10.0/11.3 microseconds and load time was 1.9578/16.1456/130.548 ms.
It validates 1,000 Unicode insertions, retained snapshots, UTF-16 round trips and
undo; these are core measurements, not end-to-end GUI latency.
Physical monitor DPI transitions, real GPU device loss and an installed IME's
candidate interaction have not been induced by SDL event injection. Packaging
inventory checks alone do not establish runtime acceptance; the isolated smoke
above establishes only its recorded Windows package snapshot. Native platform
builds, runtime checks and distribution validation remain unverified.

## Earlier Windows evidence (2026-10-09)

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
- Process/Git modules: 17 real child-process cases and 19 real repository cases
  pass in Windows Debug and Release. Coverage includes Job Object descendants,
  immediate working-directory release, stream limits, cancellation, filtered
  CRLF discard, Unicode literal paths, merge conflicts and guarded staging.
  The file-store/strict-decoding subset also passes: 16 cases / 151 assertions.
  See `docs/PROCESS_GIT.md` for the independent module target and guarantees.
- Resolved build infrastructure failures: Windows sandbox blocked MSVC PDB/MSYS named
  objects; normal Windows execution plus embedded CodeView records works. xmake's automatic
  partial-clone lock path recursed on this host; the checked-in manifest now locks a
  SHA-256-verified official recipe archive used through xmake's supported local repository.

## Excluded by the product contract

Live collaboration, embedded AI agents, embedded terminal, run, and debug interfaces.
External AI Agent control for editor testing remains required.
