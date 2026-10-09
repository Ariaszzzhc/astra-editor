# Implementation and acceptance status

Product contract: `EditorDoc.md`. Product name: Astra (confirmed by the user).

## Active architecture and workbench revision (2026-10-09)

The previous Windows acceptance below covers the earlier implementation. It does
not establish acceptance of the requested JS-owned workbench, asynchronous user
API, cohesive UI, or indexed incremental presentation model. That revision is
**in progress**, and is not a completed product or milestone.

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
dispatcher. Native modal/language/recovery workflow migration remains
unfinished. Providers cancel superseded generations before publishing a model.
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

Remaining work includes the rest of the public UI/provider contract,
native component extraction, indexed diagnostic/display views, incremental
syntax indexes, full Debug/Release and native GUI acceptance, and packaging the
new revision. Historical M0-M8 states below are not completion claims for this
active revision.

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
| Latest integrated `astra-tests` | 408 cases / 211,698 assertions PASS | 408 cases / 211,712 assertions PASS |
| `graphics-smoke`, `renderer-smoke`, `text-layout-tests` | PASS | PASS |
| `gui-smoke`: editing, Unicode/IME events, multicursor undo, save/close and long-line paging | PASS | PASS before final file-I/O fix |
| `gui-services-smoke`: real clangd completion/navigation/rename/formatting and real Git actions | PASS | PASS before final file-I/O fix |
| `gui-recovery-smoke`: startup review, restore, dirty preservation and durable/failing exit | PASS | PASS again after final file-I/O fix |
| `gui-workspace-smoke`: search-result activation, Alt+C/R/W, confirmed replace, conflict notice and guarded reload | PASS | PASS again after final file-I/O fix |
| Real clangd historical-content/undo diagnostic regressions | Focused 7 cases / 152 assertions PASS; final suite PASS | Final suite PASS |
| `astra-recovery-tests`: persistent store, ownership, corruption and real crash/reopen | 15 cases / 105 assertions PASS | 15 cases / 105 assertions PASS |
| Earlier Process/Git/LSP platform snapshot | 52 cases / 378 assertions PASS | 52 cases / 378 assertions PASS |
| Two low-level LSP `clangd_force_rebuild` cases | Final suite PASS | Final suite PASS |
| RecoveryState integration, including five lifecycle/concurrency regressions | All 9 cases PASS | All 9 cases PASS |
| Production TCP GUI channel, `gui-smoke --tcp` | PASS | PASS before final file-I/O fix |
| Independent-process `tests/cli_smoke.ps1` | PASS | PASS again after final file-I/O fix |
| `astra-core-benchmark`: 1/8/64 MiB | Not run; Release benchmark | PASS; see [PERFORMANCE.md](PERFORMANCE.md) |
| Final ZIP in a freshly extracted directory | Not applicable | 6,527-file inventory, archive SHA-256, clean-source identity and isolated runtime smoke PASS after final file-I/O fix |

The final suite logs are `build/debug-integrated-results.txt` and
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
