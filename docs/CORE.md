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

Syntax indexes retain measured semantic partitions in worker-owned immutable
snapshots. Each partition holds relative ranges, canonical persistent highlight
runs and cached byte/outline/object/node counts; parent offsets project them into
the current document without rewriting unchanged descendants. Cache candidates
must match an unchanged Rope interval, syntax child/field stamps and parent-derived
semantic context. Small token values and uncolored gaps are interned with exact
bounded keys. Update work includes changed units, direct-child enumeration and
semantic cache publication; those latter traversals are explicit costs.

`outline_range` reads bounded ordinal pages without materializing the full outline.
Enclosing-object lookup uses the containing byte partition. Full-vector adapters
materialize once on demand. `ParseOptions::incremental_analysis=false` preserves an
independent full-traversal oracle for differential tests. Error recovery reparses
from scratch to preserve canonical Tree-sitter results. Markdown block/inline
analysis has not yet migrated to these persistent semantic partitions.

Native decorations use separate measured range indexes for UTF-8 highlight bytes
and document-line diagnostic/Git ranges. Diagnostic columns remain UTF-16 until
the shaped line converts them for drawing. Range summaries compose minimum begin,
maximum end and count; half-open intersection predicates prune entire subtrees,
including indexes containing long overlapping ranges. `SumTree::for_each_pruned`
visits accepted leaves with their original ordinals; false predicates must prove
that the entire subtree can be skipped.

Publishing a range list validates it before mutation, preserves old snapshots,
and shares equal prefix/suffix ranges. Publication compares the list in O(n) and
sorts unordered input when needed; it is not a logarithmic whole-list update.
Rendering queries cached summaries instead of scanning every JSON record for
each visible row. Indexes reject negative/reversed coordinates, extent overflow
and lists above 100,000 records without replacing the retained model.
