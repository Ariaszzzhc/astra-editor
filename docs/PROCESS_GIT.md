# Process and Git services

`astra-process` owns a private libuv loop and thread for each child process.
Arguments are passed as an argv array without a shell. Standard streams, queued
writes, callback errors, timeouts, cancellation and shutdown have explicit bounds.
Completion follows both the final output and handle cleanup. On Windows,
`astra-process-host.exe` must reside beside the application: it assigns the
suspended target to a private Job Object before the target can create descendants.
The parent waits for the tracked processes before reporting completion.

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
