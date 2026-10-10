# Astra native shaping adapter

Skia remains pinned at `525f94ac3418fd0b0dae42caa220422a94b18b49`.
The maintained `glyph-cache.patch` exposes a per-font-collection HarfBuzz callback.
It retains native itemization, font fallback, bidi reordering, layout and drawing.
The callback receives logical-order glyph IDs, original UTF-8 clusters, advances,
offsets and the genuine HarfBuzz unsafe-to-break/unsafe-to-concat flags. Uncached
subranges use the original full context, font, features and native iterators.

The unsafe-to-concat buffer flag only requests metadata. The adapter does not
insert style seams, truncate context, change fonts or approximate shaping.
The original pinned Git objects remain intact. `scripts/apply-skia-patch.py`
verifies every upstream/output blob and rejects unrelated or partially applied
changes. Build manifests and xmake require the exact patch digest. Dependency
and runtime provenance include the reviewed patch and its manifest.

The current collector captures immutable measured glyph runs, sharing exact
matches only when full context, range, typeface identity, font settings, bidi,
script, language, features and tracking agree. It stores no typefaces or mutable
native collections. Capture admits at most 1,024 runs / 4 MiB of accounted run,
text, feature and glyph payload; tree/container allocation is additional.
Changed text or a worker's different native typeface identity safely reshapes.
This is an adapter and exact-run cache, not local-edit glyph splicing. Native
unsafe-to-concat boundaries alone do not certify a newly shaped splice; both
old and new boundaries must be checked before implementing that path.

Skia has no C++ exception unwinding. Astra's callback is `noexcept`: it retains
the first adapter error, retires further callbacks and reports it immediately
after native layout returns. The adapter stops before emitting failed buffers,
and SkParagraph aborts its shaping pass. Failed geometry is never published. Native
allocation failures inside Skia retain upstream's allocation behavior; this
does not establish recoverable native out-of-memory acceptance.

Windows reproduction:

```powershell
./scripts/build-graphics.ps1 -Configuration Debug -Jobs 4
./scripts/build-graphics.ps1 -Configuration Release -Jobs 4
./scripts/xmake.ps1 f -m debug -y
./scripts/xmake.ps1 build -y -j 2 text-layout-tests viewport-layout-tests astra
```

Native-host build wiring is preserved but is not validation of Linux/macOS.
