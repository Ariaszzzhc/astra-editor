# Windows core measurements

Measured on 2026-10-09 with the repository's MSVC x64 Release build, C++23 and
`/MT`. Run `./scripts/xmake.ps1 build -y astra-core-benchmark`, then
`./build/windows/x64/release/astra-core-benchmark.exe` to reproduce the workload.
`scripts/test.ps1 -Mode release -Benchmarks` also includes it.

Each size contains repeated source lines with Chinese text, a supplementary
emoji and a combining sequence. The fixed seed `0x4153545241504552` chooses
1,000 scalar-aligned insertion positions. Every edit retains its previous
snapshot and full undo entry. Timings separately measure document construction,
an atomic edit, exact incremental difference, and a byte/UTF-16 position round
trip. Final checks validate the tree, the retained original snapshot and all
1,000 undo steps back to the clean original document.

| Input | Load ms | Edit P50 µs | Edit P95 µs | Edit P99 µs | Diff P95 µs | Position round trip P95 µs | Undo mean µs | Tree height |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 MiB | 1.9578 | 6.0 | 9.2 | 32.5 | 5.2 | 3.1 | 0.2547 | 12 |
| 8 MiB | 16.1456 | 6.7 | 10.0 | 48.3 | 4.0 | 3.3 | 0.3643 | 15 |
| 64 MiB | 130.548 | 8.0 | 11.3 | 51.8 | 5.1 | 3.2 | 0.3046 | 18 |

These are real core operations, not GPU-frame or language-server latency. The
benchmark reports measurements and fails correctness checks, without a brittle
machine-specific timing threshold. GUI acceptance separately covers 70,000-byte
lines, Unicode layout, editing and horizontal navigation. No Linux/macOS
performance measurements were run in this environment.
