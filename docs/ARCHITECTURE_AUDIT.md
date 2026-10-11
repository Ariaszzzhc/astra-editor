# Product and architecture acceptance audit

Contract: [EditorDoc.md](../EditorDoc.md). Initial source review: `c7d865c`; final
Windows review includes committed clipboard `83671c3`, history `a5273e0` and the
default-workbench increment following it, 2026-10-11. This is a requirement map
and a record of tested boundaries, not a
whole-product completion claim. Runtime evidence and failed attempts belong in
[STATUS.md](STATUS.md); source wiring alone does not prove runtime acceptance.
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
both Windows modes at this checkpoint. Physical DPI transitions, real GPU device
loss and other installed IMEs are outside that evidence. A current clean-source
Windows package with fresh extraction and isolated runtime checks is still due.
No macOS/Linux build or runtime verification was performed in this environment;
platform source/configuration is not all-platform acceptance.

Collaboration/embedded Agent UI/terminal/run/debug remain excluded by the contract.
The contract's Agent control of editor commands for testing remains in scope.
