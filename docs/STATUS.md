# Implementation and acceptance status

Product contract: `EditorDoc.md`. Product name: Astra (confirmed by the user).

## Historical draft-only formatting check (2026-10-10)

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
The entries below record tested increments. Newer entries at the top supersede
older remaining-work lists without replacing their acceptance or failure evidence.

Current remaining work (2026-10-11):

[ARCHITECTURE_AUDIT.md](ARCHITECTURE_AUDIT.md) records the source
responsibility/index map and findings. Windows clipboard contention now has
explicit error handling and real failure-path evidence; all ten public
form/clipboard/update wrappers forward caller cancellation/deadline options.
Native clipboard work still executes on the owner loop: delayed-provider
responsiveness, bounded cancellation and shutdown remain unresolved. The
request runtime's Promise/deadline evidence does not close that native-I/O gap
or establish whole-architecture completion.

- Audit the final C++ service/native-mechanism versus JavaScript policy/API
  boundaries, asynchronous cancellation/error/lifecycle behavior, and measured
  persistent indexes against every `EditorDoc.md` requirement. Component test
  results do not establish whole-architecture acceptance.
- Finish whole-workbench visual acceptance across editing, Explorer, search,
  Git, Outline, pickers, forms and modal/error states. Measured typography and
  individual component tests do not establish a cohesive final UI.
- Installed-IME document, editor/sidebar forms and modal input now have Windows
  Debug/Release acceptance for the tested Chinese profile. Other input services,
  physical monitor DPI transitions and real GPU device-loss remain unverified.
- Validate and package the final source snapshot on Windows, including clean
  extraction and isolated runtime acceptance. The earlier package is stale for
  this revision. No macOS/Linux runtime verification is claimed.

### Clipboard preservation and public options (2026-10-11)

The current increment (parent `efa8a24`) fixes per-call options for `ui.update`,
seven form methods and two clipboard methods. Tests load the real shipped JS
modules and run the QuickJS runtime. Already-aborted and queued-aborted requests
must reach no native dispatcher; each method also has a pending native future
with a short per-call deadline. Timeout rejects the caller, drains its late
reply, and prevents the downstream publication. These tests use an isolated
native host and establish request semantics, not clipboard integration latency
or rollback after native acceptance. Form focus takes a string field identity;
its fourth argument follows `selectAll` and carries the request options.

The Windows reader is a narrow checked Win32 adapter under
`include/astra/ui/clipboard_text.hpp` and `src/ui/clipboard_text.cpp`. It acquires
the real clipboard, bounds the Unicode scan to both its allocated storage and
the requested text budget, strictly converts UTF-16 to UTF-8, and checks
open/data/lock/conversion/unlock/close failures. Clipboard handles and memory
are borrowed; RAII releases the leases without freeing OS-owned data. Other
platforms retain the SDL main-thread getter with explicit empty/error handling;
their behavior was not executed in this environment. Empty field paste is a
no-op; failed acquisition occurs before selection/content/history changes.

The new Windows `gui-clipboard-smoke` fixture holds the real clipboard on an
owned thread and hidden window without changing existing contents, formats or
owner. It verifies public JS rejection, editor/sidebar form value and version
preservation, continued replacement of the original selection and undo to the
original Unicode content. A selected modal field must return its exact original
Unicode value after failed paste. Document text, revision and selections are
unchanged through every route. Leases have a release deadline and report cleanup
errors. The normal `scripts/test.ps1 -Gui` matrix now includes this fixture.

Windows evidence:

- `./scripts/format.ps1 -Offline` and
  `./scripts/xmake.ps1 build -y -j 2 astra-tests gui-clipboard-smoke gui-smoke gui-presentation-smoke`
  passed in Debug (`clipboard-options-build-debug.log`,
  `clipboard-native-adapter-build-debug.log`). Later fixture corrections passed
  `./scripts/xmake.ps1 build -y -j 2 astra-tests gui-clipboard-smoke`
  (`clipboard-field-id-build-debug.log`).
- `./build/windows/x64/debug/astra-tests.exe --no-version '--test-case=workbench*'`
  passed **99 cases / 6,382 assertions** on the final source
  (`clipboard-field-id-workbench-debug.log`).
- `./build/windows/x64/debug/gui-clipboard-smoke.exe` passed real clipboard
  contention, both form mounts and modal preservation
  (`clipboard-field-id-debug.log`).
- `gui-smoke.exe` and `gui-smoke.exe --tcp` passed normal production Unicode
  clipboard/editing/history/save/reopen regressions in Debug
  (`clipboard-regression-gui-debug.log`, `clipboard-regression-gui-tcp-debug.log`).
- `gui-presentation-smoke.exe` passed Debug native workbench/input/clipboard,
  indexed views, async picker/text/review cancellation and shutdown
  (`clipboard-regression-presentation-debug.log`).
- Release configure and the four affected targets built successfully
  (`clipboard-options-config-release.log`, `clipboard-options-build-release.log`).
  Final test-source formatting was then rebuilt with
  `./scripts/xmake.ps1 build -y -j 2 astra-tests`
  (`clipboard-final-source-build-release.log`); the final Release workbench run
  passed **99 cases / 6,382 assertions**
  (`clipboard-final-source-workbench-release.log`).
- `./build/windows/x64/release/gui-clipboard-smoke.exe`, `gui-smoke.exe` and
  `gui-smoke.exe --tcp` passed real failure preservation and normal editing
  (`clipboard-final-source-native-release.log`, `clipboard-regression-gui-release.log`,
  `clipboard-regression-gui-tcp-release.log`).
- `gui-presentation-smoke.exe` also passed Release
  (`clipboard-regression-presentation-release.log`). Rebuilt the final fixture
  with checked hidden-window destruction and reran its real failure workflow:
  `clipboard-cleanup-final-build-release.log` and
  `clipboard-cleanup-final-native-release.log` passed. Rebuilt the actual
  Release Astra executable (`clipboard-final-app-build-release.log`).
- `./scripts/format.ps1 -Check -Offline` passed **197 C++ / 15 JS files**
  (`clipboard-options-final-format-check.log`); `git diff --check` passed.

Retained failures: the first fixture did not observe the required JS rejection
(`clipboard-options-native-debug.log`). Reviewing the fixed SDL 3.4.12 Windows
backend independently showed open-failure paths can return empty text without
setting an SDL error, motivating explicit Windows acquisition. The initial
lease used `OpenClipboard(NULL)` and did not establish separate-window contention;
the next run also reported lease cleanup failure
(`clipboard-native-adapter-debug.log`). The fixture now uses a concrete owned
hidden HWND. The next run failed because the fixture passed a numeric field
index to the JS focus API (`clipboard-owned-window-debug.log`); corrected to
the actual string identity before the accepted run. Failed fixtures remain
under `build/graphics/gui-clipboard-*`.

The current Release editor-surface captures `build/graphics/gui-editor.png` and
`gui-editor-tcp.png` were generated by the accepted regression runs; the local
image was visually inspected. This checks one editor/Explorer/notice state, not
the full product-state visual matrix or the native caption (surface captures
omit window decorations).

- Final Debug configuration, real Astra executable, tests and clipboard fixture
  rebuilt successfully (`clipboard-final-config-debug.log`,
  `clipboard-final-source-build-debug.log`). The final workbench run again
  passed **99 cases / 6,382 assertions**
  (`clipboard-final-source-workbench-debug.log`), and the final owned-window
  clipboard failure/cleanup fixture passed
  (`clipboard-cleanup-final-native-debug.log`).
- Final `./scripts/format.ps1 -Check -Offline` passed **197 C++ / 15 JS files**
  (`clipboard-final-acceptance-format-check.log`). `git diff --check` passed.

This increment does not establish delayed-provider responsiveness or complete
the full UI/architecture/package acceptance.

### Installed IME forms and modal acceptance (2026-10-11)

Passed in Windows Debug and Release for the installed Chinese profile. The fixture now
opens editor-mounted and search-sidebar forms using the production asynchronous
JS UI API, then opens a modal input with an awaited JS result. Every field receives
actual OS `NI` and candidate-selection input; none receives an injected SDL text
packet. Native result strings are compared with SDL and the actual form value or
accepted modal result. Text, revision and selections of the shared document must
remain unchanged throughout all field workflows.

Native caret coordinates are converted from SDL window units to physical pixels
and required to lie inside the current production form/modal field geometry.
Native-only composition additionally requires a newly visible owned popup near
that caret, valid native candidate metadata and a captured real popup. Every field
also receives a second real composition cancelled with Escape: no native result,
SDL commit, field content change or owner dismissal is allowed. Mode restoration
drains events, rechecks observations/content and verifies conversion/open state;
final profile restoration verifies original identity and document preservation.

Evidence and retained failures:

- The paused draft build failed (`installed-ime-fields-build-debug.log`) because
  dynamically labelled waits passed `std::string` to a `const char*` helper. The
  helper now accepts a string. Form/modal first acceptance then passed in Debug
  (`installed-ime-fields-first-debug.log`).
- Adding physical field geometry produced an explicit int-to-float warning under
  warnings-as-errors (`installed-ime-fields-geometry-build-debug.log`); corrected
  conversions passed the next Debug build.
- The strengthened first run failed obtaining the installed input context after
  process-local profile switching (`installed-ime-fields-accepted-debug.log`),
  preserving `build/graphics/gui-ime-342080348100`. Astra idle is not an input
  service readiness observation. A bounded message-pumped context readiness wait
  now leases/releases the actual context and requires SDL text input active;
  absent contexts still fail after the deadline.
- `./scripts/format.ps1 -Offline` and
  `./scripts/xmake.ps1 build -y -j 2 gui-ime-smoke` passed
  (`installed-ime-fields-context-format.log`,
  `installed-ime-fields-context-build-debug.log`). Running
  `./build/windows/x64/debug/gui-ime-smoke.exe` passed document commit/history/save
  plus all three field commit/cancel/isolation routes
  (`installed-ime-fields-context-debug.log`). Earlier actual form/modal screenshots
  and the actual modal native popup were visually inspected.
- Release initially stopped before key injection on lost focus
  (`installed-ime-fields-accepted-release.log`). The retry still failed context
  readiness (`installed-ime-fields-focus-retry-release.log`), and continuous
  single-profile activation alone did not correct it
  (`installed-ime-fields-session-release.log`). No vendor or production cause is
  inferred from these failures. Each field now independently obtains verified
  focus on the owned root, with the same guards before/during every OS keystroke.
  A later run safely stopped on lost focus during modal composition
  (`installed-ime-fields-owned-focus-release.log`).
- The next native trace observed a Unicode `WM_CHAR` commit without an IMM
  result packet (`installed-ime-fields-focus-diagnostic-release.log`). The oracle
  previously required `GCS_RESULTSTR` exclusively. It now independently captures
  Unicode-window `WM_CHAR` units, strictly validates/encodes complete UTF-16 when
  no IMM result exists, and still requires exact equality with SDL and actual UI
  values. It never substitutes candidate labels or expected text for native data.
  This follows Microsoft's [WM_CHAR Unicode contract](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-char).
- Another run failed context acquisition after field composition began
  (`installed-ime-fields-native-char-release.log`). Focus fallback also used an
  unscaled caption offset, which could hit a resize border. It now uses the actual
  window DPI and frame/caption metrics, requires native `HTCAPTION` hit testing and
  verifies the pointer target is owned before clicking. These changes do not
  establish a unique cause for every historical failure.
- Final-source Release build and all four real input routes passed
  (`installed-ime-fields-caption-build-release.log`,
  `installed-ime-fields-caption-release.log`). Restored Debug configuration,
  rebuilt the same source and passed all four routes again
  (`installed-ime-fields-final-config-debug.log`,
  `installed-ime-fields-final-source-build-debug.log`,
  `installed-ime-fields-final-source-debug.log`). Final accepted captures use
  `gui-ime-1146219496400-*` (Release) and `gui-ime-1259810772900-*` (Debug).
  These final runs used the native IMM result route; they do not establish another
  installed provider's character-only route or platform behavior.
- Final `./scripts/format.ps1 -Check -Offline` passed 194 C++ and 15 JavaScript
  files (`installed-ime-fields-final-check.log`); `git diff --check` passed.
  Final Debug committed editor/sidebar/modal screenshots were visually inspected.
  Only this acceptance fixture and its documentation changed in this increment;
  the unrelated integrated suite was not rerun or relabelled as current evidence.

Other installed profiles, physical DPI transitions and real GPU device-loss
behavior remain unverified. This increment does not complete whole-workbench
visual or whole-product architecture/release acceptance.

### Actual installed Windows IME acceptance (2026-10-11)

Document-input acceptance passed in Windows Debug and Release. This increment
adds a Windows-only `gui-ime-smoke` that queries enabled
installed TSF keyboard profiles, activates a profile only for the test process,
directs OS input only at its owned SDL window and restores profile/focus on exit.
It observes real SDL composition/commit events and the shared document; no SDL
text packet is injected by this fixture. Form/modal input and other installed
input-service profiles are not covered by this acceptance. Earlier draft failures
below are retained; final-source results supersede them.

`./scripts/format.ps1 -Offline` and `./scripts/xmake.ps1 build -y -j 2 gui-ime-smoke`
passed (`installed-ime-format-debug.log`, `installed-ime-build-debug.log`).
`gui-ime-smoke.exe --probe` exited 0 and found an enabled installed Chinese
keyboard text service (`installed-ime-probe-debug.log`), a stronger prerequisite
than a loaded language layout but still not composition/candidate acceptance.
The first actual GUI run exited 1 before any test keystroke because Windows
declined foreground activation (`installed-ime-gui-debug.log`); preserved fixture
`build/graphics/gui-ime-356670724106000`. A revised focus path uses a verified
owned caption point and restores the pointer, without changing foreground-lock
settings. That run exited 1 before keyboard injection because the point was
obscured (`installed-ime-focus-gui-debug.log`), preserving
`build/graphics/gui-ime-356800561218300`. Temporary fixture-only caption stacking
then obtained focus and delivered OS input, but the run exited 1 after 15 seconds
without SDL preedit (`installed-ime-caption-gui-debug.log`), preserving
`build/graphics/gui-ime-356894707566200`. All three draft builds passed; failure
paths retain diagnostics rather than reporting installed-IME acceptance.

The pinned SDL header and its
[IME UI declaration documentation](https://wiki.libsdl.org/SDL3/SDL_HINT_IME_IMPLEMENTED_UI)
require the application to declare handled composition UI before initialization.
The current renderer draft adds `composition` before first video initialization,
leaving candidate UI native. Its formatted Debug build passed
(`installed-ime-composition-format-debug.log`, `installed-ime-composition-build-debug.log`),
but actual acceptance still exited 1 with no preedit
(`installed-ime-composition-gui-debug.log`), preserving
`build/graphics/gui-ime-357025986270500`. This is a candidate correction, not
proof that the missing declaration caused the observed timeout.

Subsequent draft checks verify the activated TSF language/CLSID/profile identity
and add read-only native-message and SDL-event observation. Identity, scan-code
and message-observer Debug builds passed (`installed-ime-identity-build-debug.log`,
`installed-ime-scancode-build-debug.log`, `installed-ime-message-build-debug.log`).
Their actual runs all exited 1 without preedit; corresponding `*-gui-debug.log`
files retain diagnostics and fixtures. The current fixture sends the complete
`NI` syllable rather than assuming a particular IME must publish after its first
letter. Its formatted build passed (`installed-ime-syllable-format-debug.log`,
`installed-ime-syllable-build-debug.log`) and actual run exited 1
(`installed-ime-syllable-gui-debug.log`), preserving
`build/graphics/gui-ime-357439109052500`.

The final diagnostic verifies native foreground/focus, SDL keyboard focus and
active text input. Both OS scan-code keys reach the owned window as queued
`VK_PROCESSKEY` (229), with zero queued IME composition messages, zero native
composition bytes and no SDL preedit/commit. The selected enabled input service
is CLSID `{E7EA138E-69F8-11D7-A6EA-00065B844310}`; no profile was installed or
enabled by the test. These observations narrow the unresolved path beyond
foreground activation and raw key delivery; they do not establish candidate
presentation, preedit or commit acceptance. Draft renderer/test changes remain
uncommitted. All recorded invocations are terminal; no acceptance job remains
running for this entry.

Further draft diagnostics on native Windows remain failures, not acceptance:

- Narrowing each IMM context lease to the immediate read/change operation built
  successfully but actual Astra input still timed out (`installed-ime-lease-build-debug.log`,
  `installed-ime-lease-gui-debug.log`). No lease now spans input or message pumping.
- A native EDIT child inside the SDL root reproduced the missing composition;
  live TSF profile description identified **搜狗拼音输入法**
  (`installed-ime-reference-build-debug.log`, `installed-ime-reference-gui-debug.log`).
  This child shared SDL's parent/pump and was insufficient to isolate that layer.
- Own-process security diagnostics found integrity RID 8192 (normal medium user),
  no restricted token and Job UI restrictions 0 for the escalated actual runs
  (`installed-ime-environment-probe-escalated.log`). The default sandbox probe
  was low integrity 4096 (`installed-ime-environment-probe-default.log`); it was
  not the context used for actual IME acceptance. Other Job policies were not audited.
- Separating scan-code press/release with a 35 ms message-processing dwell built
  but did not fix the child reference (`installed-ime-key-dwell-build-debug.log`,
  `installed-ime-key-dwell-reference-debug.log`).
- Replaced the child reference with a standalone Win32 root and EDIT control,
  a regular `PeekMessageW`/`TranslateMessage`/`DispatchMessageW` loop and RAII-owned
  native windows. This path creates no SDL window, Astra service or renderer.
  First build failed on the SDK's ANSI `IDC_ARROW` macro passed to `LoadCursorW`
  (`installed-ime-independent-build-debug.log`); the corrected formatted build
  passed (`installed-ime-independent-fixed-build-debug.log`). The independent
  `gui-ime-smoke.exe --reference` still exited 1 with two `VK_PROCESSKEY` messages,
  zero native composition bytes and no text (`installed-ime-independent-reference-debug.log`),
  preserving `build/graphics/gui-ime-358712408961300`.
- Added a real OS Latin-input control and two seconds of bounded native message
  processing after Chinese mode activation. The formatted Debug build passed
  (`installed-ime-settle-build-debug.log`). Independent EDIT accepted real `A`
  scan-code input; Chinese mode read back open=1/conversion=1/sentence=8, yet `NI`
  still produced no composition (`installed-ime-settle-reference-debug.log`, exit 1),
  preserving `build/graphics/gui-ime-358827972424600`. This establishes ordinary
  OS text delivery to the control, not Chinese candidate/preedit acceptance.
- Preserving the original conversion options when enabling native input built
  successfully (`installed-ime-preserve-mode-build-debug.log`). The original
  conversion was already 1, and the reference still timed out with unchanged
  mode (`installed-ime-preserve-mode-reference-debug.log`), preserving
  `build/graphics/gui-ime-358954342482100`; dropping other mode bits was not the
  observed cause in this run.
- Continuing the independent reference through a real Space key after the
  preedit timeout **did commit Chinese text**. The ordinary Win32 loop
  (`installed-ime-independent-commit-debug.log`) and an explicitly activated,
  RAII-deactivated TSF thread with `ITfMessagePump` (`installed-ime-explicit-tsf-debug.log`)
  both passed real Latin input and real Chinese candidate commit. Explicit TSF
  reported a focused document. Both complete checks still exited 1 because their
  required IMM composition observation remained absent; fixtures
  `build/graphics/gui-ime-359087047325700` and
  `build/graphics/gui-ime-359156137650400` are retained. The earlier timeout did
  not establish that Chinese commit was broken: it had stopped before trying
  commit. Adding TSF activation did not recover the missing observation.
- Added a test-only `SetWindowSubclass` observer that forwards every message to
  the original procedure, captures synchronous IME start/composition/end and
  character delivery, and removes itself before owned window destruction.
  The formatted Debug build passed (`installed-ime-native-trace-build-debug.log`).
  Actual Astra input still had no preedit messages, but IMM reported one candidate
  list / 218 bytes and Space committed text matching the actual shared document.
  The test then failed because its new atomic-revision assertion incorrectly
  read the protocol's decimal-string revision as a JSON number
  (`installed-ime-native-trace-astra-debug.log`), preserving
  `build/graphics/gui-ime-359360460646300`. This was a test-oracle error, not evidence
  of a production revision failure; it was corrected to parse the string.
- The corrected formatted Debug build passed
  (`installed-ime-native-trace-fixed-build-debug.log`). Actual
  `gui-ime-smoke.exe` passed candidate-list observation, exact shared-document
  Chinese commit, revision +1, single-step undo/redo and file-content verification
  after `ui.save` (`installed-ime-native-trace-fixed-astra-debug.log`). No SDL text
  packet was injected. The owned fixture's actual GPU capture
  `build/graphics/gui-installed-ime-committed.png` was inspected and shows **你**
  before the seed text. Synchronous tracing recorded no start/composition/end
  messages while typing, then one of each at commit, flags 2048 (`GCS_RESULTSTR`)
  and two result bytes. The complete check still exited 1 because app preedit
  observation/painting was absent, preserving
  `build/graphics/gui-ime-359460321707000`. The evidence establishes real Chinese
  commit/history/save for this input service, not full installed-IME UI acceptance,
  candidate-window placement, form/modal composition or another IME profile.

Native-route clarification and stronger evidence:

- The selection-history/restoration draft built and verified original selection
  undo/redo and restored conversion/open/profile identity; document text stayed
  unchanged during input preparation (`installed-ime-selection-build-debug.log`,
  `installed-ime-selection-astra-debug.log`). Visible windows in the test's own
  process/thread identified `SoPY_Comp` near the caret and a separate status window.
  This supersedes interpreting absent app preedit as an entirely failed IME path.
- Bounded, RAII-owned GDI capture of only the test-owned composition window built
  and ran (`installed-ime-popup-capture-build-debug.log`,
  `installed-ime-popup-capture-astra-debug.log`). The actual caret was
  (1904,768), size 2x37; the composition window was (1897,789)-(2546,945).
  Inspected `build/graphics/gui-ime-360032737128500/native-ime-candidate.png`
  shows **ni** and Chinese candidates, with **你** selected. The older full check
  still exited 1 because it required an app preedit event even for this native
  composition route. That requirement misclassified the observed native UI;
  it is not evidence that the editor failed to present composition to the user.
- The current acceptance retains app-preedit painting checks when offered, and
  additionally requires a newly visible test-owned native composition popup near
  the actual caret plus validated candidate metadata for native-only composition.
  It waits with a deadline for popup presentation and saves a unique actual-window
  PNG outside the cleaned workspace. No disabled/stub backend or synthetic text
  was used to pass this route.
- An added candidate-label equality assertion failed
  (`installed-ime-final-gui-ime-smoke-debug.log`,
  `installed-ime-candidate-order-astra-debug.log`): native candidate metadata
  returned the display label `你 `, while Space correctly committed `你`.
  Moving the read immediately before Space did not fix this mismatch. The current
  independent oracle captures the exact native `GCS_RESULTSTR` before forwarding
  the original window procedure and compares that UTF-8 result with SDL text and
  the shared document, preserving the candidate metadata and actual UI checks.
- That result-oracle Debug build passed (`installed-ime-result-oracle-build-debug.log`).
  Real document-input acceptance and affected `gui-smoke`, `gui-workspace-smoke`
  and `gui-presentation-smoke` all passed in a terminal, sequential invocation
  (`installed-ime-result-gui-ime-smoke-debug.log` and corresponding `*-debug.log`).
  Inspected actual native popup
  `build/graphics/gui-ime-360770356170700-native-composition.png` shows the original
  phonetic input and candidates. A final fixture addition drains events/errors
  after profile restoration and verifies no residual commit/document change;
  its final-source Debug recheck passed
  (`installed-ime-accepted-final-source-debug.log`), including restored-profile
  event draining and unchanged-document verification.
- Final-source Release document-input acceptance passed
  (`installed-ime-accepted-gui-ime-smoke-release.log`). Affected `gui-smoke`,
  `gui-workspace-smoke` and `gui-presentation-smoke` also passed sequentially
  (`installed-ime-accepted-*-release.log`). The native composition popup was
  captured and visually checked. This establishes the actual installed Chinese
  input service's document route, not whole-product IME or visual acceptance.
- Final `./scripts/format.ps1 -Check -Offline` passed all 194 C++ and 15 JavaScript
  files (`build/installed-ime-final-format-check.log`), and `git diff --check`
  passed. Final-source builds and acceptance ran only on native Windows.
- `scripts/test.ps1` now exposes `-Gui -InstalledIme`, builds the real fixture and
  invokes it per requested Windows configuration. PowerShell syntax and rejecting
  `-InstalledIme` without `-Gui` passed (`installed-ime-runner-guard.log`). An enabled
  installed Chinese profile and an interactive desktop are explicit prerequisites,
  not silently skipped capabilities; `--probe` remains a prerequisite check only.

The successful route establishes native composition UI and the exact native
result's passage through SDL into atomic shared document/history/save behavior.
Absent app preedit alone was an incorrect failure criterion for this native-UI
route. Process-local profile restoration
now reads back and verifies the original identity on the successful cleanup path.
No system profile was enabled/installed and no global input setting was changed.

API boundaries were checked against Microsoft's
[profile activation](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfinputprocessorprofilemgr-activateprofile),
[profile enumeration](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfinputprocessorprofilemgr-enumprofiles)
and [OS input](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput)
documentation. These references do not establish successful runtime behavior.

### Native font-measured nonmodal forms (2026-10-11)

Test-only regressions reproduced native and actual GUI clipping
before the production changes: `viewport-layout-tests.exe --forms` exited 1
with size 16 / DPI 0.75 / field height 21 / paragraph height 16; input padding
requires another 7.5 physical pixels (`inline-font-repro-native-debug.log`).
`gui-presentation-smoke.exe` exited 1 with the actual large-font Find field
clipping its configured paragraph (`inline-font-repro-gui-debug.log`), preserving
`build/graphics/gui-presentation-355086016108600`. The test-only Debug build
passed after scoping a duplicated local test name; the initial MSVC duplicate
declaration failure remains in `inline-font-repro-build-debug.log`, and the
corrected build in `inline-font-repro-fixed-build-debug.log`.

The current patch measures overlay and mounted form line extents with native
configured font probes, reserves each control's padding and snaps bounded
physical edges. Mounted forms start below the measured sidebar heading.
Agent geometry now calls the same layout path as drawing and pointer handling.
New checks cover large fonts, Unicode pixel admission, native caret/pointer
geometry, warm probes, tiny owners, mounted fields and document preservation.

Windows Debug evidence on this patch:

- `./scripts/format.ps1 -Offline` passed for 193 C++ / 15 JS files
  (`inline-font-format-debug.log`).
- `./scripts/xmake.ps1 build -y -j 2 astra-tests viewport-layout-tests gui-smoke
  gui-workspace-smoke gui-presentation-smoke astra` exited 0
  (`inline-font-build-debug.log`).
- `astra-tests.exe '--test-case=presentation *geometry*,presentation *forms*,presentation measured forms*'
  `--no-version` passed nine cases / 72,063 assertions
  (`inline-font-focused-debug.log`). The subsequent full `astra-tests.exe
  --no-version` passed 625 cases / 2,698,647 assertions, no failed/skipped cases
  (`inline-font-all-debug.log`).
- `viewport-layout-tests.exe --forms` passed native Unicode pixel admission,
  bounded caret/painting, independent pointer placement and warm probe reuse for
  overlay/mounted forms, four fields/toggles/actions, UI sizes 9/13/16 and DPI
  0.75/1/1.5/2 (`inline-font-native-debug.log`).
- `gui-presentation-smoke.exe`, `gui-workspace-smoke.exe` and `gui-smoke.exe`
  passed in one sequential invocation that exited 0 (`inline-font-gui-debug.log`,
  `inline-font-workspace-debug.log`, `inline-font-editing-debug.log`). Actual
  large-font overlay and mounted Unicode insertion follows an independent native
  font oracle, preserves document revision/selections, and closes by measured
  pointer bounds. Inspected actual GPU captures `gui-style-large-find.png` and
  `gui-style-large-workspace-form.png` show the original Unicode in these fields.

Release configuration and the same six-target build passed
(`inline-font-config-release.log`, `inline-font-build-release.log`). Full
`astra-tests.exe --no-version` passed 625 cases / 2,698,646 assertions with no
failed/skipped cases (`inline-font-all-release.log`). Native `--forms` and
`--dialogs` checks passed (`inline-font-native-release.log`,
`inline-font-dialog-release.log`), including the shared line-probe refactor.
Release `gui-presentation-smoke.exe`, `gui-workspace-smoke.exe` and
`gui-smoke.exe` passed (`inline-font-gui-release.log`,
`inline-font-workspace-release.log`, `inline-font-editing-release.log`). The
complete sequential full-Debug-unit/configure/build/Release-test invocation
exited 0. `./scripts/format.ps1 -Check -Offline` passed for 193 C++ / 15 JS files
(`inline-font-format-check.log`) and `git diff --check` passed.
Final format verification passed (`inline-font-final-format-check.log`). Restored
Debug with `./scripts/xmake.ps1 f -m debug -y`, then
`./scripts/xmake.ps1 build -y -j 2 astra` exited 0
(`inline-font-restore-debug-config.log`, `inline-font-restore-debug-build.log`).

This Windows font-layout increment is validated. Whole-product visual
acceptance, actual installed-IME interaction and a new distribution remain
within the requested revision and are not established by these checks.

### Native font-measured modal geometry (2026-10-11)

All nine production modal layout call sites now share native `DialogMetrics`
derived from cached configured UI font line probes, including CJK/emoji fallback
and the text panel's selected monospace family. Choice buttons, confirmation
items/buttons, picker/input regions and text-panel action rows reserve measured
line extents plus their control padding. Physical minimum heights round upward;
small windows retain ordered, clipped regions. Confirmation drawing uses the
same measured item step as layout. Native GUI acceptance reads the window's
actual metrics rather than recreating geometry with default font assumptions.
JS continues to own dialog contents, choices and asynchronous workflows.

Windows work so far:

- Formatting and initial Debug builds passed (`dialog-metrics-format-debug.log`,
  `dialog-metrics-build-debug.log`, `dialog-metrics-test-format-debug.log`,
  `dialog-metrics-test-build-debug.log`).
- `astra-tests.exe '--test-case=presentation *layout*,presentation measured dialog*'
  `--no-version` exited 0: four cases / 10,581 assertions, with 620 unrelated
  cases filtered out (`dialog-metrics-focused-debug.log`). Includes measured
  line extents, very small/narrow/scaled frames, ordered input/title regions,
  button containment and invalid metric rejection.
- New `viewport-layout-tests.exe --dialogs` initially exited 1 because its
  visible-glyph predicate required fully white pixels at small antialiased
  font sizes (`dialog-metrics-native-debug.log`). The predicate now counts
  glyph pixels inside the text region excluding borders, cursor and background.
  The next run exposed an input-height comparison failure
  (`dialog-metrics-native-fixed-debug.log`); an expanded diagnostic reproduced
  Segoe UI size 16 / DPI 0.75 with input height 24.499998 versus required 24.5
  (`dialog-metrics-native-diagnostic-debug.log`). Minimum control dimensions now
  round up to physical pixels. The new matrix additionally checks native input
  caret bounds, original Unicode pixels, no painting outside the measured
  control, an independent native paragraph pointer oracle and warm probe reuse.

The source also snaps all modal region edges after an actual native pixel check
detected painting at fractional clipping boundaries
(`dialog-metrics-rounded-native-debug.log`). The final snapped Debug build passed
(`dialog-metrics-snapped-build-debug.log`); the four focused cases / 10,581
assertions and the actual native font/pixel/caret/pointer matrix passed
(`dialog-metrics-snapped-focused-debug.log`, `dialog-metrics-snapped-native-debug.log`).
The matrix covers Segoe UI and Cascadia Code at UI sizes 9/13/16, DPI
0.75/1/1.5/2 and narrow/wide inputs. Async JS native-read state includes the same
modal extents. `./scripts/test.ps1 -Mode all -Gui` exited 0 on this fixed
source and unchanged resources (`dialog-metrics-acceptance-all.log`). Debug
passed all 624 cases / 2,695,606 assertions; Release passed all 624 cases /
2,695,465 assertions, with no failed/skipped cases. Both modes passed native
GPU/window/text/viewport checks, including the new modal matrix, and all actual
editing/services/recovery/workspace/presentation/body GUI targets plus the TCP
editing variant. Packaging helpers passed 12 checks; shaping provenance passed
seven tests. The
entry point also runs the new modal font/pixel matrix as part of the native
viewport suite. Final source formatting passed (`dialog-metrics-final-source-format-check.log`)
and `git diff --check` passed before the matrix. This Windows increment is
validated; the full revision is still in progress. Whole-product visual acceptance,
installed IME interaction and a new distribution remain within the requested
revision.
Final `./scripts/format.ps1 -Check -Offline` passed for 193 C++ / 15 JS files
(`dialog-metrics-final-format-check.log`), and `git diff --check` passed. Restored
Debug with `./scripts/xmake.ps1 f -m debug -y`, then
`./scripts/xmake.ps1 build -y -j 2 astra` passed
(`dialog-metrics-final-debug-config.log`, `dialog-metrics-final-debug-build.log`).

Review of the remaining input typography found that `InlineFormLayout` still
allocates fixed 28-unit fields and scales them by available height, independently
of configured native font extents. Mounted form placement also uses a fixed
32-unit sidebar-heading offset instead of the measured heading boundary. The
modal matrix does not establish font admission for those nonmodal search/forms.
After this completed modal acceptance, add native and actual GUI coverage
for configured large-font mounted/overlay fields, then correct their geometry.
These forms remain part of the requested revision. A read-only Windows
`GetKeyboardLayoutList` audit returned loaded English `0x04090409` and Chinese
`0x08040804` layouts. This identifies an available language prerequisite for a
later OS input test; it does not establish installed-IME composition/candidate
interaction, which the SDL event-injection suites do not exercise.

### JS-owned welcome and notice surfaces (2026-10-11)

`data/workbench/chrome.js` now supplies welcome title/detail, New/Open/Commands
actions with registered shortcut captions, recovery review and workspace-notice
dismissal. Four optional banks extend the existing asynchronous chrome provider:
`welcomeTitle`, `welcomeDetail`, `welcomeActions`, `notices`. Existing providers
may omit them; native publication normalizes empty banks before total 64 KiB
admission. Title/detail accept one row each, actions/notices eight each. Actions
and notices allow a validated 64-byte badge. These are original immutable
measured `ViewIndex` rows under the same context, publication and action fences.

The active context includes bounded notice text, original notice identity and recovery availability. The
window's former fixed welcome labels and recovery/workspace-banner paint/hit
rectangles are removed. `WorkbenchChrome` admits body rows inside the document
region, derives welcome heights from actual native paragraphs, reserves separate
badge text, and shares the painted/hit boxes. Welcome targets disappear as soon
as the owner has an active document, even while old pixels/models are retained.
Notice banks have bounded height admission, favoring earlier rows. Host
diagnostics take priority and reserve their own occluding target; they are
presented directly by this same component even when a JS provider fails. Their
display is bounded to 4 KiB with scalar-safe cuts, newline normalization and
visible replacement of malformed bytes; the original host error remains intact.

Windows work so far:

- `./scripts/format.ps1 -Offline` passed for 191 C++ / 15 JS files
  (`body-policy-format-debug.log`). Targeted Debug compilation passed
  (`body-policy-build-debug.log`), including `astra-tests`, native-font pixels,
  the new independent `gui-body-smoke`, recovery GUI and application.
- Recovery acceptance now clicks the actual published JS recovery notice using
  native SDL pointer events before reviewing/restoring genuine recovery state.
  Its formatted rebuild passed (`body-policy-format-recovery-debug.log`,
  `body-policy-build-recovery-debug.log`).
- Final model validation and its negative required-bank case compiled successfully
  (`body-policy-format-final-debug.log`, `body-policy-build-final-debug.log`).
  New behavioral coverage checks legacy normalization, original body descriptors,
  stale notice actions, atomic invalid/count/badge rejection, shipped JS action
  routing, font/DPI/narrow-window pixels, host diagnostics independent of failed
  providers, and real welcome/notice replacement and restoration.
- `./build/windows/x64/debug/astra-tests.exe
  '--test-case=presentation chrome*,workbench JS chrome*' --no-version` exited 0:
  nine cases / 67,459 assertions, with 614 unrelated cases filtered out
  (`body-policy-focused-debug.log`).
- `./build/windows/x64/debug/viewport-layout-tests.exe --chrome` exited 0
  (`body-policy-native-pixels-debug.log`). Native body pixels additionally cover
  three UI sizes, four DPI values, narrow/wide and short/tall frames, Unicode
  badges, long/malformed host error display, warm reuse, owner clips, identical
  paint/hit identities and no overlap with diagnostics.
- First `gui-body-smoke.exe` exited 1 before welcome checks because its fixture
  used the nonexistent `ui.close` command (`body-policy-gui-debug.log`). The
  retained fixture is `build/graphics/gui-body-350423059222100`. Corrected the
  fixture to the registered `ui.closeTab`; formatted rebuild passed
  (`body-policy-format-gui-fixed-debug.log`, `body-policy-build-gui-fixed-debug.log`).
  Corrected `gui-body-smoke.exe` exited 0 (`body-policy-gui-fixed-debug.log`):
  actual default New/Open/Commands actions, JS workspace dismissal, asynchronous
  Unicode body replacement at UI size 16 / 480-by-420 logical resize, notice
  actions without selection fallthrough, visible failed-provider diagnostics,
  restoration and unchanged source files. `gui-recovery-smoke.exe` also exited 0
  (`body-policy-recovery-gui-debug.log`), entering real recovery review through
  the published notice and retaining its existing save/exit/failure coverage.
- Inspected real GPU captures `build/graphics/gui-body-welcome.png`,
  `gui-body-custom.png` and `gui-body-diagnostic.png`. Welcome commands and
  shortcut columns are separated; original Chinese/emoji and custom notices
  remain visible at the narrow size; failed-provider diagnostics remain visible
  below retained notice pixels. These are fixture-level visual checks.
- `./scripts/test.ps1 -Mode all -Gui` finished producing its acceptance log on this source and
  unchanged runtime resources (`body-policy-acceptance-all.log`). The entry
  point now builds/runs `gui-body-smoke` in both Windows configurations.
  Debug passed all 623 cases / 2,690,421 assertions, with no failed/skipped
  cases. Debug native GPU/window/text and all GUI/TCP targets passed, including
  the new body and recovery-notice-click checks. Release passed all 623 cases /
  2,690,273 assertions, with no failed/skipped cases. Its native and GUI/TCP
  checks also passed; the log reaches the final packaging helper (12 checks) and
  shaping provenance (seven tests) passes. The original process handle is no
  longer available, so its shell exit status was not recovered. These results
  precede the additional identity regression below.

After the initial matrix, a real Release `gui-body-smoke.exe` regression through
the existing `workspaceNotice` primitive exited 1: an old action cleared a newer
original notice sharing its first 4 KiB (`body-identity-repro-gui.log`). Preserved
fixture: `build/graphics/gui-body-352122453741500`. The test-only build and
formatting passed (`body-identity-repro-build.log`, `body-identity-repro-format.log`).
Native notice facts now receive distinct owner-local sequence identities; chrome
includes the selected original sequence independently of its truncated caption.
Default JS actions forward that identity through command handlers, and native
dismissal rechecks it immediately before mutation. Added real GUI coverage for
same-prefix replacement and replacement arriving inside an awaited handler;
explicit unguarded dismissal continues to clear the current warning. A related
default shortcut projection now caps badge UTF-8 bytes at 64 without splitting
scalars; its unit regression uses 20 emoji in registered shortcut metadata.
Final-source Release acceptance passed: formatted rebuild
(`body-identity-format.log`, `body-identity-build-release.log`), then
`./build/windows/x64/release/astra-tests.exe --no-version` exited 0 with all
623 cases / 2,690,116 assertions (`body-identity-unit-release.log`). The embedded
GUI JS fixture was separately expanded using pinned Prettier, then formatted
and rebuilt (`body-identity-fixture-format.log`,
`body-identity-fixture-build-release.log`). A single affected-GUI invocation
exited 0 after `gui-body-smoke`, `gui-recovery-smoke`, `gui-workspace-smoke`, and
`gui-presentation-smoke`; each log is
`body-identity-<target>-release.log`. The body fixture proves same displayed
prefix/distinct original sequence, stale-action preservation, replacement during
an awaited command handler, original editor selections, and explicit current
dismissal. Recovery/workspace/presentation preserve their full existing real
renderer/service workflows.
Final-source Debug configuration/build passed (`body-identity-config-debug.log`,
`body-identity-build-debug.log`). The affected invocation exited 0:
`astra-tests.exe '--test-case=presentation chrome*,workbench JS chrome*'
--no-version` passed nine cases / 67,463 assertions, with 614 unrelated cases
filtered out (`body-identity-focused-debug.log`), `viewport-layout-tests.exe
--chrome` passed actual native-font body pixels (`body-identity-native-pixels-debug.log`),
and all four affected GUI targets passed (`body-identity-<target>-debug.log`).
These final-source checks supplement the initial full matrix rather than claiming
the unchanged initial matrix was rerun after the notice-identity fix.

This Windows increment is validated; the complete requested revision is still
in progress. Final `./scripts/format.ps1 -Check -Offline` passed for 191 C++ /
15 JS files (`body-identity-final-format-check.log`), and `git diff --check`
passed. Debug remains the configured local application build.
Modal/input typography and whole-product visual acceptance, physical installed
IME acceptance and a new distribution remain within the full requested revision.

### JS-owned outer chrome (2026-10-11)

The shipped `data/workbench/chrome.js` supplies the brand, command hint, activity
and management buttons, segmented document path and left/right status items.
It formats native caret metrics explicitly as visual columns or UTF-16 columns,
and owns command/view/Explorer-reveal actions. User scripts can replace and
restore the asynchronous provider and handler through `ui.registerChrome` and
Promise-based `ui.chrome` primitives. The native window no longer contains the
old activity paint/hit loop, path caption, or status-string formatting policy.

Native `ChromeState` fences the complete owner context, including active-document
activity, revision, metadata sequence and primary-caret coordinates. Six bounded
row banks use immutable measured `ViewIndex` snapshots. Candidate validation is
atomic, generations are immutable, and pending/failed/retired pixels cannot
activate. Models are capped at 64 KiB, activity at 16 rows, path/status banks at
32 rows each, and command/manage at one row each. `WorkbenchChrome` measures and
paints the same targets. Activity overflow has pointer controls and wheel
navigation; path layout retains fitting final segments; status admission keeps
left/right targets separate and favors earlier right-hand items. All captions
use configured UI fonts and shared native grapheme-aware elision.

Provider waits have a configurable 1..30,000 ms deadline, bounded host requests,
supersession cancellation, late-error suppression and retained failure pixels.
JS exit policy suspends chrome before its first idle fence, and resumes
it when that attempt finishes, so a signal-ignoring provider cannot force exit
to wait for its deadline. Status messages now have the same validated 4 KiB
single-line UTF-8 bound as other window labels. Agent state exposes the original
model/context and visible target identities/rectangles.

Windows evidence and retained failures:

- Initial targeted build failed with MSVC C4244/C2220 for an integer reference
  font size passed to a float interface (`outer-chrome-build-debug.log`). Float
  literals fixed the warning; no suppression was introduced.
- Corrected build and focused tests passed
  (`outer-chrome-build-fixed-debug.log`, `outer-chrome-focused-debug.log`): six
  cases / 67,410 assertions. Native publication coverage uses 2,000 rounds with
  seed `0xA57A9B`, preserving old snapshots and checking action guards and atomic
  malformed/capacity rejection.
- Expanded GUI-test build failed with MSVC C2666 for ambiguous JSON/string-view
  equality (`outer-chrome-build-expanded-debug.log`). Explicit original-string
  reads fixed the comparison (`outer-chrome-build-expanded-fixed-debug.log`).
- `./build/windows/x64/debug/viewport-layout-tests.exe --chrome` passed
  (`outer-chrome-native-pixels-debug.log`). Actual native-font pixels cover three
  UI sizes, four DPI values and narrow/wide windows: bounded overflowing activity
  targets, hit identity, separated status items, hover pixels, warm-cache reuse
  and canvas/clip ownership.
- Final formatting/build passed (`outer-chrome-format-final-debug.log`,
  `outer-chrome-build-final-debug.log`), covering 190 C++ and 15 JS files.
  `./build/windows/x64/debug/astra-tests.exe
  '--test-case=presentation chrome*,workbench JS chrome*' --no-version` passed
  seven cases / 67,418 assertions (`outer-chrome-focused-final-debug.log`). Added
  behavior covers native-context retirement before the next JS event, original
  path/coordinate projection, registered action dispatch, deadline settlement and
  cancellation of a signal-ignoring provider on a nonaccepting context.
- Real `gui-presentation-smoke.exe` passed custom Unicode header/manage/path/
  status actions, activity overflow and restoration, but exited 1 on the new
  signal-ignoring-provider shutdown case (`build/outer-chrome-gui-debug.log`).
  The preserved fixture is `build/graphics/gui-presentation-348120284800500`.
  Exit policy suspended too late: its first idle fence still awaited the provider.
  Suspension now surrounds the whole exit workflow, with restoration in `finally`.
  Formatting/build and the seven focused cases / 67,418 assertions passed again
  (`outer-chrome-format-exit-fixed-debug.log`, `outer-chrome-build-exit-fixed-debug.log`,
  `outer-chrome-focused-exit-fixed-debug.log`). Fresh
  `./scripts/test.ps1 -Mode all -Gui` exited 0 on this fixed source and unchanged
  runtime resources (`build/outer-chrome-acceptance-all.log`). Debug passed all
  621 cases / 2,690,270 assertions; Release passed all 621 cases / 2,690,183
  assertions, with no failed or skipped cases. Both modes passed native GPU/text
  checks, editing/services/recovery/workspace GUI workflows, presentation including
  custom chrome actions, overflow/restoration and signal-ignoring-provider exit,
  and the TCP editing variant. Packaging helpers passed 12 checks and shaping
  provenance passed seven tests. This is not a new packaged distribution.
  Inspected the current `build/graphics/gui-outer-chrome-custom.png` and
  `gui-workbench.png`: original Unicode captions, fitting path segments,
  separated status banks and bounded activity controls are visible. These
  fixture captures do not establish whole-product visual acceptance.
- Final `./scripts/format.ps1 -Check -Offline` passed for 190 C++ / 15 JS files
  (`outer-chrome-final-format-check.log`), and `git diff --check` passed. Restored
  Debug with `./scripts/xmake.ps1 f -m debug -y`, then
  `./scripts/xmake.ps1 build -y -j 2 astra` passed
  (`outer-chrome-final-debug-config.log`, `outer-chrome-final-debug-build.log`).

This Windows increment is validated; the full revision is still in progress.
The empty-editor welcome
captions and recovery/notice presentation still live in `EditorWindow::draw`,
with fixed offsets and separate pointer rectangles. Moving that product policy
to JS and sharing bounded native geometry, complete font adaptation outside
these components, whole-product visual acceptance, physical installed-IME
acceptance and a new distribution remain part of the full requested revision.

### JS-owned Outline pages and actions (2026-10-11)

The shipped `data/workbench/outline.js` now owns visible symbol queries,
pending/unsupported/empty messages, depth/icon projection and symbol activation.
The native highlighter requests `includeOutline:false`. Both clients use the
shared service parser and measured symbol range; abandoning an Outline wait does
not cancel that shared parse. User scripts can replace/restore the provider and
activation handler, with Promise-based refresh/read/publication methods.

Native `ViewPage` fences document, text revision, document activity, viewport and
monotonic generation. Publications contain at most 256 leaf rows / 64 KiB,
validate completely before mutation and are immutable within a generation.
Pending, hidden, retired and duplicate publications cannot activate. A shrinking
source can clamp its offset; the owner retains each document's scroll position.
`ViewIndex::page` supplies an immutable measured page with off-page indentation,
without pretending it contains parent groups. The shared `ViewRows` component
paints/hits these rows and accepts JS `iconTone` theme roles. The old native
Outline projection, messages, painting loop and direct selection policy were
removed. Agent state includes the guarded page and bounded original descriptors.

Windows evidence and retained failures:

- Initial `./scripts/xmake.ps1 build -y -j 2 astra-tests gui-presentation-smoke astra`
  failed with MSVC C4834/C2220 in exception tests that discarded `[[nodiscard]]`
  results (`build/outline-policy-build-debug.log`). Explicit void casts retain
  the exception assertions; no warning suppression was introduced.
- Formatting and corrected targeted build passed
  (`outline-policy-format-fixed-debug.log`, `outline-policy-build-fixed-debug.log`).
  `./build/windows/x64/debug/astra-tests.exe
  '--test-case=view page*,workbench JS Outline*' --no-version` passed four cases /
  73,877 assertions (`outline-policy-focused-debug.log`), with 609 unrelated cases
  filtered out. Native differential coverage uses 2,000 rounds / seed `0xA57A9A`,
  retaining old snapshots and checking source order, stale generation rejection,
  atomic malformed/capacity rejection, hidden activation and shrinking ranges.
- `./build/windows/x64/debug/gui-presentation-smoke.exe` exited 0
  (`outline-policy-gui-debug.log`). Actual SDL/GPU input requests a bounded page
  of 81 symbols, independently scrolls to rank 60 and selects the original
  `outline59` symbol. A user JS provider awaits asynchronously, publishes two
  nested-indented Unicode descriptors, handles actual pointer activation without
  altering document selections and restores the shipped provider. Existing
  5,005-file, 640-tab and custom-view checks pass.
  `build/graphics/gui-custom-outline.png` was inspected: original Unicode labels
  and JS-selected added/changed icon colors appear in the Outline pane.
- Subsequent final formatting/build passed
  (`outline-policy-format-final-debug.log`, `outline-policy-build-final-debug.log`).
  Final focused tests passed five cases / 73,896 assertions
  (`outline-policy-focused-final-debug.log`). Added cases prove that superseded
  shared-parser waits drain late replies without `operation.cancel`, and that
  current failure/AbortError pages settle with scalar-safe bounded messages.
- `./scripts/test.ps1 -Mode all -Gui` exited 0 with source/runtime resources
  unchanged (`build/outline-policy-acceptance-all.log`). Debug passed 614 cases /
  2,656,130 assertions; Release passed 614 cases / 2,655,923 assertions, with no
  failed or skipped cases. Both modes passed the real GUI Outline provider,
  activation and restoration checks, renderer, editing/services/recovery,
  workspace-conflict and TCP GUI checks. Package helpers passed 12 checks and
  shaping provenance passed seven tests.
  That matrix preceded the retired-provider regression fix described below.
- The first regression-test build failed with MSVC C2039 because the test read
  an ID directly from `ViewRow` instead of its original descriptor
  (`outline-retired-build-debug.log`). The corrected build passed
  (`outline-retired-build-fixed-debug.log`). Running
  `./build/windows/x64/debug/astra-tests.exe
  '--test-case=workbench JS Outline retired*' --no-version` then reproduced a
  global error after native publication rejection: one case / 15 assertions,
  one failed assertion (`outline-retired-reproducer-debug.log`, executable exit 1).
  The test deliberately delays the next viewport event after native refresh,
  then verifies the new provider still publishes normally.
- JS now reports a provider failure globally only if the native error-page
  publication is accepted and the request remains current. Formatting and build
  passed (`outline-retired-format-final-debug.log`,
  `outline-retired-build-final-debug.log`); the focused command with
  `--test-case=view page*,workbench JS Outline*` passed six cases / 73,911
  assertions (`outline-retired-focused-final-debug.log`).
- Fresh `./scripts/test.ps1 -Mode all -Gui` exited 0 on the fixed source and
  unchanged runtime resources (`build/outline-retired-acceptance-all.log`). Debug
  passed 615 cases / 2,656,056 assertions; Release passed 615 cases / 2,655,982
  assertions, with no failed or skipped cases. Both modes passed actual Outline
  pagination, pointer activation, custom asynchronous provider/handler and
  restoration checks, plus the full renderer/text/viewport, editing,
  clangd/Git, recovery, workspace-conflict and TCP GUI matrix. Package helpers
  passed 12 checks; shaping provenance passed seven tests.
- Restored the Debug development configuration and built `astra` successfully
  (`outline-retired-restore-debug.log`, `outline-retired-restore-build-debug.log`).
  `./scripts/format.ps1 -Check -Offline` passed all 188 C++ / 14 JavaScript files
  (`outline-retired-format-check.log`); `git diff --check` passed. The final GPU
  custom-Outline capture was inspected again after the complete matrix: Unicode,
  off-page indentation, themed icons and the original selected symbol remain
  visible (`build/graphics/gui-custom-outline.png`). No new distribution was made.

Outline policy ownership is accepted for the recorded Windows checks. This does
not establish whole-product visual acceptance. Activity-bar and breadcrumb/status
components, complete font adaptation, whole-product visual acceptance, physical
installed-IME acceptance and a new distribution remain part of the requested
architecture/UI revision. The full revision remains in progress.

### JS theme on the native Windows frame (2026-10-11)

Startup and accepted asynchronous style updates now apply the compiled JS
`activityBar`, `foreground` and `border` roles to the SDL-owned Windows caption,
caption text and border. Caption luminance controls the OS dark-mode allowance;
the native adapter introduces no second application palette. Read-only support
flags appear in `ui.read().windowFrame` and Agent presentation state. The style
snapshot remains directly restorable. Unsupported OS attributes remain under OS
control, while other native errors reject publication and attempt complete
attribute rollback, reporting rollback failure too. Pending invalid/cancelled
updates leave the frame and document identity unchanged.

Windows evidence and retained failures:

- The initial Debug targeted build failed with MSVC C4701/C2220 for an
  uninitialized test array (`build/window-frame-build-debug.log`). Arrays now
  initialize explicitly; no warning suppression was introduced.
- `build/window-frame-probe-native.log` exposed an invalid acceptance assumption:
  DWM color getters returned E_INVALIDARG although setters are supported on this
  Windows system. Support now follows actual writes; the adapter retains its
  previously owned color values and still reads the supported dark-mode flag.
- Earlier native runs failed actual color assertions despite accepted writes:
  `window-frame-renderer-write-debug.log` lacked the border in PrintWindow
  output; the composed/presented/redraw/oracle/framechange logs retained stale
  light-frame pixels through GDI capture. The visible-capture attempt failed
  its foreground-ownership guard (`window-frame-renderer-visible-debug.log`)
  before desktop capture. Those failures were not treated as acceptance.
- The replacement test helper captures only the owned HWND with Windows Graphics
  Capture and reads back a bounded composed frame. The first helper build failed
  with C2227 for a void ABI pointer and C2872 for an ambiguous `DirectX` namespace
  (`window-frame-build-capture-debug.log`). Projected close calls and explicit
  namespace aliases fixed these issues. HRESULT exceptions become reported test
  errors; capture/frame/session resources close explicitly and report failure.
- `./scripts/format.ps1 -Offline` and
  `./scripts/xmake.ps1 build -y -j 2 renderer-smoke gui-presentation-smoke astra`
  passed (`window-frame-format-interop-debug.log`,
  `window-frame-build-interop-debug.log`).
  `./build/windows/x64/debug/renderer-smoke.exe` exited 0
  (`window-frame-renderer-interop-debug.log`). It covers 2,000 differential rounds
  (seed `0xA57A99`), partial write/rollback/refresh failures and unsupported
  attributes, then actual dark/light caption/text/border pixels, invalid palette
  preservation, minimize/restore, GPU output and renderer lifetime/restart.
- `./build/windows/x64/debug/gui-presentation-smoke.exe` exited 0
  (`window-frame-gui-interop-debug.log`). Real JS Promise updates expose frame
  support and change the native colors, retain actual title-bar pixels through
  invalid/cancelled updates, restore the original style and retain document
  identity. Existing 5,005-file, 640-tab and custom-provider/chrome checks pass.
  Both renderer PNGs and the real GUI dark/light PNGs were inspected. The initial
  light fixture changed only three roles and had insufficient client text
  contrast; the final fixture supplies a complete light palette. A fixed
  equal-value capability-probe failure/rollback check was also added afterward.
- Final formatting and targeted build passed
  (`window-frame-format-final-debug.log`, `window-frame-build-final-debug.log`).
  Final `./scripts/test.ps1 -Mode all -Gui` exited 0 with source/runtime resources
  unchanged throughout execution (`build/window-frame-acceptance-all.log`).
  Debug: 609 cases / 2,582,176 assertions; Release: 609 / 2,582,031, zero
  failed/skipped. Both modes pass the actual native frame and JS GUI checks,
  together with renderer/fonts/worker/viewport, editing, language/Git services,
  recovery, workspace, workbench and TCP flows. Twelve package-helper and seven
  shaping-provenance checks also pass. The final Release
  `build/graphics/gui-native-frame-dark.png` and `gui-native-frame-light.png`
  were inspected: both include the composed OS title bar, and the complete light
  fixture retains readable client text. No new distribution was produced.
- Restored Debug with `./scripts/xmake.ps1 f -m debug -y`
  (`window-frame-restore-debug.log`). Final `./scripts/format.ps1 -Check -Offline`
  passed 185 C++ / 13 JS files (`window-frame-final-format-check.log`);
  `git diff --check` passed.

This entry does not establish whole-product visual acceptance. Remaining work
includes activity-bar and breadcrumb/status components, Outline content/provider/
action ownership, complete font adaptation, whole-product visual acceptance,
physical installed-IME acceptance and a new distribution. The requested
architecture/UI revision remains in progress.

### Shared workbench header and sidebar chrome (2026-10-11)

`WorkbenchChrome` now presents the root command center and sidebar headings,
refresh/cancel control, root section and Outline heading. It borrows the shared
native `TextLabels` cache; it owns no documents, services or command workflows.
JS specifications supply view captions, including optional `outlineTitle` with
the public default `Outline`. Native registration retains that fallback for older
direct specifications and validates captions before changing state. Async view
reads expose original `title`, `outlineTitle` and root `expanded` state; native
elision never changes their identity. Existing JS refresh/cancel and Outline
commands remain the action path.

Caption painting centers text vertically, measures command hint width and reserves
the refresh rectangle before grapheme-aware elision. A busy hovered refresh shows
the cancel glyph. Font-aware `WorkbenchLayout` takes the configured 9..16-unit UI
font, keeps default 13-unit band dimensions, grows relevant bands for line metrics,
snaps shared edges to physical pixels and clips them to the frame. It rejects
non-finite input/derived geometry. Paint and hit testing use the same heading,
refresh, section and Outline boxes. Canvas state restores on text failure, and
hidden sidebar captions are not prepared. The production SDL3/Skia Graphite/Dawn
stack, indexed row source and JS provider publication remain in place.

Windows evidence and retained failures:

- Initial targeted build failed with MSVC LNK2019 for `hit_sidebar` in
  `astra-tests` (`build/workbench-chrome-build-debug.log`): pure geometry tests
  must not link the drawing library. The pure hit function is now inline in its
  public header; drawing remains in `astra-ui`. The next targeted build passed
  (`workbench-chrome-build-fixed-debug.log`).
- The first native pixel run rejected chrome painting into indexed row content
  (`build/workbench-chrome-native-debug.log`). Fractional Outline boundaries
  were rounded differently by paint clipping and float geometry. Shared layout
  edges now snap to the pixel grid; the original separation assertion remains.
  The corrected build and native run passed (`workbench-chrome-build-pixels-debug.log`,
  `workbench-chrome-native-pixels-debug.log`).
- Final `./scripts/format.ps1 -Offline` and
  `./scripts/xmake.ps1 build -y -j 2 astra-tests viewport-layout-tests
  gui-presentation-smoke astra` passed (`workbench-chrome-format-final.log`,
  `workbench-chrome-build-final-debug.log`).
  `./build/windows/x64/debug/astra-tests.exe
  '--test-case=presentation chrome*,presentation workbench layout*,workbench provider generation*'
  --no-version` passed three cases / 33,537 assertions, with 606 unrelated cases
  filtered out (`workbench-chrome-focused-final-debug.log`). Coverage includes
  2,000 geometry rounds (seed `0xA57A98`), bounded/shared control edges, invalid
  font and extreme DPI rejection, atomic caption validation and real JS caption
  registration without changing provider-generation checks.
- `./build/windows/x64/debug/viewport-layout-tests.exe --chrome` passed
  (`workbench-chrome-native-accepted-debug.log`). Actual native-font pixels at
  sizes 9/13/16 and DPI 0.75/1/1.5/2 cover Unicode elision, visible captions,
  refresh/title separation, indexed-row separation, warm reuse, hover/cancel,
  hidden views and canvas ownership after malformed UTF-8 failure.
- `./build/windows/x64/debug/gui-presentation-smoke.exe` exited 0
  (`workbench-chrome-gui-debug.log`). Its 602-row custom JS view retains original
  long Unicode captions through native elision. Actual mouse input collapses and
  re-expands the section, refreshes a pending provider and cancels a large builder
  without replacing published rows. Existing version, keyboard cursor and
  retirement checks remain. The generated `build/graphics/gui-long-sidebar-heading.png`
  was inspected: long captions elide before the refresh button, Unicode glyphs
  display and the custom Outline heading retains its caption. Later matrix stages
  overwrite this path; this is not whole-product visual acceptance.
- Final `./scripts/test.ps1 -Mode all -Gui` exited 0, with source/runtime resources
  unchanged throughout execution (`build/workbench-chrome-acceptance-all.log`).
  Debug: 609 cases / 2,582,120 assertions; Release: 609 / 2,582,012, zero
  failed/skipped. Both modes passed real renderer/fonts/worker/viewport,
  editing/service/recovery/workspace/workbench GUI and TCP flows. The new native
  chrome and actual GUI control checks passed in both modes, together with
  12 package-helper and seven shaping-provenance checks. No new distribution was
  produced.
- Restored Debug with `./scripts/xmake.ps1 f -m debug -y`
  (`build/workbench-chrome-restore-debug.log`). Final
  `./scripts/format.ps1 -Check -Offline` passed 182 C++ / 13 JS files
  (`workbench-chrome-final-format-check.log`); `git diff --check` passed.

Header/sidebar-heading extraction is accepted for these checks. Remaining work
includes activity-bar and breadcrumb/status components, Outline content/provider/
action ownership, complete font adaptation, native Windows title-bar theme
integration, whole-product visual acceptance, physical installed-IME acceptance
and a new distribution. The requested architecture/UI revision remains in progress.

### Closed-buffer metadata lifetime fencing (2026-10-11)

The GUI now retains closed service IDs independently of its live documents.
`ClosedDocumentIds` uses a persistent measured SumTree of sorted, disjoint,
non-adjacent inclusive ranges; zero is the summary identity and maximum endpoint
is the associative composition. Adjacent closes coalesce. Lookup and insertion
seek cached endpoints without flattening and merge at most two neighbors. Retained
copies keep old trees unchanged. Sparse histories can grow with distinct ranges;
no fixed heap bound or eviction is claimed. IDs are canonical positive uint64
decimals, parsed without floating-point conversion. Service IDs are never reused
within the connection, including a legitimate same-path reopen.

Close handling prepares the closed-ID allocation before removing live state.
Metadata for a retired ID cannot recreate/activate a tab or deliver obsolete
creation/selection events to JS. Native background line/syntax/diagnostic reads
also recognize a real `-32002` response carrying the exact requested `data.id`:
it confirms retirement even before the close notification arrives. This does not
hide arbitrary missing-object, transport or edit errors. Existing dispatcher,
JavaScript policy and production graphics remain in place.

Windows evidence and retained failures:

- Initial `./scripts/xmake.ps1 build -y -j 2 astra-tests astra gui-smoke` passed
  (`build/closed-document-build-debug.log`).
  `./build/windows/x64/debug/astra-tests.exe
  '--test-case=presentation closed document*,presentation document metadata*'
  --no-version` passed two cases / 92,063 assertions, with 606 unrelated cases
  filtered out (`build/closed-document-focused-debug.log`). Tests cover 2,000
  differential rounds (seed `0xA57A97`), old-tree preservation, normalized cached
  summaries/balance, 1,000 disjoint ranges bridged into one, 10,000 consecutive
  closes compressed into one, duplicate closes, invalid input and uint64 edges.
- The first GUI run failed its existing full-metadata equality assertion:
  adding activation to the retained-read fixture forced a new viewport read.
  The closing scenario now has a separate command; the original assertion and
  command behavior are preserved. `build/closed-document-gui-debug.log` retains
  the failure. The next local GUI passed, while TCP exposed a real background
  read/close race (`build/closed-document-gui-fixed-debug.log` and
  `build/closed-document-tcp-fixed-debug.log`): a missing-document reply could
  arrive before the close notification and produce an action failure.
- Final formatting and the same targeted build passed
  (`build/closed-document-format-race.log`, `closed-document-build-race-debug.log`).
  `./build/windows/x64/debug/gui-smoke.exe` and the same binary with `--tcp`
  both exited 0 (`build/closed-document-gui-race-debug.log`,
  `closed-document-tcp-race-debug.log`). The channel proxy retains actual
  response/event bodies. It forwards a real deferred lines request after the
  service closes its buffer, deliberately delivers the resulting missing-ID
  error before the held close notification, then releases old metadata,
  selection and creation bodies. No tab resurrection or stale JS selection
  delivery occurs. A same-path reopen has a fresh ID and correct disk content;
  the original active document/tab set and subsequent editing workflows survive.
  These are production local/TCP/GPU integrations, not a mock editor.
- Final `./scripts/test.ps1 -Mode all -Gui` exited 0 with source/runtime resources
  unchanged throughout execution: Debug 608 cases / 2,548,728 assertions;
  Release 608 / 2,548,623, zero failed/skipped. Real renderer/fonts/worker/viewport,
  all editing/service/recovery/workspace/workbench GUI flows, TCP editing,
  12 package-helper and seven shaping-provenance checks passed.
  `build/closed-document-acceptance-all.log`. During this run the generated Debug
  captures `build/graphics/gui-editor.png` and `gui-chrome-command-center.png`
  were inspected; later matrix stages overwrite those paths. Inspection does
  not establish whole-product visual acceptance.
- Restored Debug with `./scripts/xmake.ps1 f -m debug -y`
  (`build/closed-document-restore-debug.log`). Final `./scripts/format.ps1 -Check
  -Offline` passed 180 C++ / 13 JS files (`build/closed-document-final-format-check.log`);
  `git diff --check` passed. Only documentation changed after full acceptance.

Live and closed-buffer metadata ordering now have acceptance evidence. Sidebar
outer chrome, headers and Outline extraction, complete font adaptation, native
title-bar theme integration and whole-product visual acceptance remain work to
finish, as do physical installed-IME interaction and a new distribution. Only
Windows evidence is claimed.

### Published asynchronous document metadata (2026-10-11)

Service document metadata now carries a positive canonical uint64 decimal-string
`metadataSequence`, separate from the text revision. The owner-local
`DocumentMetadataClock` retains published and prepared core tuples separately.
Repeated reads of unchanged state, its response and its legitimate notification
share a marker; selection-only changes and changed save/reload bindings advance
the marker. A discarded candidate cannot overwrite published state. Prepared
document/search/workspace-replace/LSP-rename/recovery/reload publication adopts
the exact staged marker. No document text is cached by this adapter; it retains
at most two tuples per record, forgets closed buffers, and uses existing bounded
notification batches and contiguous new IDs for unpublished records. This is
neither a text-edit counter nor a general heap/latency bound.

The native GUI strictly parses revision/sequence without floating-point conversion
and rejects regressing metadata for retained buffers before tab/document changes
or JS notification delivery. Equal-current legitimate events remain deliverable
after a read. Native Agent view state now exposes the accepted active document
metadata, matching the existing JS UI read payload. Production graphics and
dispatch/transport implementations are unchanged.

Windows evidence and retained failures:

- The first per-serialization marker design broke stable before/after metadata
  comparisons. The initial direct unit run also lacked real TCP/LSP permissions:
  604 cases, 548 passed / 56 failed; 2,454,829 assertions / 28 failed.
  `build/metadata-order-unit-initial-debug.log` retains the result and permission
  errors. This was not acceptance. Stable snapshot markers preserve existing
  equality checks; no old assertions were weakened.
- Earlier stable-marker focused and local/TCP GUI checks passed in
  `build/metadata-order-focused-final-debug.log`, `metadata-order-gui-debug.log`
  and `metadata-order-tcp-debug.log`. An earlier full run passed 605 cases in
  both modes (`build/metadata-order-acceptance-all.log`). Source review then
  identified that serialization of an unpublished clone could replace the live
  cached identity. The final published/prepared clock replaces that design;
  the final full run below supersedes the earlier acceptance for current source.
- The staged-clock test build failed on MSVC C4834: doctest's throwing expression
  discarded `[[nodiscard]]` results. Explicit void casts fixed the fixture without
  changing warnings or behavioral assertions. Retained failure:
  `build/metadata-order-staged-build-debug.log`. Final
  `./scripts/format.ps1 -Offline` formatted 180 C++ / 13 JS files
  (`build/metadata-order-staged-format-fixed.log`).
- `./scripts/xmake.ps1 build -y -j 2 astra-tests astra gui-smoke
  gui-workspace-smoke gui-recovery-smoke gui-services-smoke
  gui-presentation-smoke` passed after Debug configuration;
  `build/metadata-order-staged-build-fixed-debug.log`.
- `./build/windows/x64/debug/astra-tests.exe
  '--test-case=service metadata*,presentation document metadata*,application metadata*'
  --no-version` passed six cases / 15,154 assertions, 601 unrelated cases filtered
  out (`build/metadata-order-staged-focused-debug.log`). Tests retain old tuples,
  mutate returned JSON, exercise abandoned/published preparation, clean Save As
  binding changes, closed-record cleanup, decimal precision above 2^53 and
  exhaustion rejection. Shuffled metadata and staged-publication differential
  runs each cover 2,000 rounds (seeds `0xA57A95` and `0xA57A96`).
- Final `./scripts/test.ps1 -Mode all -Gui` exited 0 with source/runtime resources
  unchanged during execution. Debug and Release each passed 607 cases /
  2,460,623 assertions, zero failed/skipped. Real renderer/fonts/worker/viewport,
  editing/service/recovery/workspace/workbench GUI, TCP editing, 12 package-helper
  and seven shaping-provenance checks passed. The real local/TCP channel proxy
  delays actual RPC responses and selection notifications: stale same-text
  replies do not revert GUI state or reach JS listeners, while legitimate current
  events still arrive after a read. Original revision/selections are restored
  before the original editing workflows. No fake editor/backend establishes
  this result. `build/metadata-order-staged-acceptance-all.log`.
- Restored Debug using `./scripts/xmake.ps1 f -m debug -y`
  (`build/metadata-order-restore-debug.log`). Final `./scripts/format.ps1 -Check
  -Offline` passed 180 C++ / 13 JS files (`build/metadata-order-final-format-check.log`);
  `git diff --check` passed. Only documentation changed after full acceptance.

This establishes live-buffer metadata ordering. Removing a buffer still drops
the client's retained comparison state, so late replies can recreate closed
documents; lifetime fencing remains required. Sidebar outer chrome, headers and
Outline extraction, complete font adaptation and whole-product visual acceptance,
physical installed-IME interaction and a new distribution remain incomplete.
Only Windows evidence is claimed.

### Shared native indexed view rows (2026-10-11)

Explorer, Search, Git and registered sidebar views now use the same native
`ViewRows` component. It borrows immutable indexed rows and JS-supplied labels,
icons, badges and selection descriptors. Provider content, row actions, refresh
and publication remain JS policy. Window code supplies current focus/pointer
state and a narrow file-icon resolver rather than embedding the row painter.
`ViewRowsLayout` supplies one clipped, half-open row range to painting, pointer
hit testing, keyboard paging and visible file-icon discovery. Painting visits
only that range through the measured tree; it does not flatten the source.

Row height now reserves the configured UI font's line height and padding while
preserving the default 24-unit minimum. Labels retain native Unicode grapheme
elision and the bounded paragraph cache. Focused/inactive selection, JS-selected
rows, hover, badges, group chevrons and projected icons use shared theme roles.
Canvas clip/transform/save state is restored even when an icon resolver throws;
collapsed, zero-area and wholly clipped-out views prepare no captions. Agent
sidebar state exposes `viewport.{first,end,page,painted}`; `painted` is the last
completed frame's component count, while the range describes current geometry.

Windows evidence and retained failures:

- Initial `./scripts/xmake.ps1 build -y -j 2 astra-tests viewport-layout-tests
  gui-presentation-smoke astra` passed (`build/view-rows-build-debug.log`). The
  first row-boundary run failed two assertions: float rounding admitted a
  zero-height final row and disagreed at its preceding boundary. The corrected
  layout checks physical rounded edges and bounded adjacent-row hit correction;
  `build/view-rows-unit-debug.log` retains the failure. The next run failed 31
  fixture midpoint samples: a positive one-ULP final span's computed midpoint
  rounded to its excluded bottom. The fixture now samples a representable point
  inside the rectangle and asserts containment, retaining all height/top/bottom
  checks; `build/view-rows-unit-corrected-debug.log` retains that failure.
- `build/view-rows-build-final-debug.log` records a test compilation failure:
  this pinned doctest's `CAPTURE` macro accepts one expression. Split diagnostic
  captures into separate calls without changing warnings or assertions. Final
  formatting/build passed in `build/view-rows-format-fixed.log` and
  `build/view-rows-build-fixed-debug.log`.
- `./build/windows/x64/debug/astra-tests.exe '--test-case=presentation*row*'
  --no-version` passed six cases / 181,860 assertions, with 596 unrelated cases
  filtered out (`build/view-rows-unit-fixed-debug.log`). Coverage includes 2,000
  fractional geometry rounds (seed `0xA57A94`), half-open boundaries, partial
  rows, tail clamping, maximum index values, zero area and invalid/precision-lost
  geometry. `./build/windows/x64/debug/viewport-layout-tests.exe --rows` passed
  actual native-font Unicode pixels at four DPI scales, exact seven-row access
  into a 5,000-row source, selection/hover/theme pixels, warm reuse, retained tree
  immutability and canvas failure ownership (`build/view-rows-native-fixed-debug.log`).
- `./build/windows/x64/debug/gui-presentation-smoke.exe` exited 0. The real
  602-row JS custom view verifies visible-range paint counts and the final
  intersecting row's SDL pointer action, then exercises folding, indexed paging,
  cancellation, publication and JS action identity. Styled inputs also check
  row height against the configured native UI font. `build/view-rows-gui-debug.log`.
- Final `./scripts/test.ps1 -Mode all -Gui` exited 0 with source/runtime resources
  unchanged during execution: Debug 602 cases / 2,445,612 assertions; Release
  602 / 2,445,550, zero failed/skipped. Real renderer/fonts/worker/viewport and
  native row checks, all editing/service/recovery/workspace/workbench GUI flows,
  TCP editing, 12 package-helper and seven shaping-provenance checks passed.
  `build/view-rows-acceptance-all.log`. Debug and Release GPU captures
  `build/graphics/gui-style-large.png` and `gui-custom-view.png` were visually
  inspected; later acceptance runs overwrite those capture paths.
- Restored Debug with `./scripts/xmake.ps1 f -m debug -y`
  (`build/view-rows-restore-debug.log`). Final `./scripts/format.ps1 -Check
  -Offline` passed 176 C++ / 13 JS files (`build/view-rows-final-format-check.log`);
  `git diff --check` passed. Documentation alone changed after full acceptance.

Sidebar outer chrome, headers and Outline still have separate native window
code. This increment does not establish whole-product visual acceptance or
complete font adaptation for every chrome component. Same-text selection
metadata ordering, physical installed-IME interaction and a new distribution
also remain incomplete. Only Windows evidence is claimed.

### JS Explorer reveal and retained selection (2026-10-11)

Explorer automatic reveal is now JS policy, enabled by the declarative
`explorer.autoReveal` default. Activation and successful provider publication
capture the active document and native view version through Promise APIs.
Superseded requests are cancelled; native activity/version fences reject stale
continuations, including A-to-B-to-A. The activity identity also changes when
the active path changes or the active document closes. Automatic reveal leaves
sidebar visibility, active view and keyboard focus unchanged. Explicit
`ui.revealActiveFile` shows/focuses Explorer even when automatic reveal is off.
Merged service preferences and explicit client overrides are read through a
bounded generic preference-section API.

The generic native row-reveal primitive expands only ancestors and preserves
the target's own fold and unrelated folds. It prepares a persistent projection
before publication, replaces only the outermost hidden subtree, and retires the
old projection off the input loop. Work is bounded by depth 64, 4,096 retained
folds and measured tree seeks; it does not flatten the full row source. Missing
identities, invalid pages and retirement backpressure leave fold/cursor/scroll
state intact. Fold generation changes invalidate a pending refresh projection.
Optional async `handlers.published` callbacks run after successful publication
and guarded completion metadata; cancelled completion cannot invoke the hook.

Actual screenshot inspection exposed a presentation defect: a retained cursor
was only highlighted while focused. Native rows now retain selection after
focus leaves the widget. JS theme role `inactiveSelection` supplies the weaker
background; focused selection and its focus accent retain their separate roles.

Windows evidence:

- Initial `./scripts/xmake.ps1 build -y -j 2 astra-tests gui-presentation-smoke astra`
  rejected the GUI fixture's two-argument call to the name-only
  `execute_ui_command` interface. Replaced it with the existing JS `ui.closeTab`
  command; no production interface was widened. Failure retained in
  `build/explorer-reveal-build-debug.log`.
- Before the final selection-paint correction,
  `./scripts/test.ps1 -Mode all -Gui` exited 0: Debug 600 cases / 2,355,579
  assertions; Release 600 / 2,355,466, zero failed/skipped. Native renderer,
  font/worker/viewport checks, GUI editing/services/recovery/workspace/workbench,
  TCP editing, 12 package-helper and seven shaping-provenance checks passed.
  `build/explorer-reveal-acceptance-all.log`. This precedes the paint correction;
  the final affected checks below establish that correction's acceptance.
- Final `./scripts/xmake.ps1 build -y -j 2 astra-tests gui-presentation-smoke astra`
  passed in both configurations. Then
  `./build/windows/x64/{debug,release}/astra-tests.exe --no-version` passed:
  Debug 600 / 2,355,513; Release 600 / 2,355,501, zero failed/skipped.
  `build/explorer-reveal-visual-build-{debug,release}.log` and
  `build/explorer-reveal-visual-unit-{debug,release}.log`.
  Coverage includes 400 tree fold/reveal differential rounds (seed `0xA57A93`),
  immutable retained snapshots, all 64 ancestors, unrelated/target folds,
  publication invalidation, retirement backpressure, obsolete async reads,
  disabled/manual policy and cancelled publication hooks.
- Final `./build/windows/x64/{debug,release}/gui-presentation-smoke.exe` exited 0
  in both modes. The real 5,005-row workspace verifies hidden-folder expansion,
  hidden-sidebar/focus preservation, refresh, native activity/version rejection,
  A-to-B-to-A, disabled automatic policy, explicit reveal and close fallback.
  It also decodes actual GPU PNGs and checks the target row's exact RGBA pixels
  against the focused and inactive JS theme roles.
  `build/explorer-reveal-visual-gui-{debug,release}.log`;
  `build/graphics/gui-explorer-focused-reveal.png` and
  `build/graphics/gui-explorer-auto-reveal.png` were visually inspected.
- Debug restored with `./scripts/xmake.ps1 f -m debug -y` before the final
  Debug build/tests; `build/explorer-reveal-restore-debug.log`. Source/runtime
  resources stayed unchanged throughout each build/test execution.
- `./scripts/format.ps1 -Check -Offline` passed 172 C++ / 13 JS files;
  `build/explorer-reveal-commit-format-check.log`. `git diff --check` passed.

Native sidebar component extraction, cohesive whole-product visual acceptance,
same-text selection metadata ordering, physical installed-IME interaction and
a new distribution remain incomplete. This increment claims Windows only.

### Persistent local glyph splicing and font identity (2026-10-11)

Native logical glyphs now live in a measured persistent `GlyphSequence`, with
at most 256 glyphs per immutable backing block. Splits/concatenation preserve
unchanged interior nodes; cached UTF-8 extents relocate retained clusters without
rewriting old raw arrays. Sliced leaves charge their whole backing block, including
large clusters spanning several blocks and invisible source extents. Snapshot
admission remains 1,024 runs / 4 MiB of accounted payload, excluding capacities,
tree/container, native font/paragraph and GPU heaps.

Local edits use scalar-aligned differences, old/new ICU grapheme boundaries and
genuine old/new HarfBuzz unsafe-to-concat certificates. An extra cluster exposes
the right cut's native flag; failed certificates expand the window geometrically
for at most eight attempts before complete shaping. Unchanged surrounding context,
font geometry, bidi/script/language/tracking and whole-item feature applications
must match. Partial feature ranges and absent certificates retain full shaping.
No ASCII or whitespace approximation establishes a reusable boundary.

Background owners now fingerprint sorted native SFNT tables, variation positions,
style/synthetic attributes and glyph metrics with incremental SHA-256. A private
engine memo retains at most 128 identities, scans at most 128 MiB in total and
uses a fixed 64 KiB table buffer. Unsupported/failed/bounded-out reads cannot
certify cross-owner reuse. Snapshots retain digest values, never native fonts;
interactive caption owners continue using native instance IDs without table scans.
Font fingerprinting and native calls have the existing worker's soft cancellation
and deadline semantics: input stays responsive, and a running native call drains.

Windows evidence and retained failures:

- Initial `./scripts/xmake.ps1 build -y -j 2 astra-core-tests text-layout-tests`
  rejected the new tree summary's missing equality operator; added value equality
  without changing warning settings. `build/glyph-splice-initial-build-debug.log`.
- `./build/windows/x64/debug/astra-core-tests.exe '--test-case=content hash*' --no-version`
  passed three cases / 4,108 assertions. Standard vectors and 2,050 binary sizes
  compare streaming/padding against independent Windows BCrypt; seed `0xA57A256`.
  `build/glyph-splice-hash-tests-debug.log`.
- The first real Unicode differential failed at seed `0xA57A92`, round 18:
  deleting Indic consonants left a proposed cut inside the new combining cluster.
  Geometry matched but native break/concat flags differed. Added old/new ICU
  grapheme certification; retained raw-field comparison, independent native pixels,
  both caret affinities and hit testing. Temporary full-shaping diagnostics were
  removed before acceptance. `build/glyph-splice-native-debug.log`,
  `build/glyph-splice-diagnostic-native-debug.log` and
  `build/glyph-splice-final-native-debug.log` retain failure/final evidence.
- `./scripts/xmake.ps1 build -y -j 2 astra-tests text-layout-tests viewport-layout-tests astra`
  passed; `build/glyph-splice-final-build-debug.log`. Native tests include 500
  persistent block edits (seed `0xA57A91`), retained snapshots, bounded chunks,
  multi-block clusters, whole-block charging, seven Unicode/script/tab inputs at
  three DPI values, 80 scalar edits, exact raw fields and cross-owner font identity.
- `./build/windows/x64/debug/viewport-layout-tests.exe --coordinates` passed
  actual libuv publication, cancellation and old-paragraph lifetime checks;
  `build/glyph-splice-final-worker-debug.log`. In both full acceptance modes,
  the 12,000-byte ASCII replacement shaped two bytes, shared 11,745 glyphs and
  certified two different native font instances. This is a measured test case,
  not a guarantee for all scripts/fonts or an end-to-end latency benchmark.
- `./scripts/test.ps1 -Mode all -Gui` exited 0 with source/runtime resources
  unchanged during acceptance. Debug: 594 cases / 1,736,095 assertions;
  Release: 594 cases / 1,736,043 assertions, zero failed/skipped. Both passed
  native renderer/font/viewport/input checks, all editing/service/recovery/
  workspace/presentation GUI workflows, TCP editing, 12 package-helper checks
  and seven shaping-provenance checks. `build/glyph-splice-acceptance-all.log`.
- An additional actual Latin-ligature fixture first failed because native Cambria
  did not merge `ffi`; Calibri does. Subsequent fixtures incorrectly demanded
  partial shaping despite all 720 captured Calibri cluster starts being unsafe.
  Those failures remain under `build/glyph-splice-ligature-*.log`. The final
  fixture requires actual glyph merging, complete raw parity for ligature creation/
  deletion and kerning, independent native callback byte counts, immutable old
  snapshots, and complete shaping when no native certificate exists. Production
  logic is unchanged. Updated `text-layout-tests` passed both modes;
  `build/glyph-splice-ligature-final-build-{debug,release}.log` and
  `build/glyph-splice-ligature-final-native-{debug,release}.log`.
- Restored Debug with `./scripts/xmake.ps1 f -m debug -y`;
  `build/glyph-splice-restore-debug.log`. Final
  `./scripts/xmake.ps1 build -y -j 2 text-layout-tests astra` passed;
  `build/glyph-splice-commit-build-debug.log`. The final native executable and
  `astra-tests.exe '--test-case=content hash*' --no-version` passed again
  (three hash cases / 4,108 assertions); `build/glyph-splice-commit-native-debug.log`
  and `build/glyph-splice-commit-hash-debug.log`.
  `./scripts/format.ps1 -Check -Offline` passed 172 C++ / 13 JS files, and
  `git diff --check` passed; `build/glyph-splice-commit-check.log`.

Text comparison, grapheme segmentation and native output materialization still
process whole context; this does not establish logarithmic total paragraph layout.
Fonts without native certificates may reshape entirely. Further native component
extraction, cohesive whole-product visual acceptance, Explorer active-path reveal,
same-text selection metadata ordering, physical installed-IME interaction and a
new distribution remain incomplete. Only Windows evidence is claimed.

### Native glyph snapshots and exact run replay (2026-10-11)

The real pinned Skia/HarfBuzz path now exposes logical-order glyphs, UTF-8
clusters, advances/offsets and genuine unsafe-to-concat metadata through a
per-font-collection adapter. A reviewed patch and per-blob manifest retain the
upstream pins; build preparation rejects changed patch/source, partial application
and unrelated edits. Build manifests, xmake and runtime provenance require the
adapter. This is the same production Graphite/Dawn/SkParagraph stack.

`GlyphSource` retains immutable measured run snapshots without native fonts or
paragraphs. Exact full-context/typeface/font/bidi/script/language/feature/tracking
matches share raw glyph arrays and skip actual shaping; changed text or a worker's
different typeface ID conservatively reshapes. Capture admits 1,024 runs / 4 MiB
of accounted run/text/feature/glyph payload. Native allocation, vector capacity,
tree/container and GPU heaps are outside that number. Admission bypass still
shapes the complete native run. Source/job/viewport admission now charges glyph
payload; the old opaque native paragraph cache is replaced by Astra's existing
bounded paragraph LRU so a native hit cannot omit shaping metadata. Collectors
detach on success and exceptions before any paragraph transfer.

Current Windows evidence:

- `./scripts/build-graphics.ps1 -Configuration Debug -Jobs 4` compiled and
  installed the actual adapter; `build/glyph-graphics-debug.log`.
- First application compile rejected narrowing native `size_t` feature ranges
  to 32-bit fields. Retained native range width without casts or weakened
  warnings; `build/glyph-build-debug.log`.
- `./scripts/format.ps1 -Offline` and focused native/application builds passed.
  Latest focused build: `./scripts/xmake.ps1 build -y -j 2 text-layout-tests viewport-layout-tests astra`;
  `build/glyph-capacity-format.log`, `build/glyph-capacity-build-debug.log`.
- The first native replay fixture made only a paint change, which SkParagraph's
  opaque cache had already handled without invoking the collector. Its skipped
  callback made the raw replay assertion fail (`build/glyph-native-debug.log`).
  The fixture now also changes strut geometry independently of glyph inputs.
  Complete paragraphs are retained only by the bounded Astra LRU, keeping
  collector snapshots complete.
- The first viewport check used an old 700-byte budget that could not admit
  new glyph payload (`build/glyph-viewport-debug.log`). The entry-eviction fixture
  now uses 4,096 bytes; its separate byte-capacity fixture still derives the
  exact measured entry size and checks exact eviction/accounting.
- `./build/windows/x64/debug/text-layout-tests.exe` passed actual raw glyph
  replay and independent complete-shaping pixels, width/height, both caret
  affinities and hit positions. Cases include ligatures, Arabic/Hebrew, native
  CJK/emoji fallback, combining/ZWJ, tabs and DPI 1/1.5/2; font/context changes
  preserve parity and old snapshots. A 180,000-byte input exceeds capture
  capacity without skipping native shaping; `build/glyph-capacity-native-debug.log`.
- `./build/windows/x64/debug/viewport-layout-tests.exe` passed all native
  viewport, worker, editable-input, IME, label, field, tab, cancellation and
  admission checks. The retained paragraph's glyph snapshot remains immutable
  through a real libuv-worker local edit and transfer;
  `build/glyph-capacity-viewport-debug.log`.
- `.tools/graphics/python/python.exe tests/skia_patch_tests.py` passed seven
  Windows-run provenance cases: idempotent known patch; changed patch, paired
  patch/manifest changes, wrong revision, unrelated source, altered adapter and
  partial application are rejected before mutation. Fixture Git metadata is
  isolated; the actual graphics build above establishes native integration.
  `build/glyph-final-provenance-tests.log`. The standard acceptance script now
  runs these alongside existing package-helper checks.

The first full `./scripts/test.ps1 -Mode all -Gui` passed Debug 591 cases /
1,731,998 assertions and Release 591 cases / 1,731,903 assertions, all native and
GUI checks and both TCP editing runs, then failed its package-helper fixture:
the isolated dependency-patch guard had not copied the newly pinned Skia patch.
Updated that fixture's inputs, retaining its production hash-rejection check;
`build/glyph-acceptance-all.log` retains the complete failed run.

Review also identified exception-disabled Skia frames at the callback boundary.
The Astra collector is now `noexcept`, retains the first error, retires subsequent
callbacks and rethrows outside native layout before publication. The native
fixture injects a callback error and verifies exact error reporting and retirement.
Upstream native out-of-memory recovery is not established. Acceptance of this
follow-up is pending.

The adapter now explicitly aborts before native run-buffer emission on collector
failure and propagates that failure out of the SkParagraph shaping pass.
`./build/windows/x64/debug/text-layout-tests.exe` passed an actual SkParagraph
failure injection: exactly one failing native callback, no subsequent emission,
the original error retained/reported, and all subsequent glyph/pixel/Unicode
checks pass. `build/glyph-abort-build-debug.log` and
`build/glyph-abort-native-debug.log` record the updated Debug build and run.
The package-helper fix passed all 12 checks (`build/glyph-error-package-tests.log`).

Final Windows evidence superseding that pending follow-up:

- Updated Debug and Release graphics builds passed with the failure-propagating
  adapter; `build/glyph-error-graphics-debug.log` and
  `build/glyph-abort-graphics-release.log`.
- Final `./scripts/test.ps1 -Mode all -Gui` completed with exit code 0 using
  unchanged native source/runtime resources. Debug: 591 cases / 1,732,029
  assertions; Release: 591 cases / 1,731,898 assertions, zero failed/skipped.
  Both modes passed actual native failure injection and raw replay, all renderer/
  font/viewport/input/editing/service/recovery/workspace/presentation GUI workflows,
  the 5,005-file/640-tab scenarios and TCP editing. All 12 package-helper checks
  and seven shaping-provenance checks passed. `build/glyph-final-acceptance-all.log`.
- After every acceptance process exited, restored Debug configuration with
  `./scripts/xmake.ps1 f -m debug -y`; `build/glyph-restore-debug.log`.
  Final `./scripts/format.ps1 -Check -Offline` passed 165 C++ / 13 JS files and
  `git diff --check` passed; `build/glyph-final-check.log`.

Local-edit glyph splicing, stable cross-worker font equivalence, further
native component extraction, whole-product visual acceptance and a new package
remain incomplete. No non-Windows validation is claimed.

### Persistent native text coordinate snapshots (2026-10-11)

`TextSource` now couples immutable materialized UTF-8 with the measured persistent Rope.
`TextLayout` uses this index for UTF-8/UTF-16 conversion instead of dense
per-byte/per-code-unit tables. Interior surrogate columns snap only through the
explicit layout adapter; document edit conversion remains strict. ICU grapheme
snapping, bidi, wrapping, fallback, tabs and native paragraph painting are retained.

Cache misses may seed a worker with the same row's prior source from the bounded
paragraph LRU. Scalar-aligned text differences replace only the changed Rope
extent; unchanged coordinate subtrees survive. Workers never borrow published
native paragraphs/fonts. Source comparison, grapheme segmentation and native
glyph shaping still process a complete row; actual incremental glyph reuse is
not implemented by this increment. Cache/job accounting includes flat-text and
Rope payload bytes, not tree/container/font/paragraph/GPU heaps.

Current Windows evidence:

- First build failed in the new doctest fixture because `string_view` diagnostic
  output needed a complete `<ostream>` and exception assertions discarded
  `[[nodiscard]]` results. Added the include and explicit void casts, without
  weakening warning settings. Log: `build/text-source-build-debug.log`.
- `./scripts/format.ps1 -Offline` and
  `./scripts/xmake.ps1 build -y -j 2 astra-tests text-layout-tests viewport-layout-tests astra`
  passed in Debug; logs: `build/text-source-final-format.log` and
  `build/text-source-final-build-debug.log`.
- `./build/windows/x64/debug/astra-tests.exe '--test-case=text source*,rope*' --no-version`
  passed 14 cases / 224,830 assertions. Four new source cases compare independent
  flat Unicode coordinates, surrogate interiors across 1,024-byte chunk edges,
  250 randomized scalar edits (seed `0xA57A82`), retained snapshots, shared
  subtree identities and atomic malformed-UTF-8 failures. Log:
  `build/text-source-unit-debug.log`.
- `./build/windows/x64/debug/text-layout-tests.exe` passed actual native fonts,
  Unicode/bidi caret/hit/selection, tabs, wrap, DPI and GPU text. Log:
  `build/text-source-native-debug.log`.
- `./build/windows/x64/debug/viewport-layout-tests.exe --coordinates` passed
  actual libuv-worker publication/cancellation and cold native pixel parity.
  A local Unicode edit retains more than 2,048 bytes of shared prefix and suffix
  coordinate subtrees; the old paragraph continues painting unchanged during
  the worker job. The replacement's pixels, caret and hit testing match a fresh
  layout. These checks also run through the default viewport acceptance entry
  point. Log: `build/text-source-worker-debug.log`.
- Final `./scripts/test.ps1 -Mode all -Gui` completed with exit code 0 using
  unchanged native source/runtime resources. Debug: 591 cases / 1,732,121
  assertions; Release: 591 cases / 1,731,950 assertions, zero failed/skipped.
  Both modes ran the new source tests and worker coordinate-reuse checks, all
  renderer/font/viewport/input/editing/service/recovery/workspace/presentation
  GUI workflows, TCP editing and all 12 package-helper invariants. Log:
  `build/text-source-acceptance-all.log`.
- After acceptance exited, `./scripts/xmake.ps1 f -m debug -y` restored Debug
  configuration (`build/text-source-restore-debug.log`). Final
  `./scripts/format.ps1 -Check -Offline` passed 162 C++ / 13 JS files
  (`build/text-source-final-check.log`), and `git diff --check` passed.

Further native component extraction, row-internal incremental glyph layout,
whole-product visual acceptance and a new distribution remain incomplete.
No non-Windows validation is claimed.

### Asynchronous Quick Open and provider pickers (2026-10-11)

`Ctrl+P` now uses a JS-owned fuzzy file provider instead of literal path entry. The
generic Promise-based `astra.ui.pick` accepts an asynchronous query provider,
with per-query AbortSignals, stale-result rejection, original-item completion,
local error/retry and cancellation of ignored-signal providers. Native code
owns measured result indexes, presentation/input and atomic generation checks.
Loading contributes to window idle and prevents Enter/pointer acceptance of
previous rows. Visible result icons share JS theme policy.

The JS index uses service pages, caps retained paths at 100,000 / 8 MiB of UTF-8
identity bytes, yields every 32 entries or 4,096 string units, and publishes at
most 128 rows / 60,000 label bytes. These are admission/accounting limits, not
heap or frame-time guarantees. Partial filesystem results are reported; explicit
path entry remains available in the command palette. No watcher-backed index is
claimed.

Current Windows evidence:

- `./scripts/format.ps1 -Offline` passed 159 C++ / 13 JS files.
- `./scripts/xmake.ps1 build -y -j 2 astra-tests` passed. Focused
  `./build/windows/x64/debug/astra-tests.exe '--test-case=*quick open*,*dynamic picker*,*picker preserves static*' --no-version`
  passed five cases, zero failures. Tests cover retained native snapshots and
  atomic invalid publication, cancellation of obsolete providers, retirement of
  late host mutations, error/recovery, paged 5,003-file Unicode lookup and
  cancellation draining a late service operation identity, static item/data
  compatibility and rejection of obsolete dynamic selections. Log:
  `build/quick-open-final-policy-tests-debug.log`.
- Initial GUI fixture compilation referenced nonexistent `WorkbenchViewState`
  document/status fields. Corrected it to use the production service identity
  query and `status_text()`; log: `build/quick-open-gui-build-debug.log`.
- First GUI run incorrectly expected `path` in `document.get`; corrected to
  `workspace.documents`, preserving the actual-file identity assertion. Log:
  `build/quick-open-gui-debug.log`.
- Second GUI run passed all new Quick Open/provider checks, then failed the
  existing save-as content check: the inserted quick-open/close fixture had
  changed the active document. Restored the original captured document before
  the save workflow without weakening its existing content assertion. Log:
  `build/quick-open-gui-final-debug.log`.
- Rebuilt with `./scripts/xmake.ps1 build -y -j 2 gui-presentation-smoke astra-tests astra`;
  `./build/windows/x64/debug/gui-presentation-smoke.exe` passed the complete
  actual SDL3 / Graphite / Dawn presentation workflow. It includes 5,005 indexed
  entries, fuzzy lookup beyond 4,096 rows, same-batch typing / pending Enter
  rejection, pending pointer rejection, local provider error/recovery, late
  completion isolation, Escape, existing same-batch save-as, large text/custom
  views and 640-tab pointer/drag/close behavior. Log:
  `build/quick-open-gui-restored-debug.log`. The GPU capture
  `build/graphics/gui-quick-open.png` was inspected for query, matched file,
  clipping and file icon. Whole-product visual acceptance remains outstanding.
- Final `./scripts/test.ps1 -Mode all -Gui` completed with exit code 0 using
  unchanged native source/runtime resources. Debug: 587 cases / 1,617,282
  assertions; Release: 587 cases / 1,617,253 assertions, zero failed/skipped.
  Both configurations passed the new Quick Open/provider workflow and the
  complete renderer/font/viewport/input/editing/service/recovery/workspace/
  presentation GUI workflows, including TCP editing. All 12 package-helper
  invariants passed. Log: `build/quick-open-acceptance-all.log`.
- After acceptance exited, `./scripts/xmake.ps1 f -m debug -y` restored Debug
  configuration (`build/quick-open-restore-debug.log`). Final
  `./scripts/format.ps1 -Check -Offline` passed 159 C++ / 13 JS files
  (`build/quick-open-final-check.log`), and `git diff --check` passed.

Further native component extraction, row-internal incremental glyph layout,
whole-product visual acceptance and a new distribution remain incomplete.
No non-Windows validation is claimed.

### Shared native tab bar presentation (2026-10-11)

`TabBar` now owns tab caption preparation, font-generation tracking, overflow
scroll/reveal, painting and caption/close/navigation hit testing. Window code
supplies the persistent `TabIndex`, dirty state and already classified icon
descriptors, then invokes the existing activation/reorder/close workflows. The
component contains no document, filesystem or service policy. Theme and file-icon
associations remain JS-owned. Pointer dispatch and painting use the same measured
geometry; pointer dispatch performs no new caption measurement. The existing
eight-measurement / four-millisecond dispatch budget and visible-caption priority
remain. Palette-only changes update colors without invalidating measured widths.
Borrowed canvas state is restored by RAII, including callback failures.

Windows evidence:

- `./scripts/xmake.ps1 build -y -j 2 viewport-layout-tests gui-presentation-smoke gui-smoke astra`
  passed in Debug; log: `build/tab-bar-build-debug.log`.
- First `viewport-layout-tests.exe --tabs` rejected the new fixture with
  `Invalid UI text geometry`: its `WorkbenchStyle` supplied the UI font list but
  omitted the required monospace font list. Corrected the fixture without
  weakening production style validation. Log: `build/tab-bar-viewport-debug.log`.
- `./scripts/format.ps1 -Offline` and
  `./scripts/xmake.ps1 build -y -j 2 viewport-layout-tests gui-presentation-smoke`
  passed; logs: `build/tab-bar-final-format.log` and
  `build/tab-bar-final-build-debug.log`. Focused
  `./build/windows/x64/debug/viewport-layout-tests.exe --tabs` passed actual native
  fonts at DPI 1 / 1.5 / 2: clipped Unicode/emoji/combining pixels, eight warm
  repaints with zero additional preparation, bounded measurement, separate caption/
  close/overflow identities, foreground changes without generation invalidation,
  cold independent-presentation pixel parity, atomic invalid bounds, extreme
  scroll steps without overflow and canvas save/clip/transform preservation on
  callback failure. Log: `build/tab-bar-final-viewport-debug.log`.
- `./build/windows/x64/debug/gui-presentation-smoke.exe` passed actual Windows
  SDL3 / Graphite / Dawn workbench checks, including 640 real opened files,
  bounded font refresh and warm reuse. New pointer checks navigate one identity
  back/forward without activation or new measurement, activate a caption, drag
  reorder against a flat identity oracle and close the dragged active identity.
  Log: `build/tab-bar-gui-debug.log`. Actual GPU capture
  `build/graphics/gui-tabs-component.png` was inspected: the reordered active
  caption and overflow controls remain visible with separate close targets.
  This is component verification, not whole-product visual acceptance.
- `./scripts/test.ps1 -Mode all -Gui` completed with exit code 0 using unchanged
  native source/resources. Debug: 582 cases / 1,616,777 assertions; Release:
  582 cases / 1,616,761 assertions, zero failed/skipped. Both configurations ran
  the new native tab-bar pixel/identity/failure checks through the default viewport
  entry point and passed the 640-tab pointer workflow. All renderer/font/viewport/
  input/editing/service/recovery/workspace/presentation GUI workflows and TCP
  editing passed, as did 12 package-helper invariants.
  Log: `build/tab-bar-acceptance-all.log`.
- After acceptance exited, `./scripts/xmake.ps1 f -m debug -y` restored Debug
  configuration; log: `build/tab-bar-restore-debug.log`.
- Final `./scripts/format.ps1 -Check -Offline` passed 159 C++ / 13 JS files and
  `git diff --check` passed; log: `build/tab-bar-final-check.log`.

Further native component extraction, row-internal incremental glyph layout,
whole-product visual acceptance and a new distribution remain incomplete.
No non-Windows validation is claimed.

### Cached asynchronous document IME preparation (2026-10-11)

Document preedit now uses the existing shared `TextInputLayout` / libuv layout
queue instead of shaping a new paragraph on every paint. Cold candidates of at
least 4096 UTF-8 bytes return a progress underline while the owner loop continues;
short candidates and exact cache hits remain immediate. The key includes font,
DPI, line height, foreground and automatic direction. Candidate updates accept
at most 16 KiB of single-line UTF-8 and validate/allocate before replacing the
retained preview. Preedit never changes document text, selections or history.

One clearing path retires candidate jobs/cache on empty updates, Escape,
document activation/active-document closure, input focus changes, window focus
loss and shutdown. Explicit cancellation dismisses native composition through
the narrow SDL adapter. Committed text clears only the preview: it does not
cancel an OS composition started after that packet. Pending ticketless work is
included in native idle detection. View state reports pending/drawn status,
candidate byte count and the actual paragraph foreground for GUI observability.
Preparation exceptions report an IME preview error and paint an error marker
without closing the window through its fatal renderer boundary. Failed job keys
do not resubmit during repaint; changing the candidate/style or clearing the
preview permits another preparation.

Windows evidence:

- `./scripts/format.ps1 -Offline` passed 157 C++ / 13 JS files;
  log: `build/document-ime-format.log`.
- `./scripts/xmake.ps1 build -y -j 2 gui-smoke astra` passed in Debug;
  log: `build/document-ime-build-debug.log`.
- `./build/windows/x64/debug/gui-smoke.exe` passed the real SDL3 / Graphite /
  Dawn local editing workflow; log: `build/document-ime-gui-debug.log`.
  New checks send 13,000-byte CJK/emoji/combining candidates, require cold
  asynchronous publication and one worker submission, then verify eight warm
  paints and an identical candidate submit no additional shaping. Theme changes
  replace foreground geometry. Candidate cancellation, document switch/close
  and focus loss drain obsolete work. Oversized, malformed UTF-8 and multiline
  candidates retain the prior preview and entire authoritative document state.
- First `./scripts/test.ps1 -Mode all -Gui` passed with exit code 0. Debug:
  582 cases / 1,616,808 assertions; Release: 582 cases / 1,616,795 assertions.
  All GUI/TCP workflows and 12 package-helper invariants passed.
  Log: `build/document-ime-acceptance-all.log`. Subsequent review found that
  native preedit preparation failures reached the fatal renderer boundary;
  corrected the exception boundary.
- Final `./scripts/test.ps1 -Mode all -Gui` passed with exit code 0 using
  unchanged native source/resources. Debug: 582 cases / 1,616,913 assertions;
  Release: 582 cases / 1,616,774 assertions, zero failed/skipped. All renderer,
  font, viewport, input, editing/service/recovery/workspace/presentation GUI
  workflows and TCP editing passed, as did 12 package-helper invariants.
  Log: `build/document-ime-acceptance-all-final.log`. Existing real-worker
  editable-layout checks cover failed native style, retained cache, terminal
  failure/no resubmission, corrected style, cancellation and destruction.
- Actual GPU captures `build/graphics/gui-document-ime.png` and
  `build/graphics/gui-document-ime-tcp.png` were inspected: the
  candidate's CJK/emoji/combining glyphs and underline stay clipped to the document
  viewport, with the changed foreground. This validates injected SDL events,
  not a physical input-method candidate window or scalar-range candidate styling.
- After final acceptance exited, `./scripts/xmake.ps1 f -m debug -y` restored
  Debug configuration (`build/document-ime-restore-debug.log`). Final
  `./scripts/format.ps1 -Check -Offline` passed 157 C++ / 13 JS files and
  `git diff --check` passed; log: `build/document-ime-final-check.log`.

Further native component extraction, row-internal incremental glyph layout,
whole-product visual acceptance and a new distribution remain incomplete.
SDL candidate start/length styling is not implemented by this painter; physical
IME interaction remains unverified.
No non-Windows validation is claimed.

### JS file icon policy and bounded descriptors (2026-10-10)

Shipped `fileIcons` preferences now own filename, longest compound-extension and
directory-name associations in JS. Visible tabs and Explorer rows share the same
resolver; duplicate C++/Explorer extension tables were removed. C++ translates
validated glyph/color descriptors into drawing primitives. Palette roles use the
current UI colors without rerunning associations. Custom view rows can request
file policy with `iconPath` and their directory `group` flag.

Async `astra.ui.icons.read/resolve/update/replace` expose effective theme snapshots,
bounded resolution, merge patches and complete replacement. Invalid candidates
and stale concurrent generations reject before publication. Paths and patches are
captured before awaiting; cancellation of a read prevents a later update. Complete
replacement also removes rules introduced after the saved snapshot. Live overrides
survive a later service-startup preference reply and do not persist to disk.

The native LRU holds at most 2,048 descriptors and 4 MiB of accounted identity bytes;
map/list/queue storage adds bounded overhead. At most 256 identities wait for one
active batch, capped at 32 paths / 64 KiB. Theme changes retain old drawing data,
refresh visible paths lazily, and discard obsolete replies without filesystem
rescanning or document paragraph invalidation. Malformed replies publish no new
descriptors. Failed batches report errors and suppress repaint retries until the
generation changes. Long-path validation processes small chunks and yields after
4,096 JavaScript string units, retaining the existing 8 ms execution budget.

Windows evidence:

- First build failed on a missing namespace closing brace in `file_icons.cpp`;
  corrected. Log: `build/file-icons-build-debug.log`. A following rebuild ended
  with unexplained xmake exit `-1073741816`; its log contains no further native
  diagnostic (`build/file-icons-build-debug-fixed.log`). Retrying with `-j 2`
  exposed `[[nodiscard]]` warnings in the new failure fixture; corrected explicit
  discarded values. Log: `build/file-icons-build-debug-retry.log`.
- The first long-path focused run hit `InternalError: interrupted` in synchronous
  UTF-8 validation. Log: `build/file-icons-final-unit-debug.log`. Bounded chunk
  validation and yielding fixed valid long ASCII/CJK paths and an adversarial
  dotted name; oversized UTF-8 paths still reject. The execution budget was not
  increased.
- `./scripts/xmake.ps1 build -y -j 2 astra-tests gui-presentation-smoke astra`
  passed in Debug (`build/file-icons-final-build-debug.log`). Final focused
  `astra-tests.exe --no-version '--test-case=presentation file icons*,workbench file icons*'`
  passed 5 cases / 30,282 assertions; 577 unrelated cases were filtered out.
  Log: `build/file-icons-final-unit-debug-async.log`. Coverage includes warm reuse,
  stale generations, atomic malformed-reply preservation, eviction/admission,
  count/byte batch limits, actual JS defaults, case/compound precedence, Unicode,
  invalid/colliding rules, captured inputs, racing updates, cancellation, deletion
  and full-theme restoration.
- First `./scripts/test.ps1 -Mode all -Gui` passed both 582-case unit suites and
  native workbench checks but failed Release TCP syntax selection. The fixture
  changed selections through another client and treated local GUI idle as proof
  that its notification had arrived. Log: `build/file-icons-acceptance-all.log`.
  The fixture now waits for the actual GUI connection's matching revision and
  selections, and deliberately defers one notification poll per external change.
  The service's stale-selection rejection remains unchanged. Release rebuild and
  `gui-smoke.exe --tcp` passed: `build/file-icons-tcp-fixture-build-release.log`
  and `build/file-icons-tcp-fixture-release.log`. This does not establish a general
  solution to every same-text-revision metadata reordering scenario.
- Final `./scripts/test.ps1 -Mode all -Gui` completed with exit code 0, with
  unchanged code/resources while running. Debug: 582 cases / 1,617,057 assertions;
  Release: 582 cases / 1,616,763 assertions. Zero failed/skipped in both modes.
  Renderer/font/viewport/input checks, all editing/service/recovery/workspace/
  presentation GUI workflows, TCP editing and 12 package-helper invariants passed.
  Log: `build/file-icons-acceptance-all-final.log`.
- Real GUI checks verify deferred visible icon resolution, zero additional warm
  resolutions, unchanged Explorer source version and document shape count, then
  scroll to previously hidden C++ rows and verify their new descriptors. Actual
  GPU captures `build/graphics/gui-file-icons-custom.png` and
  `build/graphics/gui-file-icons-explorer.png` were inspected: the tab and file
  rows draw the same custom glyph/color. These are icon component checks, not
  whole-product visual acceptance. The 640-tab pressure checks also passed.
- After acceptance exited, `./scripts/xmake.ps1 f -m debug -y` restored Debug;
  log: `build/file-icons-restore-debug.log`. Final
  `./scripts/format.ps1 -Check -Offline` passed 157 C++ / 13 JS files and
  `git diff --check` passed; log: `build/file-icons-final-check.log`.

Further component extraction, document IME preparation, row-internal incremental
glyph layout, whole-product visual acceptance and a new distribution remain
incomplete. No new package or non-Windows validation is claimed.

### Persistent measured tabs and JS exit batching (2026-10-10)

Native tab order and cached logical widths now use `TabIndex`, a persistent
measured SumTree with an identity-to-order map. Ordinary identity lookup,
append/remove/reorder and single-caption measurement use logarithmic tree paths.
Exhausting an order-label gap causes a bounded O(n) relabel; this exceptional
path is exercised by adversarial reordering. The model admits at most 65,536
tabs. Count/width/order/font-generation summaries support prefix seeking and
finding the next pending measurement without scanning the complete sequence.

Font/DPI invalidation advances one generation in O(1), retaining provisional
widths until refresh. Preparation prioritizes visible captions, admits at most
eight measurements within a four-millisecond dispatch budget per paint, and
keeps the active tab visible as measurements refine. One caption can overrun
that dispatch budget; this is not a hard per-frame latency guarantee. Pointer
dispatch does not prepare new widths. Repainting current geometry does not
measure every offscreen caption. Explicit tab enumeration and the window's
other document polling still have linear costs; this is not a claim that the
whole frontend now has logarithmic frame cost.

Compact tab metadata pages are exposed by the async UI read API. JS exit closes
at most 32 clean background documents concurrently without activating them;
each close is guarded by its captured text revision and refuses discard.
Changed/dirty rejections retain the document, publish a status and return to
the sequential close decision. Other failures stop exit. Batches drain before
the next decision/checkpoint, and dirty/final active documents retain normal
save/discard/cancel behavior. This replaces the previous repeated activation,
viewport loading and close loop when many clean tabs are open.

Windows evidence:

- First build failed because the new differential fixture omitted `<array>`;
  corrected the self-contained include. Retained log:
  `build/tab-index-build-first-failed.log`.
- The first GUI run failed in the existing long-modal waiter requiring an
  observable `pending` instant; log: `build/tab-index-gui-first-failed.log`.
  The diagnostic rerun passed that stage and the 640-tab checks but exceeded
  the original 20-second shutdown deadline with sequential tab activation;
  log: `build/tab-index-gui-diagnostic.log`. Both fixture directories were kept.
  The modal waiter now recognizes pending work or actual retained shaped
  geometry. Native worker cancellation/deadline tests remain in full acceptance.
  JS clean-background batching fixed the large-window shutdown path; its GUI
  shutdown deadline remains 20 seconds.
- `./scripts/xmake.ps1 build -y astra-tests gui-presentation-smoke astra` passed
  in Debug; log: `build/tab-index-final-build-debug.log`.
  `astra-tests.exe --no-version '--test-case=presentation tab*,presentation indexed tab*,workbench window*'`
  passed 10 cases / 398,090 assertions; 567 unrelated cases were filtered out.
  Log: `build/tab-index-final-unit-debug.log`. Coverage includes 2,000 random
  mutations (seed `0xA57A7AB`), retained identities/labels/widths, exact summary
  and balance checks, 65,536-entry admission, malformed-input preservation,
  O(1) invalidation with shared snapshots, 256 repeated front moves and 1,000
  flat-layout comparisons on 8,192 tabs (seed `0xA571D3`). JS tests hold a full
  32-request close batch and inject stale, dirty and other service failures,
  checking protected documents and prevention of checkpoint/teardown.
- `gui-presentation-smoke.exe` passed real Windows SDL3/Graphite/Dawn checks;
  log: `build/tab-index-final-gui-debug.log`. It opens 640 distinct actual files,
  verifies all identities and active reveal, observes zero additional width
  measurements or paragraph shapes across eight warm repaints, checks deferred
  font invalidation and one measurement per tab in the new generation, then
  restores style and closes within the unchanged deadline. Actual GPU capture
  `build/graphics/gui-tabs-indexed.png` was inspected: captions/close targets
  are separate, overflow controls remain visible and the active final tab is
  visible. This is a tab component check, not whole-product visual acceptance.
- `./scripts/test.ps1 -Mode all -Gui` completed with exit code 0 using unchanged
  source/resources. Debug: 577 cases / 1,586,697 assertions; Release:
  577 cases / 1,586,665 assertions. All passed, zero failed or skipped. Both
  modes passed renderer/font/viewport/input checks, editing/service/recovery/
  workspace/presentation GUI workflows (including the 640-tab pressure case),
  TCP editing and 12 package helper invariants.
  Log: `build/tab-index-acceptance-all.log`.
- After acceptance exited, `./scripts/xmake.ps1 f -m debug -y` restored Debug;
  log: `build/tab-index-restore-debug.log`. Final
  `./scripts/format.ps1 -Check -Offline` passed 154 C++ / 12 JS files and
  `git diff --check` passed; log: `build/tab-index-final-check.log`.

Native icon policy, further component extraction, document IME preparation,
row-internal incremental glyph layout, whole-product visual acceptance and a
new distribution remain incomplete. No new package or non-Windows validation
is claimed.

### JS textobject workflows and shared parsing (2026-10-10)

Default function/class/block/string selections now execute JS-owned workflows
through `astra.ui.selectObject`. Its fenced document read captures the document,
text revision and all selections before asynchronous parser preparation. The
service validates optional `baseSelections` before mutation; cursor movement
without a text revision change therefore rejects the stale selection request.
Malformed, empty, oversized or JavaScript-unsafe guards also reject atomically.
Legacy callers without this optional guard retain current-selection semantics.

The service advertises cached parser operations with `shared:true`. Automatic
JS cancellation or timeout drains only that caller's outstanding response, even
when its operation descriptor arrives late. It does not cancel a parser used by
another client. Owned operations retain automatic cancellation, and explicit
`operation.cancel` retains its service-job semantics. Cancelled selection
continuations cannot submit a later selection or UI publication.

Targeted `revealCaretFor` publication and the native JS completion adapter both
check the document identity before revealing a caret. Background document results
cannot activate or scroll the current document. Duplicate native textobject,
tab, sidebar, Outline and Explorer product-command branches were removed; those
commands resolve through the JS registry. Parsing, document validation, selection
primitives, font geometry and rendering remain native.

Windows evidence:

- The first focused run failed because the new test requested nonexistent
  `document.read`. Corrected the fixture to production `document.get`;
  retained failure log: `build/js-object-unit-first-failed.log`.
- `./scripts/xmake.ps1 build -y astra-tests gui-smoke gui-presentation-smoke astra`
  passed in Debug, including the shared-operation runtime changes.
  Log: `build/js-object-build-debug.log`.
- `astra-tests.exe --no-version '--test-case=application textobject guards*,workbench JS textobject*,QuickJS*shared operation*,QuickJS*late operation cancellation*'`
  passed 7 cases / 258 assertions; 564 unrelated cases were filtered out.
  Log: `build/js-object-unit-debug.log`. These cover fenced deferred reads,
  selection guards, flags/defaults, unavailable/obsolete syntax, actual service
  parser-sharing descriptors, late cancellation, timeout and unchanged owned-job
  cancellation semantics.
- `./scripts/test.ps1 -Mode all -Gui` completed with exit code 0 before the final
  native completion-adapter identity guard. Debug: 571 cases / 1,188,752 assertions;
  Release: 571 cases / 1,188,686 assertions. All passed, zero failed or skipped.
  Both modes passed real renderer/font/viewport/input checks, all GUI workflows,
  TCP editing and 12 package-helper invariants. No source or JS resources changed
  while this acceptance session was live. Log: `build/js-object-acceptance-all.log`.
- After that process exited, the final native completion-adapter guard and GUI
  regression were added. The test returns background document metadata after
  JS publication and checks the active scroll position after command completion.
  `build -y astra-tests astra gui-smoke gui-services-smoke gui-recovery-smoke gui-workspace-smoke gui-presentation-smoke`
  passed in both modes. Logs: `build/js-object-final-build-{debug,release}.log`.
  Each mode then passed `gui-smoke`, `gui-services-smoke`, `gui-recovery-smoke`,
  `gui-workspace-smoke`, `gui-presentation-smoke`, and `gui-smoke --tcp`.
  Logs: `build/js-object-final-gui-{debug,release}.log`. Real local/TCP editing
  verifies all four default selectors on C++ source containing CJK, emoji and
  combining characters, exact string-inner byte bounds and retained content,
  text revision and dirty state.
- `./scripts/xmake.ps1 f -m debug -y` restored Debug configuration;
  log: `build/js-object-restore-debug.log`. Final
  `./scripts/format.ps1 -Check -Offline` passed 151 C++ / 12 JS files and
  `git diff --check` passed; log: `build/js-object-final-check.log`.

This increment does not finish the architecture/UI revision. Native icon policy,
component extraction, measured tab layout, document IME preparation, row-internal
incremental glyph layout, whole-product visual acceptance and a new distribution
remain incomplete. No new package or non-Windows validation is claimed.

### JS-owned workbench style and async updates (2026-10-10)

The client now imports the real shipped `data/defaults.js` before the workbench
and user configuration. Runtime bootstrap settings are extracted separately
from subsequent client preferences; effective startup style merges shipped
defaults, service preferences and explicit client preferences in that order.
The window's duplicate native RGB palette and UI font-family presets were removed.
`WorkbenchStyle` validates a complete candidate before publication. Modal panels
and the tab strip have separate roles; syntax error color is explicitly configured.

Labels, titles, modal/picker inputs, mounted fields, wrapped dialog detail and
monospace text now derive their size and font families from the same configured
UI base. Native font geometry, clipping, input and painting remain in C++.
UI-only changes preserve document paragraph work; document font/foreground/syntax
changes retire obsolete prepared geometry while retaining measured line indexes
and the bounded exact paragraph cache.

`astra.ui.style.read` / `update` return Promises and accept standard request
cancellation/timeout options. Updates are client presentation operations, not
document edits or persisted service configuration. Unknown fields, removed
required roles, malformed colors or invalid fonts reject without partial
publication. The effective style is observable through Agent view state. The
reply is prepared before publishing the candidate.

Live updates now pass through a bounded 256-entry native queue. Patch field/name/
family bounds are checked before copying into the queue. Already accepted native
pointer/text events finish with their captured font geometry before style
publication; an open modal does not require an answer before an update completes.
The owner loop publishes at most eight candidates within a four-millisecond
budget, with a 30-second native deadline. Request cancellation removes queued
updates, and shutdown settles their futures without publishing.

Focused Windows evidence:

- Three initial builds failed under warnings-as-errors: a shadowed color-parser
  local, ignored `[[nodiscard]]` results in exception tests and integer-to-float
  conversion in conditional reference sizes. Corrected the variable name, explicit
  test casts and float constants. Failure logs:
  `build/style-build-{first,second,third}-failed.log`.
- `./scripts/xmake.ps1 build -y astra-tests gui-presentation-smoke astra` passed
  in Debug; logs: `build/style-build-debug.log` and `style-final-build-debug.log`.
- `astra-tests.exe --no-version '--test-case=presentation style*,workbench*style*'`
  passed 4 cases / 108 assertions. The remaining 561 cases were excluded by the
  filter. Tests cover complete JS defaults without bootstrap I/O, UI text hierarchy
  at 9/13/16, independent service/client preference layers, malformed palettes and
  families, invalid geometry, deferred native read/update completion and Promise
  return values. Final log: `build/style-final-unit-debug.log`.
- `gui-presentation-smoke.exe` passed actual SDL3/Graphite/Dawn workflows. The
  service sets font size 11 and sidebar width 270; an explicit client size 13 wins
  without losing the width. Awaited live font/family/palette changes, eight invalid
  patch forms, a pre-aborted request and exact style restoration pass. Styled
  modal pointer insertion at sizes 9 and 16 agrees with independently configured
  native font geometry. Captured document revision/content/selection are unchanged.
  Initial log: `build/style-gui-debug.log`. Queue acceptance additionally checks
  cancellation after actual native submission and a live font update concurrent
  with cold long-input pointer/text/acceptance. A first queue run reported a
  redundant failure from an abandoned native future after the caller's AbortError.
  Cancellation/shutdown now settle that native future without another exception;
  actual invalid configuration errors still reject. Failure log:
  `build/style-queue-first-failed.log`. The corrected run passed;
  log: `build/style-final-gui-debug.log`.
- Inspected actual GPU captures `build/graphics/gui-style-large.png`,
  `gui-style-large-choice.png` and `gui-style-small-input.png`. The large font and
  separate custom panel/tab palettes are visible, body text follows configured
  size, and Unicode input remains clipped within its field. These are component
  checks, not final whole-product visual acceptance.
- `./scripts/format.ps1 -Offline` passed 151 C++ / 12 JS files and
  `git diff --check` passed. Log: `build/style-format.log`.

An initial complete-acceptance attempt reached the Debug GUI presentation run
but failed with `Unknown native UI primitive`: JS resources were edited while
that validation process was still live, so the fixture read the new API against
its previously built binary. This was a validation sequencing error, not accepted
evidence. The process exited, failure log was preserved as
`build/style-acceptance-first-failed.log`, and all targets were rebuilt together.

- `./scripts/test.ps1 -Mode all -Gui` subsequently completed with exit code 0 on
  Windows, using unchanged runtime resources. Debug: 565 cases / 1,188,476
  assertions; Release: 565 cases / 1,188,469 assertions. All passed with zero
  failed or skipped cases. Both modes passed real renderer/font/viewport/input
  checks, all GUI/service/recovery/workspace/presentation workflows, TCP editing
  and 12 package helper invariants. Log: `build/style-final-acceptance-all.log`.
- After the full acceptance process exited, `./scripts/xmake.ps1 f -m debug -y`
  restored Debug configuration; log: `build/style-restore-debug.log`.
  Final `./scripts/format.ps1 -Check -Offline` passed 151 C++ / 12 JS files and
  `git diff --check` passed; log: `build/style-final-format.log`.

No new distribution has been made by these checks.
Remaining native icon policy, component extraction, document IME preparation,
row-internal incremental glyph layout and whole-product visual acceptance are
still incomplete.

### JS-owned document keymaps and page policy (2026-10-10)

The window no longer supplies a second hardcoded default keymap or an editing /
Alt+F4 / page-scrolling fallback after JS binding lookup. Built-in client command
registrations supply the initial map, followed by retained service preferences
and explicit client preferences. SDL token conversion, focused native widget
interaction and revision-checked editing primitives remain in C++. An unmatched
document key cannot silently execute a different native editing policy.

Runtime key bindings carry owner-local registration generations. An explicit
rebinding to the same default value survives the client preference projection,
instead of being erased by an equal-value comparison. The native adapter checks
the generation on key dispatch and publishes a fully prepared map before use;
client rebinding after an async command await takes effect on the next key.
Invalid registrations preserve the map and generation.

JS owns PageUp/PageDown target calculation from native visible-page metrics
provided by immediate UI reads. The existing native scroll update clamps the
captured document's viewport without editing text/selection. Previous/next tab
registrations now include Ctrl+Shift+Tab and Ctrl+Tab; Ctrl+PageUp/PageDown and
Ctrl+Shift+Z are explicit JS aliases. README now points to current acceptance
evidence instead of presenting the old 408-case/older ZIP as the revised workbench.

Focused Windows evidence:

- Initial build failures were in the new acceptance code: a nonexistent
  `EditorWindow.activate_document` call and a multi-argument doctest `CAPTURE`.
  Replaced the former with production `open_path` calls and the latter with
  individual captures. Failure logs: `js-keymap-build-first-failed.log` and
  `js-keymap-build-second-failed.log`. The corrected
  `./scripts/xmake.ps1 build -y astra-tests gui-presentation-smoke astra` passed;
  log: `js-keymap-build-debug.log`.
- `astra-tests.exe --no-version '--test-case=JavaScript*keymap*,workbench JS*'`
  passed 4 cases / 52 assertions; 557 cases were excluded by the filter. These
  check default bindings, deliberate same-value preferences, invalid-call
  preservation, rebinding after await, deferred page reads and document/boundary
  target calculation. Log: `js-keymap-unit-debug.log`.
- `gui-presentation-smoke.exe` passed actual SDL3/Graphite/Dawn acceptance.
  The fixture intentionally gives the service a conflicting previous-tab key;
  an explicit client binding back to the default wins. Actual Ctrl+Tab,
  Ctrl+Shift+Tab, Ctrl+PageUp/PageDown and plain PageUp/PageDown pass. Rebinding
  PageDown after await consumes the next native key without scrolling; restoring
  its JS default restores scrolling. A captured service document revision and
  selection remain unchanged across those page actions. Existing workbench,
  input, view publication/cancellation and shutdown checks also pass. Log:
  `js-keymap-gui-debug.log`.

- `./scripts/test.ps1 -Mode all -Gui` completed with exit code 0 on Windows.
  Debug: 561 cases / 1,188,382 assertions; Release: 561 cases / 1,188,336
  assertions. All passed with zero failed or skipped cases. Both modes passed
  real renderer/fonts/viewport/input checks, all GUI workflows (including the
  new JS key-policy scenarios), TCP editing and 12 package helper invariants.
  Log: `build/js-keymap-acceptance-all.log`. This did not create a distribution.
- Restored Debug configuration after every acceptance process exited with
  `./scripts/xmake.ps1 f -m debug -y`; log: `js-keymap-restore-debug.log`.
  Final `./scripts/format.ps1 -Check -Offline` passed 149 C++ / 12 JS files,
  and `git diff --check` passed. Log: `js-keymap-final-format.log`.

Remaining native theme/icon policy, coherent UI font scaling, component
extraction, document IME preparation, row-internal incremental glyph layout,
whole-product visual acceptance and a final distribution remain incomplete.

### Shared native text fields and IME presentation (2026-10-10)

Modal/picker inputs and JS inline/mounted form fields now use `TextField`.
The native component owns field borders, bounded padding/clipping, placeholder
elision, selection/caret paint, horizontal reveal and IME composition display.
Pointer conversion shares its exact padding/scroll transform. The window adapter
retains SDL candidate-area conversion and guarded deferred input; document
editing and product workflows remain in their existing document/JS layers.
The component receives borrowed frame text and selections, never a mutable
document, service or JS callback. End-of-input reveal reserves the caret stroke.
Collapsed controls do not publish an out-of-bounds candidate area. RAII balances
the canvas save/clip stack even if composition preparation fails mid-paint.

IME preedit uses its own exact paragraph cache. Stable repaint reuses geometry;
cold preedit of at least 4096 bytes shapes through the existing background queue.
Changed/empty preedit, blur and control destruction retire obsolete work. Native
paragraph height positions the underline. Successful and failed composition
preparation leave committed input unchanged. `TextInputLayout.reset()` cancels
work and releases its paragraph on the owner thread. Agent view state exposes
composition pending observations for modal and mounted fields.

Focused Windows evidence:

- `./scripts/xmake.ps1 build -y viewport-layout-tests gui-presentation-smoke astra`
  passed in Debug. Logs: `text-field-build-debug.log` and
  `text-field-final-build-debug.log`.
- `viewport-layout-tests.exe --fields` passed native font/pixel checks for
  Unicode/bidi, exact paragraph reuse, every pixel outside field bounds, tiny
  controls, caret reveal/hit transform and cached composition. Failure injection
  with invalid composition checks both accepted-input preservation and the
  balanced canvas stack. The real worker cancellation test rejects late
  composition publication after a shorter replacement. Log:
  `text-field-native-debug.log`. Additional successful long-composition
  publication coverage is included in the full validation below.
- `gui-presentation-smoke.exe` passed real SDL3/Graphite/Dawn workflows, including
  an injected preedit event that leaves input byte count unchanged and subsequent
  Unicode selection/edit/undo/clipboard, same-frame pointer/text and long-input
  cancellation checks. Log: `text-field-gui-debug.log`.
- Inspected the real GPU capture `build/graphics/gui-text-field-ime.png`:
  CJK/emoji preedit and its underline remain within the input field. This validates
  injected SDL composition display, not an installed physical IME candidate UI.

- `./scripts/test.ps1 -Mode all -Gui` completed with exit code 0 on Windows.
  Debug: 558 cases / 1,188,312 assertions; Release: 558 cases / 1,188,308
  assertions, all passed with no skipped cases. Both modes passed native
  renderer/fonts/viewport/field checks, including successful long-composition
  publication, exact fresh-paragraph comparison and reuse after completion.
  All real GUI workflows and TCP editing passed, as did 12 package helper
  invariants. Log: `build/text-field-acceptance-all.log`. No distribution was
  produced by this command.
- After every acceptance process exited, `./scripts/xmake.ps1 f -m debug -y`
  restored Debug configuration. Log: `text-field-restore-debug.log`.
  Final `./scripts/format.ps1 -Check -Offline` passed 149 C++ / 12 JS files,
  and `git diff --check` passed. Log: `text-field-final-format.log`.

The shared field still performs bounded source
validation/copying and short native shaping on the UI thread. Other native
components, document IME preparation, row-internal incremental glyph layout,
whole-product visual acceptance and a new final package remain incomplete.

### Shared chrome text and responsive controls (2026-10-10)

`TextLabels` now owns plain title/tab/row/status text preparation and Unicode
elision. It reuses the existing exact paragraph cache through a one-row measured
viewport: 512 entries / 2 MiB of accounted text/style, immutable retained
paragraphs and incremental LRU eviction. The old concatenated, rounded float
cache keys and clear-all-after-1024 behavior were removed. DPI/resize no longer
clear the entire chrome cache; exact font/DPI/color/family/locale keys certify
each reuse. UI caption direction is automatic for bidi text.

Display preparation scans a bounded 4096-byte source prefix, normalizes controls
and Unicode line separators, conservatively omits a potentially incomplete
final grapheme, and explicitly appends an ellipsis. Width elision measures its
final grapheme-safe text. The full source model is unchanged. This is plain
unwrapped chrome text, not document or editable-input shaping.

Native `WorkbenchLayout.command_center` is shared by painting and hit testing.
Decorative header clicks are ignored; the visible control invokes the existing
JS command palette. Titles/breadcrumbs now elide within their visible width.
Narrow windows preserve 360 logical units for editing while the sidebar remains
visible. A tab must fit separate icon/caption/close targets; unusable fragments
of the next tab remain in overflow navigation. Tiny navigation targets and the
tab band stay bounded/clipped to the parent. The native GUI fixture also displays
a multiline C++ example with real highlighting, CJK/emoji and Outline symbols.

Focused Windows evidence:

- `./scripts/xmake.ps1 build -y astra-tests viewport-layout-tests
  gui-presentation-smoke astra` passed in Debug. The first build found a leftover
  `labels.clear()` in the DPI handler after replacing its type. Exact keys now
  replace that obsolete bulk invalidation. Failure log:
  `build/chrome-text-build-first-failed.log`.
- `astra-tests.exe --no-version '--test-case=presentation workbench layout*,presentation tabs use*,presentation tab overflow*'`
  passed 3 cases / 406 assertions; 555 other cases were excluded by that filter.
  Bounds cover widths 0–1440 and scales 1/1.25/1.5/2, separate tab hit targets,
  active-tab reveal and invalid dimensions. Log: `chrome-text-layout-debug.log`.
- `viewport-layout-tests.exe --labels` passed actual Windows font/SkParagraph
  pixel and geometry comparisons. New tests cover an exact DPI change smaller
  than the old string key's precision, color, bidi/CJK/emoji/combining text,
  control normalization, width-limited grapheme elision, a byte boundary inside
  an emoji family, a long single combining cluster, invalid UTF-8 and retained
  paragraphs across bounded LRU eviction. Log: `chrome-text-labels-debug.log`.
- `gui-presentation-smoke.exe` passed on actual SDL3/Graphite/Dawn. It verifies
  decorative/header-control clicks, twelve stable repaints without new shaping,
  cache limits, the enlarged narrow editor area and separated tab hit targets;
  the existing real presentation, editable input and shutdown checks also pass.
  Log: `chrome-text-gui-debug.log`.
- Visually inspected real GPU captures `gui-workbench.png`,
  `gui-workbench-narrow.png` and `gui-chrome-command-center.png` under
  `build/graphics/`: the narrow editor area is larger, tab icon/close overlap is
  absent, code/Outline hierarchy is readable and the native palette matches its
  command-center hit target. This is workbench/component evidence, not final
  whole-product visual acceptance.
- `./scripts/format.ps1 -Offline`: 147 C++ / 12 JS files passed;
  `git diff --check` passed.

- `./scripts/test.ps1 -Mode all -Gui` completed with exit code 0 on Windows:
  Debug 558 cases / 1,188,298 assertions and Release 558 cases / 1,188,270
  assertions, all passed with no skipped cases. Real native graphics, fonts,
  viewport/input/label layout, all GUI workflows, local/TCP editing and all
  12 package helper invariants passed. Log: `build/chrome-text-acceptance-all.log`.
  This command validates package helpers; it does not produce a new distribution.
- Restored Debug configuration after all acceptance processes exited with
  `./scripts/xmake.ps1 f -m debug -y`; log: `chrome-text-restore-debug.log`.
  Final `./scripts/format.ps1 -Check -Offline` and `git diff --check` passed;
  formatting log: `chrome-text-final-format.log`.

Short caption preparation and remaining native components still cost UI time.
Complete component extraction, row-internal incremental glyph layout, final
whole-product visual acceptance and a new final package remain incomplete.
No non-Windows validation is claimed.

### Background editable input and native line metrics (2026-10-10)

Modal inputs and inline form fields now share background preparation through
`TextInputLayout` and the existing libuv `LayoutJobs`. Cold single-line inputs
of at least 4096 UTF-8 bytes submit one coalesced job; short inputs and exact
text/style cache hits remain immediate. Content/style changes cancel obsolete
jobs. Failed preparation retains the successful cache and document, reports an
error, and paints an unavailable state while other controls remain usable.

Geometry-dependent pointer input captures widget identity/epoch, focused field,
document revision/selection, exact style and a 30-second deadline. Following SDL
input owns its text and IME payloads in `InputEvents`: 256 events / 64 KiB total,
16 KiB per payload, adjacent-motion coalescing and FIFO transitions/text. Replay
accepts at most 16 events, checking a 4 ms frame budget between events. Target
changes reject obsolete input visibly; Escape, window closure and inline-form
close bypass the queue. Both
pointer and native layout deadlines are soft; native calls still drain safely.
Agent view state exposes input pending/queue and shared worker observations.
JavaScript continues to own dialog/form workflows and await their existing APIs.

A 13.5 KiB tab/CJK/emoji/combining/bidi paragraph exposed repeated native font
metric construction in `getLineMetricsAt`, once for each tab and caret. The
paragraph now retains compact numeric metrics from one `getLineMetrics` call
per actual layout. Refinement and caret queries reuse those native metrics.
Identical tab-delimited width estimates also reuse a bounded 256-entry cache
within one unchanged style. Final paragraphs still use full native shaping.

Focused Windows evidence:

- `./scripts/xmake.ps1 build -y gui-presentation-smoke` passed in Debug.
  `viewport-layout-tests` and the UI were rebuilt during this increment as well.
- `./build/windows/x64/debug/viewport-layout-tests.exe` passed real Windows font/
  SkParagraph pixels, grapheme carets/hits and UTF-16 comparisons. New checks
  cover cold editable shaping, frame coalescing, exact changed-style geometry,
  late cancellation, terminal worker errors, destruction and admission retry.
  Deferred SDL events cover owned/moved payloads, captured modifiers, motion
  coalescing and event/byte overflow without losing already accepted entries.
- `./build/windows/x64/debug/gui-presentation-smoke.exe` passed on actual
  SDL3/Graphite/Dawn. Long modal/form pointer/text placement agrees with
  independently shaped geometry. Confirm, Escape, close-button cancellation and
  a programmatic update interleaved with a captured input sequence preserve the
  expected content. The latter observed an accepted JS update and visible stale
  input rejection. Log: `build/editable-async-gui-debug.log`.
- Visually inspected `build/graphics/gui-editable-long-input.png`: long mixed
  Unicode input, field clipping, caret and focused border render correctly.
  This is component evidence, not whole-product visual acceptance.
- `./scripts/format.ps1 -Offline`: 145 C++ / 12 JS files passed. JS additions
  inside the C++ test fixture are directly written across readable lines.

Corrections and retained failures:

- The earlier shared-input entry overstated its same-batch pointer regression:
  the old mouse helper settled between events. This increment uses an enqueue-
  only helper for text/pointer/text and drains the actual SDL batch together.
- The first build incorrectly treated `utf8::validate` as a boolean instead of
  its throwing/void interface. It was corrected before rebuilding. The log is
  `editable-async-build-first-failed.log`.
- Initial long-tab layout runs exceeded the worker deadline. Reusing segment
  estimates alone did not resolve this. Profiling the pinned native call path
  identified line metric rebuilding; temporary profiling was removed after the
  compact metric cache fix. Logs: `editable-async-viewport-first-failed.log`,
  `editable-async-viewport-diagnostic-failed.log`,
  `editable-async-tab-cache-failed.log`, and
  `editable-async-tab-probe-first-failed.log`.
- Early GUI waits checked before an actual paint submitted work, and one later
  wait expired while the native metric bottleneck remained. Preserved logs:
  `editable-async-gui-first-failed.log`, `editable-async-gui-diagnostic-failed.log`
  and `editable-async-gui-wait-failed.log`.
- The first script race scheduled the update before pointer capture and therefore
  observed a valid click on the changed text. It now captures a bounded backlog
  before starting the script. The next fixture inferred the JS result from a
  status subsequently replaced by the intended stale-input error. It now records
  and checks the actual form API result independently of status/error. Logs:
  `editable-async-gui-race-first-failed.log`,
  `editable-async-gui-race-second-failed.log` and
  `editable-async-gui-race-diagnostic-failed.log`.

Integrated Windows evidence before the final post-pointer close correction:

- `./scripts/test.ps1 -Mode all -Gui` passed. Debug: 557 cases / 1,185,931
  assertions; Release: 557 cases / 1,185,797 assertions. Zero failures/skips.
  Both passed CLI/TCP, actual D3D12/Dawn/Graphite readback, native Unicode/pixel
  checks, editable background layout, all GUI workflows, real clangd/Git,
  workspace/recovery and shutdown. Package helpers passed 12 checks; no new
  distribution package was produced. Log: `build/editable-async-acceptance-all.log`.
- An additional real GUI regression then completed pointer placement while
  retaining queued keys/text and closed the form through its JS API. It exposed
  a real cancellation omission: closing cleared only a pending pointer, leaving
  post-pointer input to be reported as stale. Form close now clears the matching
  widget/epoch input sequence; global cancellation clears queued input as well.
  The failed Release fixture is retained in
  `build/editable-async-close-first-failed.log`.

Final Windows evidence after the post-pointer close correction:

- Rebuilt `astra`, `gui-smoke`, `gui-presentation-smoke`, `gui-services-smoke`,
  `gui-recovery-smoke` and `gui-workspace-smoke` in both configurations with
  `./scripts/xmake.ps1 build -y` followed by those target names. Build logs:
  `editable-async-final-build-{debug,release}.log`.
- Ran `gui-presentation-smoke.exe`, `gui-smoke.exe`, `gui-services-smoke.exe`,
  `gui-recovery-smoke.exe`, `gui-workspace-smoke.exe` and `gui-smoke.exe --tcp`
  from both `build/windows/x64/debug/` and `release/`: every run passed. This
  includes JS form close after pointer completion with retained keys/text, the
  programmatic update race, native Escape/close controls and the existing real
  editing, clangd/Git, recovery/workspace, input ordering and shutdown workflows.
  Logs: `build/editable-async-final-gui-*-{debug,release}.log`.
- `./scripts/format.ps1 -Check -Offline`: 145 C++ / 12 JS files passed;
  `git diff --check` passed. Log: `editable-async-final-format-check.log`.
- Restored Debug with `./scripts/xmake.ps1 f -m debug -y` before the final Debug
  build/tests. Log: `editable-async-final-restore-debug.log`.

Text validation/flattening, short/preedit shaping and remaining native window
components can still cost UI time. Row-internal incremental glyph layout,
complete component extraction, whole-product visual acceptance and a new final
distribution package remain incomplete. No non-Windows validation is claimed.

### Shared editable input behavior and geometry (2026-10-10)

Modal input/picker fields now use the same bounded Unicode document model and
editing commands as inline forms. They support caret/word movement, extended
selection, grapheme deletion, clipboard and local undo/redo instead of maintaining
an append-only string and a select-all flag. JavaScript still owns dialog
workflows and consumes the existing awaited result. Native pointer dragging and
the shared field painter use the document selection; IME placement/composition,
selection colors and horizontal scrolling share that painter.

`TextInputLayout` caches one exact text/all-options paragraph for each field.
Painting and pointer input prepare current content/style, including several
SDL events before repainting. A failed preparation preserves the previous cache
key/paragraph. Single-line/16 KiB limits are checked before shaping. The obsolete
fallback modal renderer and three independent input cache fields were removed.

Focused Windows evidence:

- `./scripts/xmake.ps1 build -y astra viewport-layout-tests gui-smoke
  gui-presentation-smoke`: passed in Debug. The first build failed MSVC C4458
  because a pointer helper's local `geometry` shadowed the window member. It was
  renamed before rebuilding; the failure remains in
  `build/text-input-build-first-failed.log`.
- `./build/windows/x64/debug/viewport-layout-tests.exe`: passed with actual
  Windows font/SkParagraph pixel, caret and hit-test oracles. Editable geometry
  covers emoji/combining/bidi/tab/CJK text, changed color/fonts/DPI, stale text
  detection and preservation after invalid UTF-8, newline, over-limit content
  and invalid style.
- `./build/windows/x64/debug/gui-presentation-smoke.exe`: passed on actual
  SDL3/Graphite/Dawn. Modal selection/copy preserves emoji and CJK, Backspace
  removes an entire combining sequence, undo/redo preserves the expected result,
  and a same-batch text-change/pointer/text sequence agrees with independently
  shaped geometry. Existing picker filtering, actions, custom views and shutdown
  checks also passed.
- `./scripts/format.ps1 -Check -Offline`: 143 C++ / 12 JS files passed;
  `git diff --check` passed. The new JS fixture embedded in a C++ raw string was
  manually expanded; the file formatter does not format that embedded language.

Integrated Windows evidence:

- `./scripts/test.ps1 -Mode all -Gui`: Debug passed with 557 cases / 1,185,869
  assertions, zero failures/skips, including all real GUI/TCP/service checks.
  The first Release run stopped in the existing parent-directory rename watcher
  test: Windows rejected `filesystem::rename` with `Access is denied`. Its
  exception is retained in `build/text-input-acceptance-first-failed.log`;
  no assertion failed in that run. The cause is not established.
- Ran `astra-tests.exe --no-version '--test-case=file monitor parent rename*'`
  in Release 20 times: every run passed. The filtered
  `--source-file=*file_monitor_tests.cpp` suite also passed 14 cases / 573
  assertions. Logs: `text-input-watch-repeat-release.log` and
  `text-input-watch-release.log`. No watcher behavior or assertion was changed.
- `./scripts/test.ps1 -Mode release -Gui`: the full rerun passed with 557 cases /
  1,185,738 assertions, zero failures/skips. Both configurations passed actual
  D3D12/Dawn/Graphite rendering, Unicode/viewport/input geometry, local/TCP
  editing, real clangd/Git, recovery, workspace and presentation checks. Package
  helpers passed 12 checks; no new distribution package was produced.
- Complete logs: `build/text-input-acceptance-all.log` and
  `text-input-acceptance-release.log`.
- Restored Debug with `./scripts/xmake.ps1 f -m debug -y`; final
  `git diff --check` passed. Restore log: `text-input-restore-debug.log`.

Focused logs: `build/text-input-viewport-debug.log`, `text-input-gui-debug.log`,
`text-input-build-debug.log` and `text-input-format-check-first.log`.
Editable shaping remains synchronous and bounded to 16 KiB. This increment does
not establish background editable shaping, row-internal incremental glyph
layout, complete native component extraction, cohesive whole-product visual
acceptance or a new distribution package.

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
