# Native graphics build

Astra's renderer uses SDL3 for windows and input, Skia Graphite for drawing, and
Dawn's native graphics backend for presentation. Windows x64 uses D3D12 and has
been built and tested. The macOS Metal and Linux Vulkan paths and build recipes
are prepared in source only; neither platform has been compiled or tested here.
A raster backend, SDL renderer, Ganesh, or a mock renderer does not satisfy this contract.

## Native Windows frame

The SDL-owned window retains its OS title bar, buttons, sizing and input behavior.
The renderer's narrow DWM adapter applies the JS-compiled `activityBar`,
`foreground` and `border` roles to caption background, caption text and border.
Caption luminance selects the native dark-mode allowance. It does not select a
second application palette. The documented attributes are available starting
with Windows 11 build 22000; actual writes determine per-window support.
Unsupported attributes remain OS-controlled and are exposed as capability flags.
See Microsoft's [DWM attribute reference](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute).

The adapter validates opaque ARGB before mutation, borrows the SDL-owned HWND,
retains its previous write-only color attributes and queries the readable dark
flag. A partial write or frame-refresh failure restores every attempted attribute,
including a possibly mutated failed write. It attempts the remaining restores
after a rollback failure and reports both errors. Invalid or cancelled pending
JS style updates never reach this mutation. Initial color rollback restores OS
defaults. Identical accepted palettes do not rewrite the native frame.

Renderer and GUI acceptance capture only their own HWND through
[Windows Graphics Capture](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createforwindow).
The test-only helper uses a free-threaded frame pool, bounded dimensions and a
three-second wait, owns COM/frame/session lifetimes, reports capture/close failures
and reads back the composed BGRA frame. Its D3D11 dependencies belong only to
those test targets. Production client drawing and GPU PNG capture continue to
use Skia Graphite/Dawn with D3D12 on Windows. These checks do not establish support
on older Windows versions or on another platform.

## Source and tool lock

The pinned Skia checkout includes the reviewed `glyph-cache.patch` under
`xmake/packages/skia-graphite/`. Upstream/output blobs and the patch digest are
verified before building; unrelated or partial source changes are rejected.
Original upstream Git objects remain intact. The installed graphics manifest
records the digest, and xmake requires it. Runtime provenance includes the patch,
manifest, recipe, README and application helper.

The adapter exposes logical-order native HarfBuzz glyphs and genuine
unsafe-to-concat boundaries without replacing SkParagraph's font fallback,
itemization, bidi, metrics or drawing. `GlyphSource` stores immutable measured
run snapshots (1,024 runs / 4 MiB accounted payload). Exact full-context run
matches share the retained raw arrays and skip native shaping. Typeface IDs,
font geometry, bidi/script/language/features/tracking and range must agree;
changed text or different worker font identity currently reshapes completely.
This does not implement local-edit prefix/suffix glyph splicing.

The opaque SkParagraph paragraph cache is replaced by Astra's existing bounded
viewport paragraph LRU so native cache hits cannot bypass metadata collection.
Jobs and that LRU account retained glyph seed/payload bytes. Font, paragraph,
tree/container and GPU allocations remain outside these payload measurements.
Collectors are detached on every layout exit; snapshots hold no native fonts
or paragraphs and can cross worker threads while older paragraphs paint.

The callback catches and retains adapter errors inside Astra. Failed collectors
retire further callbacks; errors are rethrown immediately after native layout
returns, before geometry publication. They never unwind through exception-disabled
Skia frames. This does not claim recovery from upstream native allocation failure.

| Component | Fixed identity |
| --- | --- |
| Skia | `525f94ac3418fd0b0dae42caa220422a94b18b49` |
| Skia tree | `4924cfdc267cd1fcdd7ea63981a0fcb0e10df010` |
| Dawn, selected by that Skia DEPS | `ab3c12305e776fa707503f7ecfab07364e9c7697` |
| GN, selected by that Skia fetch-gn | `b2afae122eeb6ce09c52d63f67dc53fc517dbdc8` |
| Python embeddable | 3.13.7, Windows amd64 |
| CMake | 3.31.8, Windows x86_64 |
| Ninja | 1.13.1, Windows x64 |
| Microsoft DXC | v1.9.2609, Windows x64 runtime DLLs |

The [bootstrap script](../scripts/build-graphics.ps1) contains fixed HTTPS artifact
URLs and SHA-256 checksums. CMake and Ninja hashes were checked against the official
GitHub release asset metadata; Python and GN hashes were calculated from the
official downloaded archives. Skia is fetched by commit, and both its commit and
tree identity are checked. Every dependency checkout is verified against Skia's
own DEPS, including tracked-file modifications. No source or dependency version is
chosen from a moving branch at build time.

Tools and source live under `.tools/graphics/`; downloaded archives live under
`.tools/downloads/`; build intermediates live under `build/graphics/`. The script
does not modify global Git settings or install tools globally. Git and an installed
Visual Studio C++ toolchain with Windows SDK are prerequisites. Compiler/SDK tools
are invoked normally; bootstrap does not read other projects or their configuration.

## Commands

Run with PowerShell 7 from this checkout:

```powershell
./scripts/build-graphics.ps1 -Stage Bootstrap
./scripts/build-graphics.ps1 -Stage Configure -Configuration Release
./scripts/build-graphics.ps1 -Stage Build -Configuration Release -Jobs 8
./scripts/build-graphics.ps1 -Stage Build -Configuration Debug -Jobs 8
```

`Bootstrap` verifies tools and the primary Skia checkout. `Configure` synchronizes
the pinned dependencies, verifies them, discovers the installed Windows toolchain
through its supported setup entry point, and runs GN with unknown arguments treated
as errors. `Build` additionally runs Ninja and emits an install manifest. A failed
native command, source mismatch, or missing required library fails the script.
`-Resync` explicitly asks the upstream dependency synchronizer to run again.

If Visual Studio discovery is unavailable, run from its Developer PowerShell or
pass `-VisualStudioPath`. Download and build stages can take a substantial amount
of time. Plan for several GB of dependencies and approximately 10–20 GB of build
space; the exact figures depend on the compiler and configuration. This is an
estimate, not a measured acceptance result.

The upstream GN build invokes Skia's Dawn CMake integration. This yields
`skia.lib`, `dawn_combined.lib`, SkParagraph, SkShaper, and ICU-backed SkUnicode.
No independent Dawn package is linked into the same executable. The installed
manifest records library hashes, source identities, include paths, and CRT policy.

The local [xmake recipe](../xmake/packages/skia-graphite/xmake.lua) can be integrated
by the root build as follows:

```lua
includes("xmake/packages/skia-graphite/xmake.lua")
add_requires("skia-graphite", {
    system = false,
    configs = {root = os.projectdir(), configuration = "Release", runtimes = "MT"}
})
-- In the GUI target:
add_packages("skia-graphite")
add_rules("astra.graphics-runtime")
set_runtimes("MT")
add_defines("_ITERATOR_DEBUG_LEVEL=0")
```

The GUI and all C++ libraries sharing STL types with it must use **MT and
`_ITERATOR_DEBUG_LEVEL=0`**, including the debug configuration. Skia's official
Windows Dawn builder deliberately retains the release CRT for its debug build,
using disabled optimization plus debug symbols. Do not change only the application
to MD, MDd, or MTd. `is_trivial_abi=false` also remains identical across consumers.
The recipe exports `SK_GRAPHITE`, `SK_DAWN`, `SK_DAWN_HAS_D3D12`, and the matching
`SK_DEBUG`/`SK_RELEASE` header mode, and retains
the upstream whole-archive requirement for `dawn_combined.lib`.

The runtime rule copies `icudtl.dat`, `dxcompiler.dll`, and `dxil.dll` beside the
executable when building and installing. ICU's Windows loader searches beside
the executable/library; it does not embed the Unicode dataset. The DXC runtime
is pinned to Microsoft's official v1.9.2609 release and included with its licenses.
Do not ship only the static libraries and assume all runtime resources disappeared.

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

## Evidence and validation boundary

Validated on Windows x64 on 2026-10-09:

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

## Native macOS and Linux preparation — unverified

`scripts/build-graphics.py` is a separate native bootstrap. It rejects Windows
and any target architecture different from the current host. No Linux/macOS
compiler, executable, emulator, cross build, or CI job was invoked during this
Windows development session. Only Python source parsing and review against the
pinned sources were performed. These instructions require acceptance on actual
native hosts before those platforms can be called supported and verified.

On a native Linux or macOS machine, prerequisites are Python 3.12+, Git, Clang
(Apple Clang on macOS), the platform C++23 standard library and SDK, and `ar`.
The native GN bootstrap requires checkout and compiler paths without whitespace
or shell metacharacters. Linux uses the default libstdc++ ABI and macOS uses
libc++; the application and all C++ dependencies must match. Neither native
configuration enables Skia's trivial-ABI attribute. The system toolchain is a
prerequisite, not a downloaded binary, and its version is recorded in the manifest.

Linux also needs Fontconfig, X11 and Wayland development packages and pkg-config.
SDL 3.4.12's Wayland window backend requires `wayland-client >= 1.18`,
`wayland-egl`, `wayland-cursor`, `egl`, `xkbcommon >= 0.5.0`, and
`wayland-scanner` even when its separate rendering APIs are disabled. X11 needs
Xext and the enabled X11 extension development packages. IBus/D-Bus development
packages are needed for desktop input-method integration; native IME behavior
still needs physical acceptance. A Vulkan loader and a functioning Vulkan GPU
driver are runtime prerequisites. CJK/emoji fallback on Linux requires installed
fonts such as Noto Sans CJK and Noto Color Emoji. They are not synthesized by the
renderer or bundled under an unreviewed font license.

Run these commands only on the corresponding native host:

```sh
python3 scripts/build-graphics.py --stage Bootstrap
python3 scripts/build-graphics.py --configuration Release --jobs 8
python3 scripts/build-graphics.py --configuration Debug --jobs 8
```

Skia and all 48 DEPS checkouts use the same pinned identities as Windows. GN is
built from its exact pinned source; CMake 3.31.8 and Ninja 1.13.1 use the official
native archives with fixed SHA-256 values in the script. CMake hashes came from
its [official checksum file](https://github.com/Kitware/CMake/releases/download/v3.31.8/cmake-3.31.8-SHA-256.txt),
and Ninja hashes from the [official release asset metadata](https://api.github.com/repos/ninja-build/ninja/releases/tags/v1.13.1).
The GN commit position `2175 (b2afae122eeb)` matches the pinned official Windows
binary; the shallow native source bootstrap writes that version header without
fetching moving tags. Generated Ninja regeneration paths are adjusted in build
output only. Pinned source files are left unchanged.

The script uses Skia's genuine GN-to-Dawn-CMake integration, embeds ICU data in
the native archive, builds the native font manager (CoreText or
Fontconfig/FreeType), and installs checked archives and public/generated headers.
Ninja's nested Dawn build is bounded by the same `--jobs` limit. Build environments
exclude unrelated ambient credentials because upstream Dawn prints its environment.
Tools and caches remain under `.tools/graphics/native/<platform>-<arch>/`, source
under `.tools/graphics/src/`, outputs under `build/graphics/<platform>/<arch>/`,
and installed packages under `.tools/graphics/install/<platform>/<arch>/<mode>/`.
Expect several GB of source and approximately 10–20 GB of build/install space for
both modes; actual size and build time depend on the native host.

The xmake package validates platform, architecture, configuration, source identity,
standard-library ABI and artifact hashes before publishing include/link settings.
Linux static archives use a GNU linker group; macOS receives the native Metal,
CoreText and Cocoa frameworks. The SDL recipe explicitly requires Cocoa/Metal on
macOS or both X11/Wayland on Linux and verifies the generated SDL configuration
before installation. Missing window backends fail instead of producing an
offscreen-only package. SDL shared libraries are copied next to built/installed
executables, with `$ORIGIN` or `@executable_path` runtime search paths on native
Unix platforms. Windows likewise copies SDL3.dll alongside ICU and DXC runtime
files; it must launch without xmake adding a package directory to PATH.

The renderer obtains X11/Wayland handles from SDL window properties, and macOS
owns an `SDL_MetalView` until after the Dawn surface is released. It passes the
view's checked `CAMetalLayer` to `wgpu::SurfaceSourceMetalLayer`. The pinned Dawn
`SwapChainMTL.mm` sets the Metal device and drawable pixel dimensions when the
surface is configured, so resize uses the same pixel-size reconfiguration path
as Windows. Native font and surface APIs were source-reviewed; that is not
evidence of native runtime behavior. Independent native acceptance must include
window startup, resize, DPI, minimize/restore, Unicode/IME/clipboard, actual GPU
readback, device loss, shared-library loading and shutdown on each window system.

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

Activity-bar and breadcrumb/status painting remain separate controller work.
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
The remaining outer components retain separate controller paths. This evidence
does not establish whole-product visual acceptance.

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
