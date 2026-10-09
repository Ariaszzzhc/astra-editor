# Document core

The core is a GUI- and transport-independent C++23 library under `include/astra/core`.

`SumTree<Item, Summary, Measure>` is an immutable AVL sequence. Leaves own items and cached
summaries; branches aggregate summaries, item counts, and heights. Split/concat copy paths
and retain shared immutable subtrees. Summary composition must be associative with a default
identity. Measured seek requires a nonnegative additive projection. Diagnostic validation is
linear and is deliberately not performed on every edit.

`Rope` stores bounded UTF-8 chunks (at most 1024 bytes) in a SumTree. Chunk boundaries are
Unicode scalar boundaries. Replace, split, concatenate, and range extraction avoid flattening
unaffected text. Tree summaries cache byte counts, UTF-16 units, LF counts, and trailing-line
metrics. Adjacent small boundary chunks coalesce. Size arithmetic is checked even when
structural sharing permits enormous conceptual sequences with few physical allocations.

Raw text preserves CRLF. LF begins a new line; raw UTF-16 coordinates include CR and therefore
round-trip every scalar boundary. `line_end()` excludes CRLF/LF. LSP adapters clamp positions
to content ends. Grapheme-aware movement belongs to the Unicode command layer; scalar boundary
validation in the Rope is not a claim of complete grapheme or bidirectional layout support.

`Document` serializes mutations against a required base revision. Every edit in a transaction
uses original coordinates; validation completes before mutation. Overlapping edits and ambiguous
insertion order are rejected. Selections normalize deterministically, and all cursors produce
one history entry. Undo/redo restore persistent snapshots and selections while advancing a
monotonic revision. Saved content has a separate identity, so undoing to the saved version
correctly clears dirty state. A worker saving an earlier snapshot must mark that snapshot
saved rather than the latest text.

The design follows the measured-tree concepts in the product's references:
[Rope & SumTree](https://zed.dev/blog/zed-decoded-rope-sumtree) and
[Rope Optimizations](https://zed.dev/blog/zed-decoded-rope-optimizations-part-1).
The implementation and differential tests in this repository were written for Astra.
