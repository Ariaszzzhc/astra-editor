# Skia overlay boundary

This port is copied from the official Skia 148 port at vcpkg commit
`e456309491fd875487bf02b8a0dca76c1b1b00cc`. Keep its source pin, archive hash,
dependency features, upstream patches, GN invocation and generated CMake exports
aligned with that official recipe. Astra adds only `glyph-cache.patch` to PATCHES.

The adapter preserves the application's logical-order, incremental shaping cache
and failure propagation through SkParagraph. `src/ui/glyph_cache.cpp` and
`text_layout.cpp` require its RunCache and FontCollection interfaces. Real native
text-layout tests compare cached shaping with uncached output, including bidi,
context, invalidation and memory admission failures. Do not replace those tests
with source-text assertions. Patch bytes are pinned in dependencies.lock.json;
vcpkg verifies the upstream archive and incorporates the port/patch into its ABI.

Dawn uses its unmodified official port as an external dependency. CRT flags come
from the triplet; this overlay has no CRT patch or external graphics builder.
