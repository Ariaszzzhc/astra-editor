# Windows graphics build

Astra's renderer uses SDL3 for windows and input, Skia Graphite for drawing, and
Dawn D3D12 for presentation. A raster backend, SDL renderer, Ganesh, or a mock
renderer does not satisfy this contract. This document describes the native
Windows x64 build only; Linux and macOS have not been tested here.

## Source and tool lock

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

Logs, independent MSVC response files, executables, and PNGs are under `build/graphics/`.
The standalone smoke sources are checked in under `tests/` and have their own
`main`; do not add them to the doctest executable. MSVC's
PDB service was inaccessible in the restricted execution sandbox (C1902); the
normal authorized Windows build outside that sandbox passed. No SDK files were
modified. These results verify the native renderer and dependency path. Editor
layout and command acceptance remain separate.

Acceptance still requires:

- A physical monitor DPI transition and real GPU device loss. The renderer queries
  pixel dimensions/display scale, handles DPI events, and rebuilds lost devices;
  those two external events have not been induced by this validation.
- Rendered text, selection, caret, scrolling, and IME behavior through the actual
  editor command path, with error callbacks free of unexpected validation errors.
- Final application packaging and its runtime dependency inspection.

Record actual commands and outcomes in `docs/STATUS.md`. Do not enable or run
Linux/macOS validation from this Windows environment.

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
- [SDL pixel dimensions](https://wiki.libsdl.org/SDL3/SDL_GetWindowSizeInPixels)
- [SDL text input](https://wiki.libsdl.org/SDL3/SDL_StartTextInput)
- [SDL IME location](https://wiki.libsdl.org/SDL3/SDL_SetTextInputArea)
- [Microsoft DXC v1.9.2609](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2609)
- [xmake generic Skia package](https://raw.githubusercontent.com/xmake-io/xmake-repo/master/packages/s/skia/xmake.lua)

The generic xmake Skia package does not currently expose this Graphite/Dawn build
configuration. It is not interchangeable with the local graphics package.
