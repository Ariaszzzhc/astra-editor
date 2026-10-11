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

Document metadata also includes `metadataSequence`, a positive canonical decimal
string within the unsigned 64-bit range. It identifies a published metadata
snapshot within one service lifetime, separately from the text `revision`.
Selections, saved state and file bindings can change without a text edit. Compare
these markers numerically and per buffer; do not convert them to a JavaScript
`Number`, compare their strings lexicographically, or use another buffer's marker
as a floor. A new service connection needs a fresh client snapshot.

Unchanged core metadata reads and their corresponding notification/response retain
the same marker. A changed core tuple receives a newer marker; saving/reloading a
file binding also invalidates the previous binding identity. Prepared operation
results retain their candidate marker separately until publication, so abandoned
preparation cannot replace the already published tuple. Failed preparations may
leave sequence gaps: this value is neither an edit count nor a text revision.
The service retains at most one published and one prepared core tuple per clock
record, without document text, and forgets closed buffers. Existing operation and
notification limits bound unpublished batch IDs; this is not a heap-size bound.

The native GUI rejects older metadata for a retained buffer before changing its
tabs, selections or JS event delivery. Both revision and sequence must be at least
the accepted values, and an equal sequence must retain the same revision. An event
for the exact currently accepted snapshot still reaches JS even if a read arrived
first. Client `astra.ui.read` and native `WorkbenchViewState.document` expose the
accepted document metadata.

The GUI also retains closed buffer IDs for its connection. Service IDs are not
reused within that lifetime: a same-path reopen creates a fresh ID. Persistent
measured ranges merge adjacent closed IDs and support logarithmic lookup/update
in the number of ranges, preserving retained snapshots. Sparse close histories
can still grow; there is no fixed heap bound or expiry that allows resurrection.
Late metadata responses, creation and change events for a retired ID cannot
create a tab, activate it or reach the corresponding JS metadata listener.
`ui.update({closedDocument:id})` records an acknowledged close; ordinary workbench
commands send this only after `document.close` succeeds.

A background `document.lines`, `document.syntax` or `language.diagnostics` reply
with error `-32002` and an object `data.id` matching its requested document also
confirms retirement. This reply may beat the close notification on another
connection. The GUI removes that document without reporting an action failure;
later duplicate close observation is idempotent. Other error codes, unrelated
IDs, transport failures and edit failures retain their normal error handling.

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
  Results include `fileCount`, the number of distinct matching paths (including
  untitled-buffer labels) in the complete retained result. Every paged response
  repeats this same global count; `page.total` counts matches, not files.
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

The client loads the shipped `data/defaults.js` policy before its workbench and
user script. Default settings are retained separately: effective startup settings
merge shipped defaults, service preferences, then explicit client preferences.
An explicit client value equal to the default still overrides a different service
value. C++ compiles a complete theme/font candidate before publishing it; the
window has no second product palette or UI font-family preset.

`astra.ui.style.read(options)` and `astra.ui.style.update(patch, options)` return
Promises for the effective `{theme, workbench}` presentation settings. Updates
apply only to this window and do not edit documents, persist service settings or
change another client's presentation. Use `astra.configure` in the user startup
script for persistent preferences. The style read is the authority for current
presentation; `astra.settings()` describes configuration in its own runtime.

```javascript
const previous = await astra.ui.style.read();
await astra.ui.style.update({
    theme: { panel: '#252526', tabs: '#181818' },
    workbench: {
        uiFontSize: 14,
        uiFontFamilies: ['Segoe UI', 'Microsoft YaHei UI', 'Segoe UI Emoji']
    }
}, { signal, timeoutMs: 30000 });
await astra.ui.style.update(previous);
```

Updates accept partial `theme` and `workbench` objects. Unknown fields, deleted
required roles, malformed `#RRGGBB` colors and invalid font values reject without
publishing any part of the candidate. Theme roles include separate `panel`,
`tabs`, and `syntaxError` colors. Workbench style fields are `uiFontSize` (9–16),
`sidebarWidth` (finite and clamped to 180–480), `uiFontFamilies` and
`uiMonospaceFontFamilies` (1–8 nonempty UTF-8 names, at most 128 bytes per name).
Body text, titles, labels and editable inputs scale their reference sizes relative
to the same 13-unit UI base. Document font configuration remains under `editor`.

Live updates enter a bounded native queue after structural/name/family validation.
Already accepted pointer/text input finishes using its captured font geometry
before an update publishes. Modal input may remain open. Cancellation removes
unpublished updates; shutdown settles pending native futures without publication.
The queue has a 30-second native deadline in addition to the request timeout.

On Windows, startup and accepted live themes also apply `activityBar`,
`foreground` and `border` to the native caption, caption text and window border.
The native adapter uses the compiled palette; it has no separate product theme.
`await astra.ui.read({immediate:true})` reports read-only `windowFrame` support
flags: `darkMode`, `captionColor`, `textColor` and `borderColor`. A flag records
whether the OS accepted that attribute, rather than its current color or the
window's dark-mode value. Unsupported attributes stay under OS control. Other
native failures reject the update and attempt to restore all changed attributes;
rollback failures are reported too. Invalid and cancelled unpublished updates
leave the frame unchanged. The style snapshot remains `{theme, workbench}` and
can still be passed directly to `style.update` to restore presentation.

`astra.ui.icons.read(options)`, `resolve(paths, options)`, `update(patch, options)`
and `replace(theme, options)` return Promises. They operate on this window's
effective file icon theme, independently of document content and service state.
Startup preferences use `astra.configure({fileIcons: {...}})`. A live icon update
survives a later startup-service preference reply; it does not persist to disk.

```javascript
const previous = await astra.ui.icons.read();
await astra.ui.icons.update({
    extensions: { '.d.ts': { glyph: 'symbol', tone: '#75BEFF' } },
    filenames: { 'package.json': { glyph: 'braces', tone: 'added' } }
}, { signal, timeoutMs: 30000 });
const icons = await astra.ui.icons.resolve([
    { path: 'src/index.d.ts', directory: false },
    { path: '.git', directory: true }
]);
await astra.ui.icons.replace(previous.theme);
```

Reads/updates return `{theme, generation}`; resolution returns
`{generation, items: [{path, directory, icon: {glyph, tone}}]}`. Themes require
`fallback`, `folder`, `filenames`, `extensions` and `directories`. Filename and
directory-name matches ignore case. Exact filename rules take precedence over
the longest matching compound extension; directories use their own name table.
Partial updates use JSON merge-patch semantics: `null` removes an association.
`replace` restores the complete captured theme, including removal of subsequently
added rules. Concurrent stale updates reject with `-32001` before publication.
Invalid candidates reject before changing the theme. Cancelling an outstanding
read prevents later publication; cancellation cannot undo an accepted update.

Drawing glyphs are `files`, `search`, `branch`, `settings`, `folder`, `document`,
`code`, `braces`, `symbol`, `function`, `refresh` and `warning`. Tones are
`muted`, `foreground`, `folder`, `codeIcon`, `scriptIcon`, `accent`, `added`,
`changed`, `error`, `warning`, or a literal `#RRGGBB`. Palette role colors follow
the current UI theme without rerunning filename associations. Association tables
admit at most 256 entries each; keys admit 128 UTF-8 bytes. Resolution admits
256 paths, at most 32 KiB UTF-8 per path and 256 KiB in total. Long-path validation
yields after bounded chunks and checks cancellation. Themes/patches admit at most
65,536 serialized JavaScript string units; the native copy is capped at 256 KiB.

Visible tabs and Explorer rows share a native descriptor cache. It admits 2,048
entries and 4 MiB of accounted identity bytes (map/list/queue storage adds bounded
overhead), queues at most 256 identities and runs one batch of at most 32 paths /
64 KiB. C++ translates glyph/color values and draws; JS alone owns associations.
Changing icon themes preserves previous descriptors while refreshing visible
paths, discards obsolete replies, and does not rescan the filesystem or invalidate
document paragraphs. Failed batches preserve old descriptors, report the error
and suppress repaint retries until the theme generation changes. Custom view rows
can opt into the same policy with `iconPath` and `group` (directory); ordinary
`icon` rows retain their existing explicit glyph semantics.

`astra.ui.selectObject(kind, parameters = {}, options = {})` is an asynchronous
client workflow. Kinds are `function`, `class`, `comment`, `string`, `block`,
`parameter` and `heading`; parameters accept `id`, `baseRevision`, `inner` and
`expand`. The command captures a fenced document read and its selections, requests
current syntax without downloading highlights/Outline, then awaits any parser
operation through the shared service. Cancellation/timeouts prevent a later
selection request. Unsupported or obsolete syntax returns `null` with a status;
invalid flags reject and service failures remain observable.

Syntax operation descriptors include `shared:true`: the service caches one parser
job per document revision for all callers. Aborting or timing out an automatic JS
wait drains its outstanding response without sending `operation.cancel`, including
when the initial descriptor arrives after cancellation. Owned operation descriptors
omit this marker and retain automatic cancellation. An explicit `operation.cancel`
still requests service-job cancellation; clients should not use it to abandon only
their wait on shared syntax.

The resulting `document.selectObject` request includes optional `baseSelections`:
a nonempty array of at most 4096 `{anchor, head}` JavaScript-safe UTF-8 byte
integers matching the current selections exactly. A mismatch rejects with stale
state code `-32001` before changing any selection. This guard is separate from
`baseRevision`, because moving a cursor does not increment the text revision.
Legacy service requests without that optional guard select from the current
selections at dispatch. The same checks run for in-process and TCP clients.

Default Select Function/Class/Block/String commands are registered in JS;
String defaults to inner content. Users can override a command or call the same
API with explicit flags. Publication uses `revealCaretFor: document.id`, an
owner-loop UI update that reveals only if that document is still active. An
async selection result cannot activate or scroll a different current document.
The native command-result adapter also checks the returned document identity
before revealing a caret, including after the JS completion callback.
Duplicate native tab/sidebar/Outline/Explorer and textobject policy branches
were removed; these product commands must resolve through the JS registry.

Native tab order and cached logical widths now use a persistent measured
`TabIndex`. Identity lookup and ordinary sequence/width updates follow logarithmic
paths. Active reveal seeks cached width prefixes; layout visits only the bounded
visible range. Exhausted order-label gaps trigger a bounded full relabel, rather
than losing identity ordering. There is a 65,536-tab model admission bound.
Font/DPI invalidation advances one generation without rebuilding every entry.
Previous widths remain provisional until each caption is refreshed, with at most
eight measurements and a four-millisecond dispatch budget per paint. This is
not a worst-case execution-time bound for one caption. Visible captions receive
priority; width preparation happens before painting, not during pointer dispatch.
Repainting valid geometry does not measure all offscreen captions. Explicit tab
enumeration/serialization still costs O(n).

`astra.ui.read({immediate:true, tabOffset, tabLimit})` can additionally return
`tabDocuments`, compact `{id, revision, dirty}` metadata in native tab order.
The offset must be a nonnegative integer; the limit is 1..64. `tabLayout` exposes
count, pending measurements, measurement count and the string font generation.
Agent view state additionally reports tree height, order relabels and the maximum
measurements per paint. Window idle requires the tab measurement queue to drain.

JS window exit requests pages of 32 documents and concurrently closes only clean
background documents, without activating them. Each `document.close` uses the
captured revision and `discardChanges:false`. Stale/dirty rejections keep the
document open, report changed state and return to the normal close decision;
other errors stop exit. Every accepted batch drains before the next decision or
durable checkpoint. Dirty documents and the final active document retain the
existing sequential save/discard/cancel workflow. Cancellation cannot undo an
already accepted clean close.

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

The window's default document keymap comes from the client JS command modules.
Retained service bindings override those defaults, and explicit client user
registrations override the service layer. Calling `astra.bind` with the same
value as the shipped default still expresses an explicit client preference.
Client commands may rebind after `await`; the next platform key observes the
new registration. An unknown/unbound document key has no hidden native editing
fallback. Native modal/form and view navigation controls retain their own
focused interaction behavior.

`Ctrl+Tab` / `Ctrl+Shift+Tab` and `Ctrl+PageDown` / `Ctrl+PageUp` select the next /
previous tab. `PageUp` / `PageDown` invoke JS scroll commands. Immediate UI reads
include `viewport: {firstLine, pageLines}` for the requested document; these are
document-line and visible-page metrics from native geometry. JS computes the
target and publishes the existing `scrollLine: {id, line}` update, which clamps
to the retained document extent. Page scrolling changes viewport position and
preserves the document text and selection. Its default command handlers are
user-overridable through the same asynchronous command registry.

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
accepted platform input handlers before returning retained metadata. This includes
JS `await` work before and after their native editing primitive. Switching tabs during
the wait does not change its document or newline policy. A read is a boundary
for prior accepted input, not a lock against future edits or another client.
Inside a platform input handler, reads of its captured document fence only
earlier handlers; they do not wait for themselves or later accepted input.
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
All default editing commands, including `editor.insert_text`, movement,
word selection and deletion, are registered in the client command host.
SDL text entry, editing shortcuts and double-click word selection invoke these
JS handlers, so client overrides apply to actual input. Parameters carry the
document `id` captured when input is accepted; movement also carries `extend`.
Keep that identity when calling native primitives after an `await`:

```js
if (astra.context === 'client') {
    astra.registerCommand('editor.insert_text', async (parameters) => {
        await astra.sleep(0);
        const {document} = await astra.ui.read({id: parameters.id});
        return await astra.ui.native('editor.insert_text', {
            ...parameters, id: document.id
        });
    });
}
```

Native editor primitives from other JS workflows also wait behind earlier
accepted input for their captured document. One platform handler runs per document;
unrelated documents remain eligible while it awaits. The platform queue accepts
at most 256 inputs and 8 MiB of aggregate serialized parameters/identities.
Overflow is rejected visibly before publication; no accepted entry is truncated.
Default vertical navigation resolves shaped visual targets when its native intent
is dispatched, then awaits the shared `document.setSelections` dispatcher.
Missing/stale shaping or a background document uses the service movement command.
Explicit revision guards still reject stale targets before mutation.
Input ordering is per document and continues until a deferred script operation
actually completes, fails or is cancelled. Later mutations for that document
wait; unrelated documents and viewport queries remain eligible. Saving does not
hold this input gate while asynchronous filesystem work owns its snapshot.
Remaining non-edit native workflow bridges still await their respective migration.
QuickJS-NG uses the fixed, hash-verified host-context extension described in
`xmake/packages/astra-quickjs/README.md`. Continuations retain the task in which
they were registered, even when another command resolves a shared Promise.
Retired/cancelled task continuations cannot issue new host RPC mutations.
`astra.ui.input({title, value, placeholder}, {signal, timeoutMs})`
returns entered text or `null` on dismissal. `astra.ui.confirm({title, detail,
accept, items}, options)` returns `true` or `null`. Optional `items` are a
scrollable review list, bounded to 4,096 single-line labels, 4 KiB per label and
64 KiB combined. `astra.ui.pick({title, items, placeholder}, options)` returns the
selected original item or `null`; items can be strings or objects with a `label`.
Filtering uses ASCII case folding and literal Unicode text matching; row identity
survives filtering and scrolling. Limits match the confirmation list. Native
filtering occurs when the query changes, not on each frame.

`items` may also be an asynchronous `(query, {signal}) => items` provider. It
receives the initial query and subsequent changed input, and can return an array
or `{items, message}`. Each result has the same 4,096-row / 64 KiB label limits;
`message` is at most 4 KiB of single-line UTF-8. Object items can supply `iconPath`
to use the JS file-icon theme: each path admits 32 KiB, 64 KiB combined per result.
The result's original item, including its application data, is returned on selection.

New input aborts the previous provider's signal and advances a native query
identity. Old rows remain visible while searching; Enter and pointer activation
are disabled until the current result arrives. Late publications cannot replace
the current query, even when a provider ignores cancellation. Dismissal aborts
the current provider. Provider failures stay inside the picker with an error
message; changing the query permits recovery. Pending providers participate in
window idle detection. Providers should check their signal and yield with
`astra.sleep(0, {signal})` during long CPU loops; the QuickJS execution budget
still applies. Native code handles input, measured result indexes and publication
validation; matching and ranking belong to the JS provider.

```javascript
const selected = await astra.ui.pick({
    title: 'Symbols',
    items: async (query, {signal}) => {
        const matches = await findSymbols(query, {signal});
        return matches.map(symbol => ({label: symbol.name, symbol}));
    }
}, {signal});
```

The shipped `Ctrl+P` command uses this provider API for fuzzy file lookup.
JS obtains `workspace.tree` through asynchronous pages once per open dialog,
retaining at most 100,000 file paths / 8 MiB of UTF-8 path bytes (not a JS heap
measurement). Ranking uses Unicode scalar subsequences, case folding, basename
and boundary bonuses; CPU passes yield every 32 entries or 4,096 string units.
Only the top 128 rows / 60,000 label bytes are published. Files beyond the static
picker's 4,096-item limit remain searchable within the bounded workspace index.
Reopening scans the current filesystem again. Cancellation drains owned service
operations without opening a file. Index limits and directory-read failures are
reported in the picker. The command palette also exposes **Open file by path...**
for an explicit workspace-relative path. This is a bounded index, not an
unlimited workspace search or a filesystem watcher cache.

`astra.ui.showText({title, text, monospace, actions, lineTones}, options)` awaits
an indexed text panel. It returns the selected action's string `id`, or `null`
on Enter/Escape/dismissal. Arrows and Shift-wheel support horizontal scrolling.
Optional `actions` contains at most eight `{id,label,key,destructive}` objects:
identities are unique nonempty UTF-8 strings up to 128 bytes; single-line labels
are at most 256 bytes; optional keys are unique ASCII letters/digits, case
insensitive. Buttons and shortcuts return identities to JS without invoking a
native service command. A shortcut's generated text/composition and auto-repeat
remain consumed through key release, preserving the editor's document and
selection state after the modal closes. `lineTones` contains ordered disjoint half-open logical
row ranges `{begin,end,tone}` bounded by the text's line count; tones are
`normal`, `added`, `removed`, `accent`, or `muted`. Native measured range indexes
resolve colors; JS supplies domain classification. `monospace` selects code fonts.
These are native retained modal
components with Promise completion; input/focus/drawing stays in C++ and the
following service workflow stays in JS. Abort dismisses the matching dialog and
drains its native future. Dialogs default to five minutes, with a separate bounded
client task deadline. UI automation distinguishes waiting for user input from
unfinished background operations. Input is bounded to 16 KiB; confirmation
details to 64 KiB. Cold editable inputs of at least 4096 UTF-8 bytes prepare
paragraph geometry on the shared libuv worker queue. Pointer-dependent input
waits for matching widget/focus/revision/style; following SDL text/IME packets
own their payloads in a bounded FIFO. Programmatic changes invalidate obsolete
input visibly, while Escape and native close controls remain responsive. The
limits, replay budget and soft deadlines are documented in [GRAPHICS.md](GRAPHICS.md).
Native text panels index logical CR/LF/CRLF rows and bound their
source to 8 MiB and 100,000 lines; diff shaping caches only a bounded visible-row
working set. Git comparison uses this same component: JS captures its path,
staged state and exact service preview identity; JS decides line tones, offers
stage/unstage actions and offers discard only for a working-tree preview.
Discard still awaits confirmation and the service validates the captured preview
before changing disk. Dismissal releases the preview session. Actions invoked
while a comparison is open abort and drain only that comparison's presentation
and metadata-read handles. A completed Git operation cannot dismiss a subsequently
opened unrelated modal. The internal
`_ui.read` result no longer carries Git-specific dialog state.
`astra.ui.update({gutter:{id,revision,marks}})` publishes generic document-line
decorations only while that document's revision still matches. Git's JS workflow
chooses clean matching documents before publishing working-tree gutter marks.
The clipboard adapter reads/writes at most 8 MiB and rejects
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
their total. The shipped JS Outline provider requests its viewport range through
the measured semantic index. Native highlight requests use `includeOutline:false`;
highlight preparation and Outline policy are independent clients of shared parsing.

`astra.ui.registerChrome(provider, handlers = {}, {timeoutMs = 30000} = {})`
installs local policy for the workbench and empty-editor body. Registration performs no I/O and
returns the previous frozen `{provider, handlers, timeoutMs}` registration.
The provider receives `(context, {signal, timeoutMs})` and may return a Promise.
`context` contains workspace/branch/message, bounded notice text, decimal-string
`noticeSequence` identifying the selected original notice, recovery availability,
active view and sidebar visibility,
the bounded registered view descriptors, active-document activity, and optional
document identity/revision/metadata sequence/path/encoding/indent/language data.
The primary caret has zero-based `line`, `utf16Column` and, when a matching
native shaped row is available, `visualColumn`. JS chooses how to label them.

The model requires `brand`, `hint`, and six arrays: `command`, `activity`, `manage`,
`breadcrumb`, `statusLeft`, `statusRight`. Total serialized size is at most
64 KiB. Command/manage accept at most one row, activity 16, and path/status banks
32 each. Rows are flat immutable measured descriptors: `id`, `label`, optional
`icon`, `tone`, `iconTone`, `selected` and `data`. `selected` marks icon buttons;
text items use their tone. The shared `settings` glyph is also available.
Four optional arrays extend that same model: `welcomeTitle`, `welcomeDetail`
(at most one row each), `welcomeActions`, and `notices` (at most eight each).
Omitted body banks normalize to empty arrays before the complete byte admission.
Action/notice rows additionally allow `badge` (64 bytes). Welcome captions use
native title/detail roles; actions place a separate shortcut badge. Welcome
targets are hidden as soon as an active document exists. Earlier notice rows
have higher fit priority within the reserved body area. In notices, `added`
uses the recovery surface, `changed` uses the warning surface, and `error` uses
the error surface; other tones use the panel. These roles do not imply commands.
`handlers.activate(data, row, context, bank)` receives a native-fenced original
descriptor. Caption, view-selection, path-navigation, welcome and notice actions have no
native product fallback. Earlier right status items have higher fit priority;
native layout exposes overflowing activity items through controls and wheel
navigation, and retains fitting final path components.

`await astra.ui.chrome.refresh()` invalidates the current generation and invokes
the registered provider; call it after live replacement or changing local policy.
Re-register the returned provider/handlers/deadline and refresh to restore them.
`await astra.ui.chrome.read()` returns `{version, modelVersion, context, pending,
model}`. Advanced `chrome.publish({version, model}, options)` returns false for
retired or already-published generations and validates a candidate completely
before mutation. Pending/failure retains prior pixels but rejects their actions.
Context changes cancel the superseded wait, including a provider ignoring its
signal; old replies/errors cannot publish or report current-window failures.
The deadline accepts integer milliseconds 1..30,000. Current failures reject the
pending publication and surface their error. `await chrome.suspend()` returns
the prior suspension state, cancels the wait and invalidates actions;
`await chrome.resume()` refreshes current policy. Exit policy suspends before
its first idle fence and resumes afterwards. Agent presentation state adds
the original chrome snapshot, activity page and bounded visible target boxes,
including the host diagnostic box. Host failures use a separate direct native
diagnostic caption so a failed provider cannot hide its own error. Display is
bounded and single-line; the original error remains in host state. That region
blocks editor pointer fallthrough and has no product action.

The shipped workspace-notice action forwards its captured `noticeSequence` through
the registered `ui.dismissWorkspaceNotice` command. At the native mutation boundary,
`await astra.ui.native('ui.dismissWorkspaceNotice', {noticeSequence})` returns false
without clearing anything if that original notice has changed, or true after clearing
the matching notice. This also fences changes arriving while a custom command handler
awaits other work. Display truncation does not define notice identity. Explicit command
execution without a sequence dismisses the currently selected notice; replacement
handlers should forward the supplied parameters to preserve captured-action semantics.

`astra.ui.registerOutline(spec, provider, handlers = {})` installs local policy for
the active document's Outline pane. `spec.loading` is its pending message.
The provider receives `(viewport, {signal, timeoutMs})` and returns a value or
Promise for `{offset, total, rows, empty?}`. `viewport` contains `id`, `revision`,
`activity`, string `version`, `offset`, `limit` and `visible`. Rows cover exactly
`min(limit, total - offset)` items; a request past a shrinking source may clamp
back into that source. Pages contain at most 256 leaf rows / 64 KiB. Unlike whole
hierarchical sidebar sources, their indentation can start inside an off-page
parent. Group rows are rejected; depth remains 0..64. The native page retains
immutable measured rows and does not materialize the provider's entire source.

`handlers.activate(data, row, identity)` owns each click. A native read checks the
published row and current document/revision/activity/viewport generation before
dispatch. The default JS handler performs a fenced document read and a
revision-guarded `document.setSelections`, then reveals only that document.
Providers choose labels, icons, empty/unsupported/error messages and actions.
Changing document, revision, visibility or viewport cancels the superseded JS
wait and rejects its late publication. Shared parse operations keep running for
other clients. A current provider failure publishes an empty terminal page with
a bounded, scalar-safe message and reports the error; a current AbortError
settles the page without reporting an unrelated failure.
If native publication rejects a retired error page before JS receives its next
viewport event, that provider error is also discarded.

Registration performs no I/O and returns the previous frozen registration
`{spec, provider, handlers}`, if present. To replace a provider at runtime, call
`await astra.ui.outline.refresh()` after registration; the old publication keeps
its original handler until invalidation. Re-register the returned triple and
refresh to restore it. `await astra.ui.outline.read()` returns the guarded
viewport plus `total`, `pending`, `empty` and the bounded published `rows`.
Advanced `outline.publish(model, options)` accepts the four identity fields
`version`, `id`, `revision`, `activity` plus the page fields above. Invalid pages
reject atomically; obsolete or already-published generations return false.
The registration wrapper supplies these identity fields automatically. All three
Outline request methods return Promises; registration is synchronous local setup.

`astra.ui.registerView(spec, provider, handlers = {})` registers a sidebar view.
The activity bar uses a snapshot of the registered `id`, `title`, `icon`, `order` (0..31) and
optional `outline` flag. `outlineTitle` supplies the associated Outline header;
the JS API defaults it to `Outline`. Native registration retains the same fallback
for older direct specifications. Captions are single-line UTF-8, at most 128 bytes;
invalid captions reject before changing the registered view. Registration installs
local policy; it performs no I/O.
At most 16 views may be presented. Activity icons are `files`, `search`, `branch`,
`braces` or `symbol`. The default Explorer, Search and Source Control use the same
native row renderer and publication path as custom views.

The provider receives `(parameters, {signal})` and may return a value or Promise
for `{rows, subtitle?, empty?, update?, complete?}`, or a row iterable directly.
`rows` accepts arrays, synchronous iterables and asynchronous iterables. Each row
has stable `id`, single-line `label`, optional
`icon`, `iconTone`, `badge`, `tone`, `depth`, `group`, `selected` and JSON `data`.
`iconTone` chooses `normal`, `muted`, `accent`, `added`, `changed` or `error`
from the effective theme; an explicit native file-icon resolver retains precedence.
Group descendants follow their parent in preorder; depth may increase only one
level after a group. Depth is 0..64. Native group folding uses the measured index.
`await astra.ui.views.read(id?)` returns the current `{id, title, outlineTitle,
expanded, version, row, index, total, scroll, focused, focus}` state. Captions retain
their original text even when native presentation elides them. `expanded` reports
the view's root section fold. `row` and `index` are null for an empty or
unselected view. `await astra.ui.views.focus(id?)` focuses the visible view's rows,
retaining its cursor or selecting the first row. Focusing a hidden view or focusing
while modal input is active is rejected. `await astra.ui.views.blur()` returns
focus to the editor. `blur({id, focus})` accepts an optional captured focus guard;
a stale guard returns false. Built-in file/search activation returns editor focus
after opening/selecting the result, without stealing focus from a subsequently
selected control. These controls, like the other public UI APIs, return Promises.

`await astra.ui.views.select(id, rowId, guards?, options?)` selects a visible row
by stable identity. `await astra.ui.views.navigate(id, action, guards?, options?)`
atomically computes navigation from the current retained cursor; it does not
require a read/select round trip. Actions are `next`, `previous`, `first`, `last`,
`pageUp`, `pageDown`, `left`, `right`, `toggle`, `activate` and `retain`.
Optional `{version, focus}` guards come from a read/focus result. A stale guard
returns null without changing the view. Selecting a missing or hidden row also
returns null. Invalid actions reject.
`await astra.ui.views.reveal(id, rowId, guards?, options?)` also accepts hidden
rows: it expands only their ancestors, selects the target and scrolls it into
the viewport. Unrelated folders and the target's own fold remain unchanged.
It does not show the sidebar, switch views or change keyboard focus. Missing
rows and stale guards return null; retirement backpressure rejects before any
fold, cursor or scroll changes. Old projections are reclaimed off the input loop.
Reveal accepts `{version, focus, activity}` guards. `activity` comes from
`await astra.ui.read({immediate:true})`; it changes on active-document switches,
active path changes and closing the active document, including A-to-B-to-A.
The coalesced `ui.document.activated` event carries the latest `{activity}`.
`ui.read({immediate:true, preferences:['explorer']})` additionally reads merged
preference sections (at most 16 names, each at most 128 bytes); absent sections
are null. Explicit client JS settings override retained service preferences.
The bundled Explorer uses `explorer.autoReveal` (default true) after activation
and successful refresh. `ui.revealActiveFile` explicitly shows/focuses Explorer
and reveals the current path even when automatic reveal is disabled.
Retained selections remain visible after focus leaves the view: JS theme role
`inactiveSelection` supplies their background, while `selection` supplies the
focused background. The focus accent remains exclusive to the focused cursor.
Left collapses an expanded group or selects its parent; Right expands a group or
enters its first child. Activate toggles groups and awaits the JS leaf handler.
Toggle changes groups only. Page navigation uses the visible viewport, and cursor
changes reveal the row. Refreshes retain cursor identity across reordering; hidden
cursors move to a visible ancestor and removed cursors to the nearest old index.
An optional `handlers.published({id, version}, options)` runs after successful
native publication and guarded completion metadata. It may return a Promise;
obsolete/cancelled publications do not invoke it. Any further native action
should retain the supplied version guard because another refresh can run while
the handler awaits. Handler failure rejects the refresh while preserving its
already-published source.

SDL row navigation dispatches overridable `ui.view.<action>` JS commands, with
captured view/version/focus guards, through a bounded FIFO per view. The limits
are 256 accepted inputs and 8 MiB of serialized parameters/identities. Independent
views are separate scopes; stale queued inputs are retired. A handler can await
`waitForIdle()` without waiting on itself or its queued successors. Input/rendering
continue while it awaits. Changing focus invalidates delayed row navigation.
Up/Down, Home/End, PageUp/PageDown, Left/Right, Enter and Space operate the focused
row widget. F6 switches between editor/forms and sidebar rows; Tab or Escape from
rows returns editor focus. Ctrl+Shift+E shows and focuses Explorer. Text/IME and
editing shortcuts do not mutate the document while row focus is active. Clicking
editor content, tabs, or a retained form transfers focus away from the row widget.

`handlers.activate(data, row)` is awaited for leaf activation; late events from
replaced view generations are discarded. `handlers.show()` can customize showing
a view. `handlers.refresh()` can read current retained controls when the view's
refresh command is invoked. Built-in providers use `handlers.project(model, options)`
to translate service data into these rows; this callback may also return a Promise.
Extensions normally return rows directly.
Projected presentations may include `update` with generic UI fields such as
`status`, `error` or `branch`. These updates are guarded by the published view
version; native rendering does not interpret provider-specific service models.
Optional `complete(options)` runs after the row iterator finishes and may return
a Promise for `{subtitle?, empty?, update?}`. Only these metadata fields are
accepted. This lets generators calculate final counts without retaining all rows.
Completion failure rejects publication and retains the old view. Providers,
iterators and completion callbacks must honor `options.signal` during external
awaits; cancellation does not forcibly interrupt an arbitrary unrelated Promise.

`await astra.ui.showView(id)` selects a registered view, refreshing its first
presentation. `await astra.ui.refresh(id, parameters)` cancels older refreshes;
`await astra.ui.renderView(id, model, {signal?})` directly publishes a model.
Both return true for an accepted publication, false for a superseded publication;
refresh cancellation may return undefined. `astra.ui.cancel(id)` signals the
current provider locally. Providers must honor their signal for external work.

Publication uses owner-local begin/append/commit/abort identities. The JS wrapper
sends at most 128 rows and 60,000 UTF-8 bytes per page, yielding between pages.
It awaits each page acknowledgement before requesting further rows, retaining
one bounded page plus one candidate row. Rows are snapshotted as JSON when
yielded, so a generator may reuse an object without changing earlier rows.
Failure, cancellation or supersession closes an active iterator and aborts its
unpublished builder. JS also enforces the total row and byte limits before
publication;
the native boundary rejects more than 256 rows or 64 KiB per page, 100,000 total
rows or 32 MiB of serialized pages. Row ids/labels are at most 4 KiB and row data
at most 8 KiB. Malformed rows, duplicate ids, invalid hierarchy and invalid UTF-8
are rejected before altering the builder. A failed or cancelled replacement keeps
the last published snapshot interactive when rejected before publication. Native fold filtering advances at most
128 rows of work per pump, restarting if the old view's fold state changes. The
commit Promise settles only after atomic publication; shutdown cancels pending
publication fences. No production drawing backend is replaced.

Native view sources, abandoned builders and superseded fold projections retire
on a dedicated memory-only worker. No SDL/JS/service/I/O runs there; libuv retains
the external I/O responsibilities. Admission reserves one of 48 lifetime slots
before beginning a build, so publication/cancellation can transfer ownership
without allocating queue entries. Sources and shared retired projections are
conservatively charged against a 256 MiB serialized-page budget. This is not a
measurement of total heap usage. Exhausted slot/byte admission rejects the new
operation before changing existing data; publication and row navigation retain
their old guards. The worker releases ownership outside its queue mutex and
drains queued jobs fairly. `waitForIdle()` and window close also await retirement;
the final native shutdown drains published views and rejects new builders.
External C++ snapshot holders still determine the lifetime of their own retained
edges. Their later releases are outside this GUI worker's ownership.

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

The default Explorer, Search and Source Control consume service pages through
async row generators instead of concatenating complete service/projection arrays
in JS. Service operations still retain completed immutable results and replacement
plans in C++; these are paged after completion, not progressive worker results.
Native source/index reclamation remains a separate latency concern.

```javascript
astra.ui.registerView({ id: 'streamed-files', title: 'Files' }, (_, options) => {
    let count = 0;
    return {
        rows: (async function* () {
            for await (const page of astra.operationPages('workspace.tree', {}, options)) {
                for (const entry of page.entries) {
                    ++count;
                    yield { id: entry.path, label: entry.path, data: { path: entry.path } };
                }
            }
        })(),
        complete: () => ({ subtitle: `${count} entries` })
    };
});
```

`astra.ui.forms.open(spec)` returns a Promise for a retained nonmodal input form.
The acknowledgement is `{id, version, values}`; it does not await dismissal or
hold a modal input task. `read(id)` returns the current state or null,
`close(id)` closes only that identity, and `update({id,version,message,busy,error})`
applies status only to that input generation, returning false for an obsolete
generation or component. Native openings receive unique epochs, including when
an extension reuses a component id. One form may be mounted in the editor and
one in each registered sidebar view; replacing a form on the same mount
emits a closed event for the old identity. Only one form owns keyboard/IME focus.
Inactive sidebar forms retain their input and history. Existing modal inputs must be dismissed
before opening a form.

Specifications contain `id`, `title`, `fields`, `toggles`, `actions` and optional
`focus` (a field id) and `view` (a registered sidebar identity). Omit `view` to
mount an editor overlay. Show the requested sidebar before mounting or focusing
its form. Sidebar controls reserve space above the view's rows. Acknowledgements
include `view` for sidebar mounts. There are 1..4 single-line fields and actions, and 0..4
toggles. All control ids are unique across the form. Fields have `id`, `value`,
`placeholder`, `submit` and optional `shiftSubmit`; submit identities refer to
actions. Toggles have `id`, `label`, boolean `value`, and optional `shortcut`
(`Alt+` plus one uppercase ASCII letter). Actions have `id` and `label`.
Input text is bounded to 16 KiB per field; CR/LF, NUL and invalid UTF-8 are
rejected before mutation. Inputs have independent bounded Unicode editing and
undo history, shaped mouse selection, clipboard and IME candidate positioning.
`await astra.ui.forms.focus(id, field, selectAll = true)` focuses a visible field,
returning its state or null for a closed identity. `blur(id)` releases focus
without closing or discarding input. `setValues({id,version,values})` validates
all supplied fields/toggles before publishing one new generation, returns the
new state (unchanged for identical values), or null for a stale/closed identity.
Programmatic text changes preserve independent input undo history. Failed
validation preserves content, selection, history and generation for every field.
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

Default `ui.search` (Ctrl+Shift+F) and `ui.workspaceReplace` (Ctrl+Shift+H)
focus retained query/replacement fields in the Search sidebar. Their JS module
owns match-case/regex/whole-word options, a cancellable 200 ms preview debounce,
service pagination, row projection/navigation and replacement confirmation.
`await astra.ui.execute('workbench.search', parameters)` updates these controls
and searches; it accepts query/options plus the service's `scope`, `maxMatches`
and `includeIgnored` bounds. Successful searches return
`{operationId, matchCount, fileCount, paths, truncated, issues}`. `paths` is a
sample of at most 200 distinct labels, not an exhaustive affected-file list;
the command no longer returns a complete JS `matches` array. Use the retained
operation identity with paged `operation.get` to inspect full results before
that operation expires. Cancelled, superseded or failed commands return false.
Search validates stable file counts and pagination before publishing rows or
offering replacement confirmation. Input changes never initiate writes. Replace all
searches afresh, refuses truncated/unreadable previews, confirms affected files,
then rechecks the native input version before submitting `workspace.replace`.
Completed reports publish both disk counts and modified unsaved-buffer metadata.
Explicit cancellation of an accepted write requests service cancellation and
continues polling its final report; pending/conflicted work and partial success
remain visible. Window shutdown cancels readonly previews and confirmations,
but waits for accepted writes before checking recovery and teardown.

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
