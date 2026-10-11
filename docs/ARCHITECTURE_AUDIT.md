# Product and architecture acceptance audit

Contract: [EditorDoc.md](../EditorDoc.md). Initial source review: `c7d865c`; final
Windows review includes committed clipboard `83671c3`, history `a5273e0` and the
default-workbench increment following it, 2026-10-11. This is a requirement map
and a record of tested boundaries, not a
whole-product completion claim. Runtime evidence and failed attempts belong in
[STATUS.md](STATUS.md); source wiring alone does not prove runtime acceptance.
Latest requested acceptance is Windows only, explicitly set by the user on
2026-10-11. All executable implementation/test obligations in that scope passed;
visual-quality judgment belongs to the user. Unavailable physical DPI/driver/
adapter tests and macOS/Linux runtime checks are documented limits, not blockers
to this requested task. Current runtime/package identity is `bfd122e`; the final
corrected Debug isolated clipboard rerun is recorded in `STATUS.md`.
Clipboard/request corrections below refer to the tested increment whose parent
is `efa8a24`; process-isolated clipboard work has parent `e3b275d`, with exact
commands and limitations in `STATUS.md`.

## Responsibility boundaries

| Responsibility | Current source authority | Acceptance obligation |
| --- | --- | --- |
| Text, Unicode coordinates, selections, atomic edits/history | `include/astra/core/`, `src/core/`, `src/editing/` | Persistent snapshots, bounded chunks, grapheme movement, stale/overlap rejection and complete multicursor history |
| Documents/workspace, parsing, I/O and external services | `src/service/`, `src/workspace/`, `src/language/`, `src/process/` | Service-owned state, asynchronous work, cancellation, failure preservation and shutdown |
| User scripts and asynchronous requests | `src/script/runtime.cpp` | Deferred Promise dispatch, task identity across continuations, bounded queues, AbortSignal/timeouts, late-response draining and observable errors |
| Product commands, labels, keymaps and default settings | `data/defaults.js`, `data/workbench/commands.js`, `workbench.js` | User overrides reach actual input/menus and the shared dispatcher |
| Files, clipboard, search, Git, language, recovery and exit workflows | `data/workbench/files.js`, `clipboard.js`, `search.js`, `workspace-search.js`, `git.js`, `language.js`, `recovery.js`, `lifecycle.js` | JS owns workflow decisions; native/service adapters expose mechanisms |
| Sidebar/Outline/chrome policy and file icon associations | `data/workbench/api.js`, `outline.js`, `chrome.js`, `icons.js` | JS providers publish bounded immutable generations; retired state cannot activate |
| Window/input, shaping, retained layout and drawing | `src/ui/renderer.cpp`, `editor_window.cpp`, component files under `src/ui/` | SDL3 + Skia Graphite/Dawn, common paint/hit geometry, responsive input and exact Unicode/native glyph coordinates |
| In-process and remote clients | `include/astra/ui/editor_channel.hpp`, `src/ui/workbench.cpp`, `src/service/service_host.cpp`, `src/transport/` | Common service requests/responses/notifications; local `_ui.*` primitives belong to the client window |

C++/JS acceptance is about ownership and actual call paths. A registered JS
wrapper around an entire native product workflow is insufficient evidence of
policy ownership. Conversely, Unicode editing, native geometry, protocol
validation and drawing do not need to move to JS merely to change line counts.

## Ordered persistent data

### Neovim responsibility comparison

Checked upstream documentation/source on 2026-10-11; these links track upstream
`master`, not a pinned Astra dependency. Neovim's boundary is a useful comparison,
but its language percentages are not an acceptance target for Astra.

- The C implementation of
  [buffer APIs](https://github.com/neovim/neovim/blob/master/src/nvim/api/buffer.c)
  owns live buffer access, coordinate/range validation, modification and undo
  integration. Lua invokes those mechanisms through `vim.api`; it does not
  replace their state ownership.
- [Lua's programming interface](https://github.com/neovim/neovim/blob/master/runtime/doc/lua.txt)
  combines its standard library, editor functions/commands and the native API.
  User configuration and plugins load as Lua modules, alongside retained
  Vimscript facilities. High-level built-in code also lives in Lua: the
  [LSP RPC implementation](https://github.com/neovim/neovim/blob/master/runtime/lua/vim/lsp/rpc.lua)
  owns request identities/callbacks, protocol messages and response dispatch
  over a transport abstraction.
- `vim.uv` exposes libuv networking/filesystem/process mechanisms. Its callbacks
  must schedule editor API calls when they are not fast-event-safe. This is an
  execution-context boundary, not a rule that every Lua API returns a Promise.
  Neovim exposes its API through Lua, Vimscript and
  [remote RPC](https://github.com/neovim/neovim/blob/master/runtime/doc/api.txt).

For Astra, product command composition, close/save/reload decisions, keymap and
provider publication belong in shipped JS. Persistent documents/transactions,
service ownership, Unicode/shaping, native handles and bounded asynchronous
operations belong in C++. The user's asynchronous JS requirement additionally
requires Promise dispatch with cancellation/deadlines and responsive native
execution; copying Neovim's scripting syntax would not establish that behavior.
The responsibility table above maps the actual Astra source owners.

The following measured persistent structures are present in the production
sources. Review must cover their use in seek/edit/publication paths, not just
the presence of a `SumTree` declaration.

| Domain | Source | Costs that must remain explicit |
| --- | --- | --- |
| Rope | `include/astra/core/rope.hpp`, `sum_tree.hpp` | Bounded UTF-8 chunks; logarithmic structural paths do not make returned text copying free |
| Undo/redo history | `include/astra/core/document.hpp`, `src/core/document.cpp` | Entry-count summaries, shared immutable leaves and roots; cloning copies current selections, and releasing unreferenced history still costs work |
| Syntax semantic partitions and highlight runs | `src/language/syntax.cpp`, `include/astra/language/syntax.hpp` | Changed parse contexts, wide-parent traversal, error recovery and cache publication can still require linear work |
| Explorer, provider views, pickers and tabs | `file_tree_index.hpp`, `view_index.hpp`, `picker_index.hpp`, `tab_index.hpp` under `include/astra/ui/` | Full enumeration/filtering, input validation and tab order relabelling are distinct from measured seeks |
| Document rows, decorations, text panels and metadata extents | `viewport_index.hpp`, `range_index.hpp`, `text_line_index.hpp`, `document_metadata.hpp` under `include/astra/ui/` | Bounded page publication/comparison, list sorting and complete serialization are explicit work |
| Native glyph runs/sequences | `src/ui/glyph_cache.cpp`, `glyph_sequence.hpp` | Exact identity/shaping context and bounded local splicing; unsupported changes require canonical shaping |

Identity maps, bounded transport queues and fixed-size widget controls serve
different purposes from ordered measured sequences. Replacing those blindly
would not demonstrate the contract's indexed-data philosophy. Existing core,
syntax, presentation, viewport and tab differential tests are evidence for their
specific invariants; they do not establish an O(log n) whole-editor update.

## Clipboard and request findings

1. **Original failure handling was insufficient.** At the reviewed checkpoint, the
   `_ui.clipboard.read` branch in `src/ui/editor_window.cpp` checks only a null
   `SDL_GetClipboardText()` result. SDL documents an empty string on failure and
   directs callers to `SDL_GetError()`. Implement explicit error handling and a
   real failure-path test that preserves document content/selections. The current
   Windows revision uses the narrow `clipboard_text.cpp` adapter: explicit
   open/read/lock/conversion/close checks, bounded strict UTF-16 to UTF-8 decoding,
   and RAII cleanup. Empty paste is a no-op. The real owned-window contention
   fixture verifies JS rejection plus editor/sidebar/modal content preservation.
   See `STATUS.md` for exact snapshot evidence and retained fixture failures.
2. **Promise return did not prove clipboard responsiveness.** The previous native
   read/write executed on the GUI owner loop. The checked reader alone did not
   prevent delayed-provider blocking. `clipboard_client.cpp` now serializes
   bounded operations in owned `astra-clipboard-host` processes. SDL/native work
   stays on each helper's main thread; the GUI polls ready replies and process
   completion. Host cancellation works even after whole-task cancellation;
   successful ownership remains alive until superseded/shutdown. The dedicated
   real Windows delayed-provider fixture proves input/render progress, Unicode,
   AbortSignal/deadlines, queued field input and bounded exit in both modes.
   This specific finding is resolved by the evidence in `STATUS.md`; no other
   platform runtime acceptance is inferred.
3. **Original public request options were inconsistent.** `astra.request` supports signal
   and timeout options, but `ui.update`, the form wrappers and clipboard wrappers
   in `data/workbench/api.js` do not forward caller options. Add consistent
   forwarding and verify cancellation before publication/dispatch. Existing task
   cancellation is useful but does not replace per-call public options. The
   wrappers now forward options. Behavior tests cover all ten methods with
   already-aborted and queued-aborted calls, per-call deadlines and discarded
   late native replies. This establishes runtime request behavior, not native
   clipboard latency or rollback after native acceptance.
4. **Prepared history previously copied all entries.** The deque representation
   copied every historical selection vector during `Document::clone()`, despite
   sharing Rope nodes. Undo/redo now use persistent entry-count measured SumTrees;
   cloning shares both roots and copies only current selections. Split/concat
   transfer existing immutable leaves, and all new roots/states are prepared
   before publication. Dynamic-limit differential tests retain and independently
   replay clones; Unicode multicursor tests cover trimming, saved identities,
   rejected late selections and branches. Exact Windows checks and the real
   before/after history-clone benchmark are recorded in `STATUS.md`. Dropping
   unreferenced nodes is still proportional to the released history; this is not
   a claim that freeing arbitrary history or whole-editor updates are O(log n).

API contracts used in this review:
[SDL_GetClipboardText](https://wiki.libsdl.org/SDL3/SDL_GetClipboardText) and
[Windows clipboard ownership/delayed rendering](https://learn.microsoft.com/en-us/windows/win32/dataxchg/clipboard-operations).
These sources establish API behavior, not measured latency in Astra.
The fixed [SDL 3.4.12 Windows backend](https://github.com/libsdl-org/SDL/blob/release-3.4.12/src/video/windows/SDL_windowsclipboard.c)
also has open-failure paths returning empty text without setting an SDL error;
checking `SDL_GetError()` alone cannot establish Windows contention detection.

## Product scope and remaining proof

The requirement-by-requirement Windows evidence below includes the final
`scripts/test.ps1 -Mode all -Gui -InstalledIme` concurrent-loss regression and
earlier recorded benchmark acceptance in `STATUS.md`, not a list of unexecuted
test names. Source review checked the
dispatcher/JS ownership table, real transport equality, Unicode/history
assertions, default key bindings and icon/configuration cases. All 632 behavior
cases ran in each mode with no skipped cases. GUI and package checks run real
production adapters in separate fixtures. These results establish the indicated
Windows behaviors, not unrestricted production or all-platform readiness.

| Explicit requirement | Authoritative implementation/evidence | Current result |
| --- | --- | --- |
| C++, JS and xmake | `xmake.lua` selects C++23 and pinned QuickJS-NG; `script_tests.cpp` evaluates Unicode, overrides and actual service edits | Windows verified |
| SDL3, Skia Graphite, Dawn | `renderer.cpp` creates the real native/Graphite/Dawn context; graphics/renderer/font tests, four idle/active and eight concurrent actual owned Dawn losses; callback/recovery/readback and reproduced classification-race correction | Windows Debug/Release verified on this GPU; physical driver reset/adapter removal unverified |
| Tree-sitter highlighting | `syntax.cpp`, grammar manifest; real grammar/syntax tests and GUI syntax-object/highlight workflows | Windows verified |
| libuv asynchronous I/O | Transport/service/process adapters; real socket/worker tests prove input-loop progress, errors, cancellation and owned shutdown; real dedicated delayed-clipboard fixture | Ordinary and isolated Windows Debug/Release verified |
| Rope and persistent measured SumTrees | Core snapshot/differential tests check summaries, chunk bounds, UTF coordinate round trips; indexed presentation tests and measured history clone benchmark | Windows verified for stated invariants/costs |
| Client/server, in-process and remote semantics | `tcp_tests.cpp` compares identical local/remote responses and notifications; actual GUI local and `--tcp` workflows | Loopback TCP verified; arbitrary remote-network environments unverified |
| C++ core, JS high-level customization | Shipped `data/workbench/` workflows and user command/provider/keymap override tests; extracted production GUI accepts workspace-relative user JS | Windows verified; language percentages are not a gate |
| Asynchronous public JS API | Promise/AbortSignal/deadline/late-reply tests, responsive native GUI workflows; public form/clipboard/update forwarding; final two-mode delayed-provider cancellation/exit | Windows verified for stated bounds and behaviors |
| Agent command control | GUI fixture `execute_ui_command` and native events use production JS/shared dispatcher; service RPC/CLI tests | Windows verified; no parallel fake document model |
| VS Code flavored default UI | Nine shipped-default real states, native screenshots, fonts/forms/pickers/modal/error and resize tests | Windows functional/layout acceptance passed; visual-quality review owned by user |
| Complete editing, multicursor | GUI Unicode/multicursor/undo/redo/save/reopen/dirty-close; atomic stale/overlap rejection and history tests | Windows verified for exercised behaviors |
| Textobjects | Unicode word/grapheme editing tests and real GUI function/syntax-object selection | Windows verified |
| File tree and workspace | Actual 5,005-file indexed Explorer, collapse/scroll/activation and temporary-filesystem workspace tests | Windows verified |
| Global search and replace | GUI workspace search/replacement, untitled/unsaved results, external conflict and guarded reload; application failure-path tests | Windows verified |
| LSP and Outline | Real packaged clangd/TypeScript/Python diagnostics, GUI completion/hover/navigation/references/rename/formatting and indexed Outline | Windows verified with bundled defaults |
| Git | Bundled real Git and GUI diff/stage/unstage/commit/guarded-discard; sanitized fixture environment | Windows verified |
| VS Code keymap | `data/defaults.js` shipped bindings, actual GUI SDL key input and JS override tests | Windows verified |
| EditorConfig | Parser/glob/layer/unset/save-policy tests plus application integration | Windows verified |
| Request/response and notifications | Framing/RPC/TCP tests cover fragmentation, malformed/EOF/bounds and notification equality; reordered GUI notification tests | Windows verified |
| File icons and themes out of box | Shipped descriptors/themes, icon identity/atomic-validation/bounds tests and inspected default captures | Windows verified |
| Formatting, tests, coherent commits and push | Pinned formatter gate, full two-mode Windows log, Git commit history and upstream equality; `AGENTS.md` and repository skills exist | Current gates satisfied |
| Deployable Windows artifact | Clean `bfd122e` ZIP inventory/hash, fresh extraction, minimal-PATH real Git/LSP/recovery, production GUI Unicode edit/undo/redo/save/close; 163 runtime source and eight Release input hash comparisons | Windows checks passed for recorded scope |
| macOS/Linux, including named Linux distributions | Native source/package paths are retained; no builds/emulators/tests launched on this host | Not verified; excluded from this task's validation by user |

Collaboration, an embedded Agent product UI, terminal, run and debugger remain
excluded; the testing Agent uses the existing command API. Name selection is
already resolved as Astra. Production-grade robustness across all requested
platforms cannot be inferred from the above Windows acceptance or declared
complete from Windows results alone. The user's requested Windows task does not
require that all-platform acceptance claim.

Editing/multicursor/textobjects map to core/editing and JS commands; file tree
and workspace to JS files and service workspace; global search/replace to JS
search and shared search services; LSP/Outline to JS language/Outline and native
language services; Git to JS Git and the process/service adapter; VS Code
keymaps, icons/themes and defaults to shipped JS policy; EditorConfig to the
native configuration service; request/response/notifications to the shared
protocol/transport. Their existing tests and GUI workflows must be checked
against the final source snapshot before release acceptance.

The final integrated entry point passed 632 behavior cases with no skipped cases
in each Windows mode, plus native local/TCP GUI workflows and real services.
The new default visual fixture exercises nine production states with shipped
providers/theme/fonts: editing/Explorer/Outline, palette, quick open, document
find, workspace search, source control, diff, dirty-close and narrow resize.
Native captures were inspected; this is concrete default-state acceptance,
not a claim that visual quality can be inferred from test counts.
Real installed Chinese IME document, editor/sidebar form and modal input passed
both Windows modes at this checkpoint. Owned native Dawn loss/callback/recovery
now has two-mode four idle/active and eight concurrent-loss acceptance, including
the reproduced classification-race correction. Physical DPI transitions, driver resets/adapter
removal and other installed IMEs are outside that evidence. Clean-source Windows
package `bfd122e` passed inventory, ZIP hash, fresh extraction, isolated service
runtime and the extracted production GUI's JS edit/undo/redo/save/close workflow.
Final-source delayed-provider rechecking passed Release and the corrected Debug
rerun, each with its actual owned process, newly generated log and exit zero.
Ordinary Debug/Release GUI/full regression passed. Earlier fixture results
remain explicitly attributed to their recorded source snapshot. Current runtime
source and Release binary hashes match the clean `bfd122e` package; exact
commands, failed launch attempts and evidence are in `STATUS.md`.
No macOS/Linux build or runtime verification was performed in this environment;
platform source/configuration is not all-platform acceptance. The user confirmed
there is no macOS/Linux acceptance host and explicitly excluded that validation
from this task. Physical DPI transitions, driver resets/adapter removal and
other installed IMEs remain limits of the evidence, not claimed passes or
blockers under the user's scope. No executable Windows requirement in the matrix
has an unresolved implementation/acceptance failure.

Collaboration/embedded Agent UI/terminal/run/debug remain excluded by the contract.
The contract's Agent control of editor commands for testing remains in scope.
