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
`workspace.replace` return `{operationId}` immediately. Query
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
- `workspace.search {query,wholeWord?,includeIgnored?,maxMatches?}` currently
  searches literal, case-sensitive **on-disk** content. Results explicitly say
  `scope:"disk"`; unsaved-buffer, regex, and case-insensitive search remain work.
- `workspace.replace {searchId,replacement}` preflights exact file versions,
  rejects dirty matching buffers and incomplete searches, and reports applied
  and pending paths. Each file is atomic; multiple files are not one filesystem
  transaction. Clean open buffers reconcile after successful writes.
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
