# Language service protocol

These methods run through the same dispatcher for in-process GUI, TCP, scripts
and Agent clients. IDs and revisions are decimal strings. Byte offsets refer to
UTF-8 scalar boundaries in the given document revision. Server ranges are LSP
UTF-16 positions; clients should let the service convert and apply them.

## Queries

`language.request` accepts `{id, baseRevision, method, byteOffset?}`. `method` is
`completion`, `hover`, `definition`, `references`, `rename`, or `formatting`.
All except formatting require `byteOffset`. Rename also requires `newName`.
References optionally accepts `includeDeclaration`; formatting accepts `tabSize`
and `insertSpaces`. The document must have a workspace path and a configured
language server. The first request starts the server automatically.

The immediate result is `{operationId}`. Poll `operation.get {id: operationId}`
or consume `operation.completed`. Successful language operations return
`{id, revision, method, obsolete, result}`, where `result` preserves the server's
LSP result. `obsolete:true` means a relevant buffer changed or closed while the
request ran; discard the result and request again with a fresh revision.
External process I/O and waiting occur on workers, not the document owner loop.

Definition and references also return `resolvedLocations` with
`{uri, path, byteOffset, endByteOffset, line, character}` for workspace targets.
Open targets include `targetId` and `targetRevision`, and positions are resolved
against their dirty buffer snapshots. If that revision has changed by the time
the client navigates, request fresh locations. Targets outside the workspace are
marked `outsideWorkspace:true` and are not read or opened by this operation.

`language.diagnostics {id}` lazily starts the configured session and returns
`{id, revision, available, pending, diagnostics}`. Unsupported extensions and
unbound buffers return `available:false, pending:false` with a `reason`, without
an RPC error. A startup failure adds `error`. Cached diagnostics are returned only
for the current buffer revision. `versioned` states whether the server supplied
an LSP document version.

An edit followed by undo can leave a newer document revision with exactly the
text already synchronized to the server. Such a synchronization does not send
an empty `didChange` or advance the independent LSP version. Diagnostics remain
bound to the server instance, URI and LSP version; only this proven no-change
case may rebind them to the newer document revision. Results arriving before
synchronization finishes are retained internally and never exposed for a
different current revision.

When synchronization does change the server text back to an earlier content
identity (for example, undo after an edit was sent), Astra uses a narrowly scoped
clangd compatibility option. For a known `clangd` process, that `didChange` carries
`forceRebuild:true` and `wantDiagnostics:true`. The latter requests diagnostics for
the new LSP version using clangd's
[force diagnostics generation extension](https://clangd.llvm.org/extensions#force-diagnostics-generation).
[Upstream issue 2484](https://github.com/clangd/clangd/issues/2484) describes the
cached-content shortcut that can suppress a new publication despite
`wantDiagnostics`, and the cost of `forceRebuild` discarding cached state.
Therefore ordinary edits and other servers receive neither field. The low-level
`LspChangeOptions::clangd_force_rebuild` defaults to false and affects only the
individual notification where it is explicitly enabled. The already-synchronized,
identical-text case above still sends no change; stale diagnostic versions are
never accepted merely because content was restored.

`language.diagnosticsChanged {id, revision, count}` is a lightweight notification
that tells local and TCP clients to fetch the current diagnostics. Repeated
identical results do not emit another event, and queued events for one document
coalesce to the newest revision. An owner-loop timer drains asynchronous server
events even when no client is sending requests.

## Applying edits

`language.applyTextEdits {id, baseRevision, edits, additionalTextEdits?, editMode?}`
accepts LSP TextEdit/InsertReplaceEdit values. `editMode` is `insert` or `replace`
(default). All edits share the same base snapshot and become one atomic document
transaction and undo step; overlaps, stale revisions and invalid ranges reject
the entire transaction. The result is updated document metadata.

`language.applyCompletion {id, baseRevision, completionItem, byteOffset, editMode?}`
applies the selected server item and its additionalTextEdits together. When no
explicit textEdit exists, the service replaces the Unicode identifier around the
cursor using insertText or label. This session advertises snippetSupport:false;
a snippet-only item fails explicitly with -32031.

## Cross-file rename

A successful rename query also returns `planId` and `documents` previews with
`{path, editCount, id?, baseRevision?}`. `language.prepareWorkspaceEdit
{operationId}` retrieves the same preview from a completed rename operation.
Preview tokens are service-owned and bounded (16 plans, 128 MiB retained closed
file content/replacement text); an expired token requires a new rename request.

`language.applyWorkspaceEdit {planId}` consumes the preview token and returns a
new `operationId`. The worker verifies closed files against their pre-request
disk snapshots. The owner then clones all affected documents, validates all
edits/revisions and prepares notifications before publishing any mutation. Any
failure leaves every buffer untouched. Success returns
`{applied:true, documents:[metadata...]}`. Closed targets become new dirty buffers;
no target is written to disk. Clients should expose these buffers for review and
normal conflict-checked saving. Each target has one undo step.

Rename scans are bounded to 10,000 workspace entries, 16 MiB per closed file and
64 MiB total source text. Plans may edit at most 256 files. Resource create/delete/
rename operations are not advertised and are rejected. A workspace too large for
these bounds receives an explicit error, rather than a partial rename. clangd
needs the project's compilation database/background index for complete cross-file
references; the integration tests wait for observable header references before
requesting the rename.

Mutations are revision-checked; a JSON-RPC response size error may occur after a
handler completed and carries `handlerCompleted/resyncRequired`. Do not retry a
mutation blindly: fetch current metadata/revisions first. A JSON-RPC batch remains
a sequence of independent operations, not an atomic workspace transaction.
