---
name: astra-validation
description: Build, test, and record Windows acceptance evidence for Astra, including transport parity, GUI automation, recovery, and release packaging.
---

Read `AGENTS.md`, `docs/BUILD.md`, and `docs/STATUS.md`. Execute only Windows validation in this
environment. Do not infer non-Windows support from a successful Windows build.

Use the pinned CMake/Ninja toolchain, vcpkg submodule and repository-local caches. Fail the build when a required
dependency or capability is missing; do not silently substitute a renderer or disable tests.
Use CMake presets and CTest labels. During migration, earlier xmake artifacts are historical
comparison evidence only. Validate the new build before removing the old entry points.
Install only production targets/resources, then verify and package that installation.

Choose tests from the changed behavior: core differential tests; real temporary-filesystem
save/recovery tests; fragmented transport and cancellation tests; real grammar/LSP tests;
or Agent-driven GUI workflows. Mocks isolate failure paths but cannot establish integration.

At milestone boundaries run Debug and Release suites. For GUI acceptance exercise startup,
actual text entry, multicursor changes, undo, save/reopen, dirty close, and renderer shutdown.
Agent control must invoke the production command dispatcher, not a parallel fake editor.

Record commands, outcomes, platform, and unresolved limitations in `docs/STATUS.md`. Preserve
failure artifacts under `build/`. Mark only tested capabilities complete and use short coherent
conventional commits. Do not publish a production-ready claim while required milestones remain.
