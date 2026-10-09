---
name: astra-core
description: Implement or review Astra's persistent measured trees, Unicode text model, selections, transactions, and history. Use for document core changes in this repository.
---

Read `AGENTS.md` and the affected public interfaces first. Keep buffer state independent of
GUI and transports; all callers must observe the same transaction semantics.

For measured trees, identify the summary identity and associative composition. Test stored
summaries against a flat reference after random insertion, deletion, split, and concatenation.
Retain old snapshots during randomized edits and prove they remain unchanged. Check balance
and chunk limits, including adversarial insertion at the same boundary.

For text, explicitly name the unit of each coordinate. Reject malformed UTF-8 and mid-codepoint
edits. Cover empty text, trailing newline, CRLF handling, emoji/supplementary characters,
combining sequences, and line/UTF-16 round trips. Grapheme movement belongs in Unicode-aware
commands, not in the raw byte-oriented tree.

For editing, validate the full transaction against its base revision before applying anything.
Normalize selections deterministically; one multicursor edit is one history entry. Preserve
pre/post selections, saved-state identity, branching history, and redo invalidation.

Run affected tests and deterministic randomized differential tests. Report reproducible seeds
for failures. Benchmark only real implementations; no full-buffer string rebuild on each edit.
