# Graphics dependencies and rendering

Astra uses the official Dawn port and an official-based Skia 148 port at the
vcpkg baseline in vcpkg.json. The Skia overlay adds only the required shaping
cache adapter; see dependencies/ports/skia/README.md. GN is Skia's internal
build tool, invoked by the official vcpkg recipe. Astra is built with CMake/Ninja.
There is no separate graphics bootstrap or custom Dawn CRT patch.

The Windows triplet uses static C++ libraries and /MD (/MDd in Debug). SDL3 is
shared. Official exported CMake targets carry Skia definitions and dependencies.
Dawn uses its independent official build; Skia does not build an embedded Dawn.
DXC DLLs and the toolchain's redistributable CRT are staged and installed beside
Astra. Official ICU is linked with its data; a separate icudtl.dat is not required.

Configure with scripts/cmake.ps1 --preset windows-debug (or windows-release).
The first configuration compiles dependencies and can take substantial time and
disk space. Fixed source hashes and vcpkg ABI binary caches are under .tools/.
The shaping and host async-context patches remain required capabilities, not
optional optimizations. Configuration fails if required interfaces are missing.

The sections below describe rendering behavior. Historical xmake build details
and acceptance are retained in STATUS.md; only new CMake evidence establishes
migration acceptance. Linux/macOS source is present but unverified on this host.

## Renderer integration contract

1. Create the SDL3 window and obtain its `HWND` with
   `SDL_GetWindowProperties` / `SDL_PROP_WINDOW_WIN32_HWND_POINTER`.
2. Install Dawn's native proc table, create the native instance and D3D12 adapter,
   then create the device with error/device-lost callbacks. Request optional
   features only when that adapter exposes them.
3. Create a WebGPU surface using `wgpu::SurfaceSourceWindowsHWND`. Query supported
   surface capabilities and configure a supported format, usage, and present mode.
4. Pass the instance, device, and queue through `DawnBackendContext` to
   `skgpu::graphite::ContextFactory::MakeDawn`, then create its recorder.
5. For each frame, acquire the surface texture, call
   `BackendTextures::MakeDawn(texture.Get())`, and wrap it with
   `SkSurfaces::WrapBackendTexture`. Draw the editor with its `SkCanvas`.
6. Snap the recorder, check `insertRecording`, check `submit`, and present. Keep
   resources alive until submission requirements are met. Service Dawn events and
   wait for unfinished GPU work before destroying the renderer.

Use `SDL_GetWindowSizeInPixels` for the surface dimensions. Reconfigure after
pixel-size changes, and skip acquisition while minimized or at zero size. Handle
acquisition failure, device loss, and missing hardware explicitly. A failure must
not leave a blank window presented as a functioning editor.

Enable `SDL_StartTextInput` and handle both committed text and IME preedit events.
Update `SDL_SetTextInputArea` as the caret moves. Text layout and hit testing must
share shaped runs; individual byte glyph widths are insufficient for Unicode.

`TextLayoutEngine` owns native DirectWrite fonts, SkParagraph caches, and ICU.
`TextLayout` uses HarfBuzz shaping and provides original UTF-8 byte offsets for
hit testing, grapheme-safe carets, range rectangles, and drawing. Native fallback
includes CJK and color emoji; exact emoji coverage depends on installed fonts
(Windows' current emoji font displays regional flag indicators as letters).
Tabs use measured space advances and pixel-width placeholders; an ICU adapter
preserves the original tab's bidi class instead of treating it as U+FFFC.
Layouts reject malformed UTF-8. The UI's default is an unwrapped logical line.
Soft wrapping supports ordinary text and LTR-base tab lines; explicitly RTL-base
lines containing tabs reject wrapping because that combination has not yet been
implemented reliably. It is not silently approximated.

## Historical graphics evidence (xmake)

The following records describe the earlier build, validated on Windows x64 on
2026-10-09. They do not establish acceptance of the official-port migration;
current CMake commands and outcomes are recorded in STATUS.md.

- `-Stage Bootstrap`: fixed tool hashes and Skia commit/tree checks passed.
- `-Stage Configure -Configuration Release`: all 48 DEPS checkouts verified;
  GN generated 114 targets with unused arguments treated as errors.
- `-Stage Build -Configuration Release -Jobs 8`: all real Skia, Dawn/Tint,
  SkParagraph, SkShaper, and ICU-backed SkUnicode libraries built and installed.
- `-Stage Build -Configuration Debug -Jobs 8`: all 1,622 build steps completed;
  the separate Debug install uses the same pinned revisions and MT policy.
- `tests/graphics_smoke.cpp`, linked against only the installed include/library manifest, created
  a real D3D12 Dawn device and Graphite surface, drew green over red, read pixels
  back from the GPU, checked the expected RGBA values, and initialized SkUnicode
  ICU using the installed runtime data. Release and Debug: **PASS**.
- `tests/renderer_smoke.cpp` used the production `astra::ui::Renderer` and SDL3
  3.4.12 to present native HWND surface frames, save and decode a real GPU PNG,
  verify exact green/red pixels and resized dimensions, minimize/restore, set the
  IME candidate area, destroy the window/device, and create a second renderer.
  Release and Debug: **PASS**. The PNG is `build/graphics/renderer-smoke.png`.
- `tests/text_layout_tests.cpp` exercises real native fonts, empty lines, CJK,
  emoji/ZWJ/flags, combining marks, RTL and mixed-direction hit/caret roundtrips,
  selection boxes, proportional-font tab stops, DPI scaling, supported wrapping,
  malformed UTF-8 rejection, and randomized mixed Unicode/tab positions. It also
  paints text/selection/carets through the real renderer into
  `build/graphics/text-layout-smoke.png` for visual inspection.
- Color spans are built as SkParagraph text styles over original UTF-8 ranges.
  Partial graphemes receive a single style. Native-font tests compare caret,
  hit-test, and layout geometry before and after syntax coloring, including tabs,
  combining marks, CJK and bidi text. UTF-16-to-UTF-8 conversion is tested with
  surrogate pairs. The pinned SkParagraph `updateForegroundPaint` only supports
  whole-paragraph updates and is deliberately not used for syntax ranges.
- `tests/gui_smoke.cpp` is an independent real SDL/Graphite window plus
  `ServiceHost`. Native SDL events exercise Unicode and IME preedit/commit,
  multicursor input and one-step undo/redo, system clipboard, document find and
  replacement, workspace replacement with explicit confirmation, syntax object
  selection, dirty-close cancellation, save/reopen, and editing beyond a 64 KiB
  line-page boundary. Debug: **PASS**. GPU screenshots are
  `gui-editor.png`, `gui-palette.png`, `gui-dirty-close.png`, and
  `gui-replace-confirm.png` under `build/graphics`.
- `tests/gui_services_smoke.cpp` uses the actual fixed clangd binary and a fresh
  Git repository under `build/graphics`. Debug: **PASS** for completion with
  atomic undo, hover, definition, references, rename preview/confirmation,
  formatting, diff/gutter, stage/unstage, commit, and guarded discard including
  refresh of the open clean buffer. Git test configuration is isolated from
  global/system configuration. Clangd fixtures disable system header searches.
  Screenshots include `gui-completion.png`, `gui-hover.png`,
  `gui-references.png`, `gui-rename-confirm.png`, `gui-git-diff.png`, and
  `gui-git-discard.png`. The Windows process host executable must be deployed
  beside these test executables, as it is beside the application.

The editor front end consumes the same JSON-RPC service as remote and script
clients. Requests are queued and futures are only polled with a zero timeout;
revision-dependent edits are serialized. Viewport requests follow all line-page
continuations and discard obsolete results. Syntax and language responses are
version checked before use, while shaping supplies both painting and hit testing.
The UI exposes the file tree, tabs, outline, command palette, search, Git changes,
and language actions. User JavaScript commands and bindings merge with defaults;
EditorConfig controls each document's indentation and line endings.

The recovery interface uses `recovery.status`, `recovery.list`,
`recovery.restore`, and `recovery.flush` through this same channel. Startup
discovery produces a persistent review banner; a session preview requires an
explicit Restore action. Restored buffers open as dirty tabs and are saved only
by the normal Save command. Exit waits asynchronously for a durable checkpoint;
on failure it keeps the window open and offers Retry, Keep open, or an explicit
Exit anyway confirmation. A canceled exit cannot be completed by a stale flush
response. Background recovery errors remain visible in the editor.

`tests/gui_recovery_smoke.cpp` is an independent Windows acceptance executable
using the real RecoveryStore, ServiceHost, native renderer and input path. It
covers startup notification, explicit Unicode/selection recovery, unchanged disk
before Save, dirty-state preservation, and durable exit. Its failure-path channel
injects one checkpoint RPC error and can withhold a real completion to check that
the window waits. It also checks cancelled exit followed by actual Unicode
editing, a dirty document created by another client during checkpoint I/O, fresh
checkpointing after that close decision, and the real Retry command. This is
fault injection around the actual service, not a replacement persistence
implementation. Execution results and their exact source scope belong in
`docs/STATUS.md`.

Logs, independent MSVC response files, executables, and PNGs are under `build/graphics/`.
The standalone smoke sources are checked in under `tests/` and have their own
`main`; do not add them to the doctest executable. MSVC's
PDB service was inaccessible in the restricted execution sandbox (C1902); the
normal authorized Windows build outside that sandbox passed. No SDK files were
modified. These results verify the native renderer, text layout, and editor
command path on Windows. Final Debug/Release application checks are coordinated
through the repository's xmake targets.

Acceptance still requires:

- A physical monitor DPI transition and real GPU device loss. The renderer queries
  pixel dimensions/display scale, handles DPI events, and rebuilds lost devices;
  those two external events have not been induced by this validation.
- Physical keyboard/IME candidate interaction on actual input methods. Automated
  acceptance injects SDL preedit/commit events and verifies the native text-input
  lifecycle and candidate-area updates; it does not simulate an installed IME UI.
- Final application packaging and its runtime dependency inspection.

Record actual commands and outcomes in `docs/STATUS.md`. Do not enable or run
Linux/macOS validation from this Windows environment.

## Chrome text and responsive controls

Titles, filenames, row captions and status text use `TextLabels`. The component
reuses the exact text/all-options paragraph cache through a one-row measured
viewport, instead of stringifying floating-point font/DPI values. Default limits
are 512 cached paragraphs / 2 MiB of accounted text/style. Paragraph handles stay
valid across eviction. Resize and DPI events preserve the LRU; each reuse still
certifies the current exact style, and width elision is measured afresh.

Caption normalization scans a bounded 4096-byte source prefix, with up to one
scalar of decoding lookahead. ASCII controls and Unicode line separators display
as spaces. Longer captions conservatively omit the final potentially incomplete
grapheme and append an ellipsis; combining/ZWJ continuations beyond the budget
are not scanned. Width elision also uses complete grapheme prefixes and measures
the final displayed paragraph. The original model text remains available to
reads/actions. These plain, unwrapped captions do not interpret color spans.
Preparation is synchronous and bounded; cache counts do not measure native
font/paragraph/GPU heaps or establish constant frame latency.

`WorkbenchLayout.command_center` owns both the drawn control and its pointer hit
area; decorative header clicks do not invoke a command or select document text.
Breadcrumbs and command-center titles elide instead of clipping an arbitrary
text tail. A visible sidebar leaves at least 360 logical units for the editor
and hides when its 180-unit minimum cannot fit. Visible tabs reserve at least
96 logical units for distinct icon/caption/close targets; remaining slivers are
accessed through overflow navigation. Navigation cells also stay inside tiny
parent bounds, and the entire tab band clips its drawing.

`WorkbenchChrome` paints the root command center and sidebar headings/controls.
It borrows `TextLabels`; the cache must outlive the component. JS view
specifications supply the title and optional `outlineTitle`, and the controller
supplies current expansion, loading and pointer state. The component owns no
documents, services or command workflows. Refresh/cancel and Outline input
targets return to the existing controller and JS dispatcher.

Header captions center vertically and elide at grapheme boundaries. The command
center reserves the hint's measured width; sidebar titles reserve the refresh
control before elision. Busy hover shows cancellation without replacing the
published indexed rows. Paint and input use the same `WorkbenchLayout` heading,
refresh, section and Outline rectangles. Layout accepts the configured 9..16
logical-unit UI font size, grows relevant bands for its line metrics, snaps shared
edges to physical pixels and clips them to the frame. Non-finite derived geometry
rejects. The component clips captions/icons and restores canvas state on failure;
hidden sidebar captions are not prepared. Native-font pixel checks cover three
font sizes and four DPI scales, including warm reuse and control separation.

`WorkbenchChrome` also owns activity, path and status presentation. `ChromeState`
retains ten bounded immutable measured row banks and a guarded owner context;
the shipped `astra/chrome` JS provider supplies all captions and actions. The
controller passes native document/primary-caret metrics, including distinct
UTF-16 and optional matching shaped visual columns. JS formats their labels.
Candidate publication validates the complete 64 KiB model before mutation;
pending, failed and retired generations retain pixels but cannot activate them.
The shared native measurement exposes the same visible boxes to painting,
pointer input and Agent state. Activity overflow has controls/wheel navigation,
path layout retains final fitting components, and status items admit earlier
right-hand descriptors first while reserving separate left/right regions.
Text uses configured UI fonts and grapheme-aware elision; providers select
theme tones and bounded shared glyphs. Provider waits support cancellation and
1..30,000 ms deadlines, with late-error suppression. JS exit policy suspends
them before the first idle fence and restores them afterwards.
Optional welcome/title/detail/action and notice banks share that same publication.
The body component derives welcome row heights from actual native paragraphs,
separates labels from badges, and clips every painted/hit target to the document
region. An active document hides retained welcome targets immediately. Notice
admission reserves room for direct host diagnostics; diagnostics remain visible
without a successful JS publication and their shared box blocks editor pointer
fallthrough. Caption display bounds and malformed-byte replacement preserve the
original host error. Recovery review and workspace dismissal are JS actions.
The native Windows frame follows the compiled JS theme as documented above;
whole-product visual acceptance remains outstanding.

`TabBar` owns the native tab band's preparation, painting, overflow position,
active reveal and caption/close/navigation hit geometry. Its caller keeps the
document identities and dirty state in `TabIndex` and supplies visible icon
descriptors from the asynchronous JS policy cache. The component has no document,
filesystem, service or JS command workflow. It borrows the shared `TextLabels`
cache, which must outlive it. The caller keeps the model stable during a pass.
Visible labels have measurement priority; each preparation pass admits at most
eight caption measurements and checks a four-millisecond budget between calls.
This is a dispatch budget, not a hard frame deadline. Pointer layout never
prepares new captions. Font/DPI changes invalidate the measured model generation;
foreground-only changes update pixels without remeasuring widths. Paint, including
exceptional metadata callbacks, restores borrowed canvas clipping/transforms by
RAII. The window handles activation and close through its existing action paths
and applies drag reorder to its caller-owned presentation index.

## Document paragraph scheduling

`TextSource` holds immutable materialized UTF-8 and a persistent Rope/SumTree
coordinate index. `TextLayout` no longer allocates per-byte and per-UTF-16-unit
conversion arrays. UTF-8/UTF-16 seeks visit measured tree paths and one bounded
1,024-byte leaf. Native layout adapters explicitly snap interior surrogate
columns to their scalar start; document edits keep the strict boundary API.
ICU remains authoritative for grapheme boundaries and user caret snapping.

On a cache miss, `DocumentViewport` can pass a retained source from the same row
in its bounded paragraph LRU. A worker shares only this immutable text/index
data, not a previously published native paragraph or font collection. The source
compares scalar-aligned unchanged edges and replaces the changed Rope extent,
preserving shared coordinate subtrees. Native text, caret, selection, tabs and
painting still use the complete Unicode paragraph. Old paragraphs can continue
painting while that worker prepares the replacement.

Cache/job accounting includes both flat-text and coordinate-chunk payload bytes,
conservatively counting shared seeds per job. Tree/container/native allocations
are additional; this is not a total heap measurement. Cache eviction may remove
a seed. Comparing incoming materialized text still scans unchanged edges, and
ICU segmentation and native glyph shaping still process the complete row. This
coordinate reuse is groundwork for incremental glyph layout; it does not claim
row-internal glyph reuse or logarithmic end-to-end edit latency.

Document paragraphs use `DocumentViewport` and `LayoutJobs`. Cache hits and short
rows remain immediate; cold rows of at least 4096 UTF-8 bytes shape on libuv
workers. A job owns a private native font collection/engine and transfers its
completed paragraph to the UI without further worker access to those fonts.
Paragraph access is serialized. This preserves complete Unicode, bidi, tab and
color-run semantics rather than slicing a shaping run arbitrarily.

The queue admits at most 16 jobs / 64 MiB of accounted input/style storage by
default, with a 16 MiB text-input limit and a 30-second soft deadline. Those
counts do not measure native font/paragraph/GPU heaps. Cancellation cannot
interrupt an active native shaping call: completion discards its result and
drains safely. The event loop keeps running during shutdown until the queue is
idle. Viewport publication/style changes cancel obsolete jobs, and geometry
dependent input waits for a matching paragraph. Changed long rows still require
complete native shaping; other text components may still shape synchronously.

Read-only panels and dialog details use `TextSurface`, sharing the same measured
page, exact-style cache and layout queue as documents. Their immutable logical
line index is bounded to 8 MiB / 100,000 rows, and visible pages to 256 rows.
Repeated frames retain pending jobs; scroll/reset/style changes invalidate old
geometry. Modal actions remain available while paragraph work is pending, and
closing a modal cancels only its own surface jobs. Source indexing and visible
page copying are still explicit UI-thread costs.

Editable modal inputs and inline form fields share the `TextField` component,
`TextInputLayout` paragraphs and Unicode document editing commands. Caret,
selection, clipboard and undo/redo operate on grapheme-safe document positions;
pointer input prepares the current text/style before hit testing, including
multiple SDL events processed before the next paint. The cache key includes
all layout options, so theme/font/DPI changes replace geometry. Failed text or
style preparation preserves the previous cache entry. These single-line fields
are bounded to 16 KiB. Cold fields of at least 4096 UTF-8 bytes use the shared
layout queue; short fields and exact cache hits remain immediate. Pending fields
paint their border and a progress marker without using geometry from old text.
Worker failures retain the document/cache, display an error, and leave other
controls active. Field cancellation/destruction discards its own pending job.

`TextField` receives borrowed text/selection frame snapshots and theme styles;
it owns no document or product workflow. Painting and pointer conversion use
the same bounded padding and horizontal scroll transform. Scroll reveal reserves
the caret stroke, and selection/text/placeholder/composition pixels stay clipped
to the control, including collapsed bounds. The returned physical-pixel IME area
stays within the control; the window adapter converts it for SDL. Canvas state is
restored by RAII when native preparation or drawing fails.

Modal layout consumes shared `DialogMetrics` measured from configured native UI
fonts, CJK/emoji fallback and the selected monospace detail family. Cached line
probes provide title/detail/input/row/button logical extents. Minimum control
heights round upward in physical pixels; every panel/title/input/content/button
edge snaps to the same pixel grid before painting and hit testing. Small frames
clip available regions while preserving their ordering. Confirmation item
painting uses its measured row step. The same input box drives `TextField`
pointer conversion and the returned SDL candidate area. Native and asynchronous
JS read state expose the measured extents for automation.

Overlay and mounted `InlineFormLayout` regions likewise use cached native line
probes for their own title/input/toggle/message/action reference sizes. Fields
reserve the same five-unit vertical padding as `TextField`; actions reserve
their seven-unit label inset. Whole physical-pixel minimum extents admit the
configured paragraph on roomy frames, while tiny owners clip ordered regions
inward. Mounted panels start at the actual sidebar heading boundary. Agent
form snapshots, paint and pointer handling invoke the same measured layout.

Field IME preedit has its own exact `TextInputLayout` cache. Unchanged preedit
reuses the native paragraph instead of reshaping every repaint. Cold preedit of
at least 4096 bytes uses the same background queue; changed preedit cancels the
older request, and blur/empty preedit releases its cache and retires its job.
The underline follows native paragraph height. Preedit never mutates the input
model. The existing JS form/dialog APIs and asynchronous workflows are unchanged.

Document IME preedit uses the same exact text/style cache and shared layout queue.
Its single-line UTF-8 payload is bounded to 16 KiB; malformed or oversized
updates reject before replacing the current preview. Cold candidates of at least
4096 bytes prepare in the background, displaying only a progress underline while
pending. Identical candidates reuse their paragraph. Font, DPI, line-height and
foreground changes invalidate the exact key; direction detection is automatic.
Candidate replacement/empty updates retire obsolete work. Escape, document
activation/closure, field focus changes, window focus loss and shutdown release
the preview and dismiss the native composition. A committed SDL text packet
clears the preview without cancelling a subsequent OS composition. Preparing
preedit never changes document text, selections or undo history. Candidate-range
styling from SDL's scalar start/length fields is not implemented by this painter.
An IME preparation exception reports a preview error and paints an error marker,
without routing it through the window's fatal renderer boundary. Failed queued
keys remain terminal until the candidate/style changes or the preview is cleared;
repainting does not resubmit failed work.

Pointer placement waiting for editable geometry captures widget identity/epoch,
focused field, document revision/selection, style, coordinates and a 30-second
deadline. Following SDL input owns its text/IME payloads in a FIFO limited to
256 events / 64 KiB, with a 16 KiB per-payload limit. Adjacent motion packets
coalesce. Replay accepts at most 16 events, checking a 4 ms frame budget between
events and checking the current target before each event; changed focus/content/
style rejects obsolete input
visibly. Escape, window closure and the inline form's close button bypass the
queue. These input deadlines are soft, as are native layout deadlines.

Paragraph layout caches compact numeric line metrics once per native layout.
Tab refinement and caret queries reuse them instead of repeatedly asking
SkParagraph to rebuild every style run's font metrics. A bounded 256-entry cache
also reuses repeated tab-delimited width estimates within one unchanged style;
the final paragraph still receives full native Unicode/bidi/tab shaping.
Text validation, document flattening and short field/preedit shaping remain
explicit UI-thread costs. This does not establish
row-internal incremental glyph shaping,
bounded native font/paragraph heaps or constant frame latency.

## Upstream references

- [Skia build requirements](https://skia.org/docs/user/build/)
- [Pinned Skia source](https://skia.googlesource.com/skia/+/525f94ac3418fd0b0dae42caa220422a94b18b49)
- [Pinned dependency manifest](https://raw.githubusercontent.com/google/skia/525f94ac3418fd0b0dae42caa220422a94b18b49/DEPS)
- [Pinned Dawn GN integration](https://raw.githubusercontent.com/google/skia/525f94ac3418fd0b0dae42caa220422a94b18b49/third_party/dawn/BUILD.gn)
- [Pinned Dawn builder and CRT policy](https://raw.githubusercontent.com/google/skia/525f94ac3418fd0b0dae42caa220422a94b18b49/third_party/dawn/build_dawn.py)
- [Pinned Windows CMake settings](https://raw.githubusercontent.com/google/skia/525f94ac3418fd0b0dae42caa220422a94b18b49/third_party/dawn/cmake_utils.py)
- [Graphite/Dawn window context](https://raw.githubusercontent.com/google/skia/525f94ac3418fd0b0dae42caa220422a94b18b49/tools/window/GraphiteDawnWindowContext.cpp)
- [Windows surface creation](https://raw.githubusercontent.com/google/skia/525f94ac3418fd0b0dae42caa220422a94b18b49/tools/window/win/GraphiteDawnWindowContext_win.cpp)
- [SDL window properties](https://wiki.libsdl.org/SDL3/SDL_GetWindowProperties)
- [SDL Metal view](https://wiki.libsdl.org/SDL3/SDL_Metal_CreateView)
- [SDL Metal layer](https://wiki.libsdl.org/SDL3/SDL_Metal_GetLayer)
- [Pinned Dawn Metal swapchain](https://github.com/google/dawn/blob/ab3c12305e776fa707503f7ecfab07364e9c7697/src/dawn/native/metal/SwapChainMTL.mm)
- [Pinned SDL native backend dependencies](https://github.com/libsdl-org/SDL/blob/release-3.4.12/cmake/sdlchecks.cmake)
- [Pinned GN bootstrap](https://gn.googlesource.com/gn/+/b2afae122eeb6ce09c52d63f67dc53fc517dbdc8/build/gen.py)
- [SDL pixel dimensions](https://wiki.libsdl.org/SDL3/SDL_GetWindowSizeInPixels)
- [SDL text input](https://wiki.libsdl.org/SDL3/SDL_StartTextInput)
- [SDL IME location](https://wiki.libsdl.org/SDL3/SDL_SetTextInputArea)
- [Microsoft DXC v1.9.2609](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2609)
- [xmake generic Skia package](https://raw.githubusercontent.com/xmake-io/xmake-repo/master/packages/s/skia/xmake.lua)

The generic xmake Skia package does not currently expose this Graphite/Dawn build
configuration. It is not interchangeable with the local graphics package.

## Workbench style ownership and live updates

Indexed sidebar views share native `ViewRows` presentation. JS supplies row
descriptors, providers and actions; C++ draws immutable projected rows and
dispatches input. `ViewRowsLayout` gives painting, pointer hit testing, keyboard
paging and icon visibility one clipped half-open range. Fractional pixel edges
use the same rounded rectangles for paint and hit testing, including partially
visible final rows. Tree access visits the visible range without flattening.

Row height reserves configured UI font line metrics plus padding, with the
existing 24-unit minimum. Unicode labels use the shared grapheme-aware elision
and retained paragraph cache; selection, inactive selection and hover retain
their separate theme roles. The painter restores caller canvas state on failure
and skips caption preparation for collapsed, zero-area and fully clipped views.
Native-font pixel checks cover four DPI values; real SDL/GPU tests cover a
602-row custom JS view and font updates. Agent sidebar `viewport.painted` reports
the most recent completed frame, while `first`, `end` and `page` describe current
geometry. `WorkbenchChrome` owns sidebar headings and their control geometry;
Outline content uses the same row painter with a bounded `ViewPage` publication.
Its immutable measured index permits leaf indentation without requiring off-page
ancestors. The native page fences document, text revision, active-document
activity, viewport and request generation; pending or retired rows cannot
activate. The shipped JS provider independently queries the service's measured
symbol range, projects labels/icons and supplies all pane messages and actions.
No complete symbol list is copied into JS on scrolling. The native highlighter
no longer fetches or projects Outline content. User providers can replace and
restore this policy through Promise-based reads, refresh and publication.
Outer activity/path/status content and actions now use the shared guarded chrome
provider/component above. This evidence does not establish whole-product visual
acceptance.

Quick Open and user-provided asynchronous pickers share `PickerLayout`, native
editable text geometry and the persistent `PickerIndex`. Painting and pointer
placement reserve the same empty loading row. Query changes retain previous
rows but prevent their activation until a matching generation is published.
Result objects with `iconPath` use the shared JS file-icon descriptor cache.
The actual GPU capture `build/graphics/gui-quick-open.png` shows a fuzzy query
selecting `generated/item4999.txt` in a 5,000-file fixture. This establishes the
tested picker presentation, not whole-product visual acceptance.

`data/defaults.js` supplies all workbench RGB roles and UI font families. The
client imports it into its actual QuickJS runtime before user policy, retains the
bootstrap settings separately and merges service and client preferences over
that base. No native alternate palette is shown before asynchronous service
settings arrive. `WorkbenchStyle` compiles an entire candidate before publishing
colors, fonts and sidebar width; popup `panel` and tab-band `tabs` are independent.

Chrome labels, modal inputs, mounted fields and dialog body/monospace text use
reference sizes relative to a shared 13-unit UI base. Font-family and exact size
changes participate in existing paragraph cache keys. UI-only updates preserve
document paragraph preparation; document foreground/syntax/font changes retire
obsolete geometry while retaining line indexes and the exact bounded cache.

The async client `astra.ui.style` API returns effective presentation settings and
supports atomic partial updates with request cancellation/timeouts. Agent view
state exposes the same effective style. Native SDL pointer tests compare styled
input insertion at sizes 9 and 16 against independently configured font shaping,
then verify document revision/content/selection preservation. Invalid patches
and pre-aborted updates retain the complete previous style. Real GPU captures
are `build/graphics/gui-style-{large,small}.png`, the corresponding `-input.png`
files and `gui-style-large-choice.png`. This validates the tested components;
whole-product visual acceptance remains outstanding.
