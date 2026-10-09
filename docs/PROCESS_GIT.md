# Process and Git services

`astra-process` owns a private libuv loop and thread for each child process.
Arguments are passed as an argv array without a shell. Standard streams, queued
writes, callback errors, timeouts, cancellation and shutdown have explicit bounds.
Completion follows both the final output and handle cleanup. On Windows,
`astra-process-host.exe` must reside beside the application: it assigns the
suspended target to a private Job Object before the target can create descendants.
The parent waits for the tracked processes before reporting completion.

The Unix source starts each target in a private session/process group using
`UV_PROCESS_DETACHED`. Immediately closing its libuv process handle transfers
reaping responsibility to the adapter, as described in the
[libuv process API](https://docs.libuv.org/en/v1.x/process.html#c.uv_spawn).
The owner timer observes the direct child with `waitid(WNOWAIT | WNOHANG)`,
sends `SIGKILL` to its group on normal leader exit or forced shutdown, and only
then calls `waitpid`. Keeping the direct child unreaped pins its PID/PGID until
the final group signal; the adapter never signals or probes that number after
reaping. This ordering follows the
[pinned libuv 1.52.1 implementation](https://github.com/libuv/libuv/blob/v1.52.1/src/unix/process.c),
whose ordinary exit callback runs after `waitpid` has already released the PID.

Successful Unix completion means that owned handles and writes have finished,
the direct child has been reaped, same-group descendants have been sent
`SIGKILL` (or the group no longer exists), and output has drained to EOF. It does
not claim that non-child descendants were reaped by Astra. Descendants that
deliberately create a different session/group escape this ownership boundary;
this is process lifecycle management, not a sandbox. Inherited output pipes
that remain open past the drain deadline are closed with an explicit error.

Unix callers must not install automatic child reaping (`SIGCHLD = SIG_IGN` or
`SA_NOCLDWAIT`), alter that disposition while processes run, or independently
wait for these children. Startup rejects an automatic-reaping disposition.
An observed `ECHILD` revokes the stored PID immediately and reports lost
ownership instead of signalling a possibly recycled number. Interrupted system
calls retry; other wait/signal errors are reported and retried by the timer.
Once a leader has exited, group signalling retries only until the shutdown
deadline, after which the leader is reaped with an error rather than retained
as a permanent zombie. A live direct child is never reported as reaped merely
because a timeout passed: an OS refusal to kill/reap it can still delay shutdown.
The timer starts before spawn so failures after a successful exec, including
stdio setup failure, retain a cleanup owner. Process fixture discovery uses
`uv_exepath` outside Windows; this source has not been run on Linux or macOS here.

`astra-git` executes Git on a worker using this adapter. It requires an explicit
working-tree root, uses literal pathspecs, and parses NUL-delimited porcelain
records. Status, staged/unstaged diffs, gutter ranges, history, branches, staging,
unstaging and commits use real repository state. Repository attributes and Git's
working-tree filters remain effective. Signing configuration remains effective;
hooks and fsmonitor are disabled for the editor's noninteractive commands.

Discard requires a previously observed exact disk version (or an explicitly
missing file), obtains the filtered index content, strictly decodes it, and saves
atomically through the normal file store. Changed versions, binary conversion,
filter failures and merge conflicts leave the existing file intact. Git operations
do not silently stage unrelated edits or invoke network commands.

Tests explicitly isolate system/global Git configuration. They use real temporary
repositories, commits, conflicts, CRLF attributes, Unicode names, metacharacters,
filters, cancellation and bounded output. Test directory cleanup allows at most
one second for transient Windows sharing/lock violations; all other failures are
reported immediately. Process lifecycle tests separately require immediate removal
of the child's working directory, with no retry allowance.

Windows Debug and Release acceptance includes 17 process cases and 19 Git cases.
Run the standalone target with the pinned wrapper:

```powershell
./scripts/xmake.ps1 f -m debug -y
./scripts/xmake.ps1 build -y astra-platform-tests
./scripts/xmake.ps1 run astra-platform-tests --no-version
```

Repeat with `-m release`. This environment does not validate Linux/macOS behavior.
