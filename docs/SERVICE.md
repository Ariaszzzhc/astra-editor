# Service and automation

Astra owns documents on one libuv loop. The local GUI submits requests directly
to that loop; the TCP client sends the same JSON-RPC objects. JavaScript commands
call the same dispatcher. Disk operations run in libuv workers, with immutable
Rope snapshots and completion on the owner loop.

## Startup

```powershell
./scripts/xmake.ps1 build -y astra-server
./scripts/xmake.ps1 run astra-server --workspace . --port 7777
```

The server prints its actual bound port as JSON and stops on Ctrl+C. It only binds
literal `127.0.0.1` or `::1`. To use another machine, create an authenticated SSH
tunnel and connect the GUI/client to the local forwarded endpoint. Native public
listener authentication is not implemented, and an unprotected public bind is rejected.

`astra-server --connect 127.0.0.1:7777 --request '<JSON>'` sends one request and
prints its response. PowerShell callers should use single quotes around literal JSON.
Windows command-line arguments retain Unicode and literal backslashes.

## Framing and IDs

Each UTF-8 JSON body has an ASCII `Content-Length: <bytes>\r\n\r\n` header.
Limits are 8 KiB for headers and 8 MiB for a body, in either direction. Parsing
supports fragmented and coalesced frames. An incomplete frame at EOF is an error.

JSON-RPC 2.0 requests use caller-supplied IDs; notifications omit `id`. Batches
execute in order and are not atomic. String request IDs have a 256-byte limit.
The native TCP client rejects malformed or ambiguous envelopes before sending,
including duplicate outstanding request IDs. Invalid method parameters still
reach the server and receive normal JSON-RPC errors. A timeout or disconnect
fails all pending futures; an edit may already have executed, so clients must
resynchronize before retrying a mutation.

Document IDs and revisions are **canonical decimal strings**. Byte positions
are nonnegative JavaScript-safe integers at UTF-8 scalar boundaries. Metadata
includes selections, matching `selectionPositions` in zero-based line/UTF-16
coordinates, dirty state, and undo/redo availability. User movement commands
honor graphemes, including combining marks, emoji sequences, and CRLF pairs.

## Document methods

| Method | Parameters | Result |
| --- | --- | --- |
| `document.create` | optional `text` | Initial baseline metadata |
| `document.list` | none | Metadata array |
| `document.get` | `id`, optional `begin`, `end` | Metadata, `text`, `range`, `full`; at most 64 KiB of text |
| `document.lines` | `id`, `firstLine`, `count` (1–256), optional continuation `begin` | Metadata and lines with original byte positions; at most 64 KiB text |
| `document.edit` | `id`, `baseRevision`, `edits`, optional `selectionsAfter` | Metadata |
| `document.insert` | `id`, `baseRevision`, `text` | Replace every selection in one transaction |
| `document.command` | `id`, `baseRevision`, `command`, command options | Metadata |
| `document.setSelections` | `id`, `baseRevision`, `selections` | Metadata |
| `document.undo` / `document.redo` | `id`, `baseRevision` | Metadata |
| `document.close` | `id`, `baseRevision`, optional `discardChanges` | Close result; dirty close otherwise rejected |
| `document.find` | `id`, `baseRevision`, `query`; optional `begin`, `direction`, `wrap`, `select`, `caseSensitive`, `regex`, `wholeWord` | Asynchronous operation; `{id,revision,match,wrapped,document?}` |
| `document.replaceAll` | `id`, `baseRevision`, `query`, `replacement`; optional `caseSensitive`, `regex`, `wholeWord` | Asynchronous operation; `{document,replacements,changed}` |

Edits contain `{begin,end,text}` in the **same original revision**. Selection
objects contain `{anchor,head}`. Stale revisions and invalid/overlapping edits
fail before mutation. One multicursor action creates one undo entry.

Document search uses immutable snapshots on workers and the same dispatcher for
local, TCP, scripting and Agent callers. Poll with `operation.get`, cancel with
`operation.cancel`; JS `astra.request` awaits operation completion automatically.
`find` retains one match, using complete-input RE2 anchors and Unicode whole-word
boundaries. `begin` is a UTF-8 scalar boundary and defaults to the primary
selection head; `wrap` defaults true, `select` defaults false. A missing match is
`null`. With `select:true`, the original selections must still match at owner-loop
publication; otherwise the operation fails without moving the user's caret.

`replaceAll` expands captures and prepares one history transaction on a worker.
Publication rejects changed revision/content, selections, saved state, closed
documents or cancellation. It changes the buffer only; saving is a separate
operation. Both methods accept up to 64 MiB input, share workspace search syntax,
and preserve original UTF-8 byte ranges. Replacement is limited to 4096 matches,
64 KiB replacement templates and 128 MiB output. No-match and identical-text
replacements leave revision and history unchanged; all limit failures precede
publication. Cancellation is checked around bounded RE2 calls, which do not
expose an interrupt inside one call.

Client `ui.find` / `ui.replace` commands now compose these APIs in JS. They accept
optional `query`, search flags, captured `id`/`baseRevision` and `signal`; replace
also accepts `replacement`. Missing strings use the shared generic input
component. Cancellation propagates to the operation and drains late replies.
After cancellation, a late valid service rejection is reported through
`astra.on('runtime.requestDrained', receipt)` with `{method,cancelled:true,error}`;
it cannot become a new failure of the cancelled user action. Malformed replies,
transport failures and failures of the cancellation RPC still surface. Report
handlers share the runtime's existing per-poll CPU budget and drain before idle.

For a long logical line, `document.lines` returns `complete:false` and a
`continuation:{firstLine,begin}` to fetch the rest. `nextBegin` includes the line
ending; `text` excludes it. Never treat a partial read as a complete document.

Command names match `editing::Command`: movement, word/grapheme deletion,
newline, indentation, comment toggle, line duplication, and adding cursors.
Optional fields are `extend`, `tabSize`, `insertSpaces`, `commentPrefix`,
`newline`, and `text`.

## Workspace operations

Paths are UTF-8 and relative to the explicitly selected workspace. Resolution
checks canonical containment, including junctions; this is not an operating
system sandbox against simultaneous external namespace changes.

`workspace.open`, `workspace.save`, `workspace.tree`, `workspace.search`, and
`workspace.replace` and `workspace.reload` return `{operationId}` immediately. Query
`operation.get {id}` or react to `operation.completed`; result states are
`pending`, `completed`, `failed`, and `cancelled`. Failed results include an
error. Tree/search results support `offset` and `limit` pagination with
`page:{offset,nextOffset,total}`. At most eight operations run concurrently;
32 operation records are retained. Older completed records expire.

- `workspace.info {}` returns root/name.
- `workspace.documents {}` adds file paths and resolved EditorConfig properties
  to document metadata.
- `workspace.open {path}` returns document metadata. Reopening an already open
  canonical path preserves its current unsaved content.
- `workspace.save {id,baseRevision,path?}` uses the loaded disk version for
  conflict detection. Untitled buffers require a path; Save As creates a new
  file and refuses implicit overwrite. EditorConfig formatting is reflected in
  the buffer only when its saved snapshot is still current. Concurrent typing
  remains dirty and is never overwritten by the completion callback.
- `workspace.tree {includeIgnored?,maxEntries?}` returns file/directory entries
  and explicit traversal issues/limit flags.
- `workspace.search {query,wholeWord?,caseSensitive?,regex?,scope?,includeIgnored?,maxMatches?}`
  defaults to literal, case-sensitive workspace search. Dirty open files replace
  their disk content in the search; untitled buffers are included. `scope:"disk"`
  explicitly searches disk only. Results identify `source:"disk"` or
  `source:"buffer"`; buffer matches carry `documentId` and `revision` so clients
  can reject stale navigation. Byte ranges always refer to the searched text.
  Regex uses bounded RE2 syntax with Unicode case folding and word boundaries;
  captures support `$0` through `$99`, `${name}` and `$$` in replacements.
  Invalid patterns/references fail explicitly. Backslashes in replacement text
  are literal. Search cancellation, limits and unreadable files are reported.
- `workspace.replace {searchId,replacement}` preflights exact file versions,
  rejects stale buffer revisions and incomplete searches. Disk-only searches
  reject matching dirty buffers. `applied`/`pending` describe disk paths;
  `modifiedBuffers` contains metadata for unsaved, undoable buffer edits and
  `pendingBuffers` identifies concurrent edits that were preserved. Each file
  is atomic; multiple files are not one filesystem transaction. Clean open
  buffers reconcile after successful writes. Inspect issues and pending results
  before searching again; do not blindly retry an expired replacement plan.
- `workspace.watchStatus {}` reports monitor readiness, limits and errors.
  External changes reload clean buffers asynchronously. Dirty, deleted or
  unreadable files retain their text and invalidate the old saved baseline.
  `workspace.fileChanged` carries `status` (`reloaded`, `conflict`, `deleted`,
  `error`), document ID, path and revision; `workspace.watchError` requests
  attention to a monitor failure. Explicit reload remains available.
- `workspace.reload {id,baseRevision,discardChanges?}` requires `discardChanges:true`
  to replace dirty content. Completion refuses intervening edits or selection
  changes; it never silently overwrites a newer buffer.
- `operation.cancel {id}` requests cooperative cancellation. A durable save
  that has already started can still complete successfully.

## Notifications and errors

`document.created`, `document.changed`, and `document.closed` carry metadata.
TCP clients receive broadcasts; the GUI receives the same events locally.
Queues have explicit limits. Local overflow produces
`transport.resyncRequired`; a TCP peer that cannot drain writes is disconnected.
On resync, fetch documents and current paged views, then query pending operations.

Codes include `-32001` stale revision, `-32002` missing/expired object,
`-32003` unsaved close, `-32004` capacity, `-32005` response limit,
`-32011` external file conflict, `-32012` manual file recovery needed,
`-32013` conflicting operation, and `-32020` script failure. A reply-limit
error includes `handlerCompleted` and `resyncRequired`: it is not a rollback.

## JavaScript customization

`data/defaults.js` supplies complete settings and VS Code-style bindings.
Explicit `--config <workspace-relative.js>` runs user configuration afterward:

```javascript
astra.configure({editor: {tabSize: 2}});
astra.bind("Ctrl+S", "ui.save");
astra.registerCommand("custom.comment", parameters =>
    astra.request("document.command", {...parameters, command: "toggle_line_comment"}));
```

`astra.settings()` returns a JSON copy of current settings. Configuration merges
objects; commands and bindings override individually. `script.settings` exposes
the effective configuration and command names to clients, and `script.command`
executes a registered command. QuickJS memory, stack, execution time, JSON size,
and pending jobs are bounded. A poisoned Promise queue requires runtime reload;
script failure does not replace or corrupt a document.

`astra.request(method, parameters, {signal, timeoutMs, raw})` always returns a Promise.
Dispatch is deferred to an owner-loop tick; it never waits for a transport or
worker future. Service operations are awaited automatically, including paged
results, up to the JSON byte budget. Service errors reject with `code`, `message`
and optional `data`. `AbortController` and `astra.sleep(milliseconds, options)`
support cancellation, and `astra.on(event, callback)` / `astra.off(event, token)`
subscribe to service notifications. Cancellation stops queued work and requests
worker cancellation; it cannot roll back an operation that already committed.

For bounded batches, use `for await (const page of astra.operationPages(method,
params, {signal, limit:256}))`. An early iterator exit requests cancellation;
the owner loop drains that request. `{raw:true}` returns the initial operation
identity for explicit paging or replacement plans.

The GUI has a separate client policy runtime using registered `astra/ui`,
`astra/files`, `astra/search` and `astra/git` ES modules. `astra.context` is
`'client'` there and `'service'` in the document service. Shared `--config`
scripts guard client customization:

```js
if (astra.context === 'client') {
    astra.ui.registerCommand({id: 'ui.save', title: 'My save'}, async () => {
        const {document} = await astra.ui.read();
        if (!document?.path) return astra.ui.execute('ui.saveAs');
        const saved = await astra.request('workspace.save', {
            id: document.id, baseRevision: document.revision
        });
        await astra.ui.update({document: saved, status: 'Saved'});
    });
}
```

Client document calls use the same in-process/TCP protocol as other clients.
`astra.ui.read(parameters, options)` captures the requested `id` (or active
document) when invoked and asynchronously waits behind that document's preceding
native input intents before returning retained metadata. Switching tabs during
the wait does not change its document or newline policy. A read is a boundary
for prior native input, not a lock against future edits or another client.
`{immediate:true}` reads current retained presentation state without that wait;
sidebar/tab controls and the command catalog use it. Reads accept `signal` and
`timeoutMs`; cancellation drains/removes the read fence without cancelling edits.
At most 256 read futures may wait. Shutdown settles them before runtime teardown.
Clipboard policy falls back to revision-checked `document.lines` when retained
line metadata is old; `nextBegin` bounds the entire logical line even when the
text page is partial. Service byte ranges count copied bytes. The pure local
`astra.utf8ByteLength(string)` primitive measures JS strings, including embedded
NUL and supplementary scalars, without a JS character loop.
`astra.ui.execute(id, parameters)` awaits a registered UI command, including user
overrides, including replacements made directly with `astra.registerCommand`.
`astra.ui.commands()` asynchronously returns the merged client/retained service
catalog with `id`, `title`, `shortcut`, `source` and `palette` fields. Command
metadata accepts `palette:false` to hide an internal/parameterized action; built-in
`workbench.*` primitives default to hidden. The JS command palette uses the native
indexed picker, preserves the original chosen identity and awaits execution.
Built-in `editor.*` wrappers forward their parameters and await actual
service completion, including deferred script operations. Their result is the
service result, and rejection preserves RPC `code` and `data`. Advanced callers
use `astra.ui.native('editor.undo', {id, baseRevision}, {signal, timeoutMs})` for
cancellation/deadlines. Explicit revisions stay fixed; absent revisions are filled
when the captured document's intent leaves the serial queue. Cancellation skips
queued work and forwards cancellation to deferred operations; committed edits
cannot be rolled back. At most 256 native edit futures may be pending.
Input ordering is per document and continues until a deferred script operation
actually completes, fails or is cancelled. Later mutations for that document
wait; unrelated documents and viewport queries remain eligible. Saving does not
hold this input gate while asynchronous filesystem work owns its snapshot.
Remaining non-edit native workflow bridges still await their respective migration.
`astra.ui.input({title, value, placeholder}, {signal, timeoutMs})`
returns entered text or `null` on dismissal. `astra.ui.confirm({title, detail,
accept, items}, options)` returns `true` or `null`. Optional `items` are a
scrollable review list, bounded to 4,096 single-line labels, 4 KiB per label and
64 KiB combined. `astra.ui.pick({title, items, placeholder}, options)` returns the
selected original item or `null`; items can be strings or objects with a `label`.
Filtering uses ASCII case folding and literal Unicode text matching; row identity
survives filtering and scrolling. Limits match the confirmation list. Native
filtering occurs when the query changes, not on each frame.
`astra.ui.showText({title, text}, options)` awaits dismissal of an indexed text
panel, supporting arrows and Shift-wheel for horizontal scrolling. These are native retained modal
components with Promise completion; input/focus/drawing stays in C++ and the
following service workflow stays in JS. Abort dismisses the matching dialog and
drains its native future. Dialogs default to five minutes, with a separate bounded
client task deadline. UI automation distinguishes waiting for user input from
unfinished background operations. Input is bounded to 16 KiB; confirmation
details to 64 KiB. Native text panels index logical CR/LF/CRLF rows and bound their
source to 8 MiB and 100,000 lines; diff shaping caches only a bounded visible-row
working set. The clipboard adapter reads/writes at most 8 MiB and rejects
oversized/NUL text rather than silently truncating it.

`astra.ui.choose({title, detail, choices, defaultChoice, keys, id}, options)`
awaits one of 2–4 choices, returning the original string/object or `null` on
dismissal. Objects carry a `label`; optional keys are unique ASCII letters/digits
(empty entries have no shortcut). Default choice indexes are zero-based. Arrows,
Tab and Enter select; narrow windows stack the buttons. Optional `id` names a JS
choice session; `astra.ui.answerChoice(id,index)` answers only that active session.
Native identities remain unique across dialog types, and duplicate active names
are rejected before showing another component. Content/deadline/cancellation
bounds match the other modal primitives.

Client JS owns dirty-tab, captured-document and window-exit decisions.
Save/Save As return the durable save
result (or `null` on dismissal), and close awaits the current registered `ui.save`
handler. A clean saved result supplies the revision for `document.close`; new
dirty edits during save keep the buffer open. Discard uses the revision captured
before the choice, so another client's intervening edit rejects it. Cancellation,
Save As dismissal, save failure and invalid custom save results do not close the
document. `ui.saveAndClose`/`ui.discard` are hidden decision actions answering the
active close choice; the original close command owns the remaining workflow.
Save and captured-close commands accept an optional JS `signal` in their command
parameters. The signal reaches input, save and close requests; cancelling exit
also prevents a late user save result from dispatching the subsequent close.

`astra.ui.withProgress({title, detail, cancel}, work, options)` opens a cancellable
native progress component and awaits `work({signal})`. Work starts only after
the component is acknowledged. Dismissal/abort cancels the operation and drains
its late result; successful completion returns its value and dismisses the
component. Operations must honor the supplied signal to settle promptly.
The same content/deadline bounds apply as for confirmation components.

`astra.ui.waitForIdle(options)` asynchronously waits for accepted native input,
script tasks and client transport responses to settle. `astra.ui.requestClose(options)`
returns false while any tabs remain, or grants native teardown after that same
quiescence check. Both have cancellable unique native wait identities; mixed
concurrent waits settle together. They default to the normal 30-second request
timeout. These APIs coordinate this client; they do not lock other server clients.

The `astra/lifecycle` JS module closes captured tabs using the current registered
close/save handlers, checkpoints with the current `ui.checkpoint` handler and
rechecks newly arriving documents before requesting native teardown. A checkpoint
failure opens a generic Retry/Keep open/Exit anyway choice. The hidden
`ui.retryExit`/`ui.exitWithoutCheckpoint` commands answer only that failed attempt;
calling bypass during normal editing cannot close the window. Cancellation
removes the close continuation, so a late checkpoint cannot finish the cancelled
exit. Native C++ owns retained progress/choice input and physical window lifetime.

`document.syntax` accepts UTF-8 highlight `begin`/`end` bounds plus
`outlineBegin` (nonnegative, default 0), `outlineLimit` (0..1000, default 1000)
and `includeOutline` (boolean, default true). Ready responses include
`outlinePage: {offset, total, nextOffset}`; an offset beyond the end clips to the
total, and `nextOffset` is null at the end. Omitting outline rows still returns
their total. Native Outline requests its viewport range through the measured
semantic index and rejects obsolete pages for pointer selection.

`astra.ui.registerView(spec, provider, handlers = {})` registers a sidebar view.
The activity bar uses a snapshot of the registered `id`, `title`, `icon`, `order` (0..31) and
optional `outline` flag. Registration installs local policy; it performs no I/O.
At most 16 views may be presented. Activity icons are `files`, `search`, `branch`,
`braces` or `symbol`. The default Explorer, Search and Source Control use the same
native row renderer and publication path as custom views.

The async provider receives `(parameters, {signal})` and returns
`{rows, subtitle?, empty?}`. Each row has stable `id`, single-line `label`, optional
`icon`, `badge`, `tone`, `depth`, `group`, `selected` and JSON `data`.
Group descendants follow their parent in preorder; depth may increase only one
level after a group. Depth is 0..64. Native group folding uses the measured index.
`handlers.activate(data, row)` is awaited for leaf activation; late events from
replaced view generations are discarded. `handlers.show()` can customize showing
a view. Built-in providers use an async `handlers.project(model, options)` to
translate service data into these rows; extensions normally return rows directly.

`await astra.ui.showView(id)` selects a registered view, refreshing its first
presentation. `await astra.ui.refresh(id, parameters)` cancels older refreshes;
`await astra.ui.renderView(id, model, {signal?})` directly publishes a model.
Both return true for an accepted publication, false for a superseded publication;
refresh cancellation may return undefined. `astra.ui.cancel(id)` signals the
current provider locally. Providers must honor their signal for external work.

Publication uses owner-local begin/append/commit/abort identities. The JS wrapper
sends at most 128 rows and 60,000 UTF-8 bytes per page, yielding between pages;
the native boundary rejects more than 256 rows or 64 KiB per page, 100,000 total
rows or 32 MiB of serialized pages. Row ids/labels are at most 4 KiB and row data
at most 8 KiB. Malformed rows, duplicate ids, invalid hierarchy and invalid UTF-8
are rejected before altering the builder. A failed or cancelled replacement keeps
the last published snapshot interactive when rejected before publication. Native fold filtering advances at most
128 rows of work per pump, restarting if the old view's fold state changes. The
commit Promise settles only after atomic publication; shutdown cancels pending
publication fences. No production drawing backend is replaced.

```javascript
astra.ui.registerView(
    { id: 'favorites', title: 'Favorites', icon: 'symbol', order: 3 },
    async (_, { signal }) => {
        signal.throwIfAborted();
        return {
            rows: [{ id: 'readme', label: 'README.md', icon: 'document', data: { path: 'README.md' } }]
        };
    },
    { activate: ({ path }) => astra.ui.execute('workbench.open', { path }) }
);
await astra.ui.showView('favorites');
```

`astra.ui.forms.open(spec)` returns a Promise for a retained nonmodal input form.
The acknowledgement is `{id, version, values}`; it does not await dismissal or
hold a modal input task. `read(id)` returns the current state or null,
`close(id)` closes only that identity, and `update({id,version,message,busy,error})`
applies status only to that input generation, returning false for an obsolete
generation or component. Native openings receive unique epochs, including when
an extension reuses a component id. One form may be open at a time; replacing it
emits a closed event for the old identity. Existing modal inputs must be dismissed
before opening a form.

Specifications contain `id`, `title`, `fields`, `toggles`, `actions` and optional
`focus` (a field id). There are 1..4 single-line fields and actions, and 0..4
toggles. All control ids are unique across the form. Fields have `id`, `value`,
`placeholder`, `submit` and optional `shiftSubmit`; submit identities refer to
actions. Toggles have `id`, `label`, boolean `value`, and optional `shortcut`
(`Alt+` plus one uppercase ASCII letter). Actions have `id` and `label`.
Input text is bounded to 16 KiB per field; CR/LF, NUL and invalid UTF-8 are
rejected before mutation. Inputs have independent bounded Unicode editing and
undo history, shaped mouse selection, clipboard and IME candidate positioning.
Tab/Shift+Tab move focus; Enter/Shift+Enter invoke field actions, and Space/Enter
activate a focused toggle or button. Clicking outside blurs the form and allows
ordinary editor input while the component remains visible.

`astra.on('ui.form.changed', state => ...)` observes text/toggle changes;
`ui.form.action` carries the same state plus `action`, and `ui.form.closed`
carries the final state. Events are dispatched on the owner loop with a bounded
queue; consecutive input changes are coalesced. Values and versions are captured
at the event, not reconstructed later. JS owns debounce, service requests,
cancellation, results and errors; C++ owns input, focus, layout and rendering.

Default `ui.find` and `ui.replace` open this component. Query changes produce
cancellable previews without changing document selections. Explicit Previous,
Next and Replace all actions execute in order; input changes/close abort obsolete
work. Calls with an explicit `query` retain their programmatic asynchronous
result API. `document.find` accepts `direction:'forward'` (default) or
`'backward'`. Without `begin`, forward starts at the primary selection head;
backward starts at its beginning and finds a match strictly before that byte
offset. Wrapping backward selects the final match. Reverse regex search retains
one result while scanning with bounded work on a worker, preserving original
input anchors, Unicode boundaries and cancellation checks.

```javascript
astra.registerCommand("custom.save", async ({id, baseRevision}) => {
    const saved = await astra.request("workspace.save", {id, baseRevision}, {timeoutMs: 10000});
    return saved;
});
```

A `script.command` awaiting work returns a normal `operationId`; clients use the
same completion notifications and `operation.get` / `operation.cancel` semantics
as for native operations. Fast commands may return their result directly.
Runtime embedders use `start_command`, `poll`, and `take_result`; the synchronous
evaluation helpers are only for immediately settling setup/test scripts.
Registered ES modules cannot load arbitrary filesystem paths. Module setup is
synchronous; I/O belongs in async commands and event handlers.
