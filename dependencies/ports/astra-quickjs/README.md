# QuickJS-NG host async context extension

Astra builds QuickJS-NG **v0.15.1**, using the pinned upstream recipe and source
SHA-256 `c4e813951b7c46845096a948e978c620b11ab4cf5fd622ca09c727ec31f42623`.
The local patch SHA-256 is recorded in the recipe and `dependencies/patches.json`;
`.gitattributes` preserves its LF bytes. The patch digest also participates in
the package configuration/cache identity. Upstream copyright and license remain
unchanged. The extension is used only by Astra's narrow script adapter.

The unmodified engine's PromiseHook does not surround ordinary promise reaction
jobs, including `await` resumption. This was verified in the fixed source and is
also described in [upstream issue 1740](https://github.com/quickjs-ng/quickjs/issues/1740).
Giving all microtasks the identity of the currently resolving RPC misattributes
continuations resumed by a different command or shared Promise. Document input
handlers can then wait on themselves, and cancellation can lose its owner.

The extension stores one **numeric host token** on the runtime, each queued job,
and each promise reaction. Reactions capture it when registered; ordinary jobs
capture it when enqueued. `JS_ExecutePendingJob` enters that captured token and
restores the previous token after the callback, including JS exception returns.
This adds no JS references, host pointers, per-Promise map, or growing token
registry. Both fulfilled and rejected reactions use the same path. Task ids stay
monotonic; retired/cancelled contexts cannot issue new host RPC mutations.

The vcpkg overlay verifies the fixed source archive and applies the patch through
vcpkg's extraction helper. CMake checks both extension symbols before configuring
Astra. An absent patch fails configuration; unmodified upstream binaries cannot
satisfy the script target.

On Windows the Debug interpreter retains `/MDd`, symbols and assertions, with
an optimized dispatch loop. Astra's C++ Debug targets retain runtime checks and
standard STL iterator diagnostics. An unoptimized interpreter exceeded the
existing 8 ms workbench budget in retained stream/icon tests; this policy keeps
that product budget unchanged rather than increasing it for Debug builds.

`tests/integration/script_tests.cpp` verifies independent/shared Promise continuations,
nested commands, host errors, cancelled late mutations and delayed operation
cancellation. Real GUI tests additionally suspend a client input handler, switch
documents, read inside the original handler, serialize later input and save.
Current Windows evidence and preserved failures are in `docs/STATUS.md`.
