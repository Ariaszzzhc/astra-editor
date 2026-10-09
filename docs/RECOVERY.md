# Durable recovery

Astra checkpoints immutable dirty-buffer snapshots on workers after 500 ms of
quiet, or at most two seconds of continuous editing. Text, selections, encoding,
workspace path and exact original disk bytes are persisted together. Recovery
does not grant permission to overwrite a file changed outside the editor.

Each process owns an exclusively locked session. Checkpoints use durable atomic
replacement, a schema and checksum, bounded decoding, and monotonically increasing
frame epochs. Epochs also distinguish a newly dirty saved baseline at the same
text revision. Saved or discarded buffers receive durable tombstones; late
workers cannot resurrect them. Corrupt records remain available for inspection.
Maintenance removes only abandoned, empty, valid sessions.

## Service API

All methods use the same local, TCP and JavaScript dispatcher. `recovery.status {}`
returns `enabled`, `ready`, `pending`, and optional `sessionId`/`error`.
The remaining methods return `{operationId}`; inspect `operation.get` or
`operation.completed` for the result:

- `recovery.list {}` lists sessions, active ownership, recoverable buffers and
  explicit issues. Active sessions cannot be claimed by another process.
- `recovery.flush {}` captures the current frame and waits for durable completion
  on a worker. If recovery is disabled it reports `enabled:false`.
- `recovery.restore {sessionId,ids?}` restores all selected records atomically
  into new unsaved buffers. It refuses duplicate/open workspace paths, corrupt
  selected records and capacity overflow. A batch is limited to 128 buffers and
  128 MiB of combined restored text and disk baseline bytes.

The old session remains claimed until the restored content has a durable new
checkpoint or has been saved/discarded. Restore never rewrites source files.
`recovery.available`, `recovery.warning` and `recovery.error` report availability
or storage problems. Failures retain dirty content and retry after a bounded
delay. The UI keeps an exit window open when its final checkpoint fails, offering
retry, keep open, or an explicit exit without a successful checkpoint.

The default state directory is scoped by canonical workspace identity under the
platform's application-data location. `--state-directory` overrides it; automated
tests use repository-local state. Shutdown drains accepted work before its final
checkpoint and reports storage errors.

The client `astra/recovery` JavaScript module owns discovery, inactive-session
selection, recovery confirmation, storage notifications and manual checkpoints.
The `ui.recovery` command composes indexed native picker/confirmation components and awaits
`recovery.restore`; dismissing either component leaves documents untouched.
Issue-only sessions open a read-only report. Selected session identity is captured
before confirmation, so a newer discovery cannot change the restore target.
Superseded scans are cancelled and older list replies cannot replace a newer
availability notification. The `ui.checkpoint` command resolves only after the service's
durable result and propagates storage failures. Native final-window checkpoint
and exit decisions remain pending migration to JS in the active revision.

## Windows acceptance

The standalone `astra-recovery-tests` target covers 15 cases and 105 assertions
in Debug and Release: real disk conflicts, exclusive session locks, malformed
and truncated data, stale epochs, atomic-write failure, selective maintenance,
and actual abrupt child-process termination followed by recovery. These results
do not establish Linux/macOS correctness; those systems are not tested here.
