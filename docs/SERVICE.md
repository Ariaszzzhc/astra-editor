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

Edits contain `{begin,end,text}` in the **same original revision**. Selection
objects contain `{anchor,head}`. Stale revisions and invalid/overlapping edits
fail before mutation. One multicursor action creates one undo entry.

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

`document.syntax` accepts UTF-8 highlight `begin`/`end` bounds plus
`outlineBegin` (nonnegative, default 0), `outlineLimit` (0..1000, default 1000)
and `includeOutline` (boolean, default true). Ready responses include
`outlinePage: {offset, total, nextOffset}`; an offset beyond the end clips to the
total, and `nextOffset` is null at the end. Omitting outline rows still returns
their total. Native Outline requests its viewport range through the measured
semantic index and rejects obsolete pages for pointer selection.

`astra.ui.registerView(spec, provider)` supplies an async model provider;
`refresh(id, parameters)` cancels older refreshes and only the current generation
can publish. Current native sidebar model kinds are `explorer`, `search`, `git`.

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
