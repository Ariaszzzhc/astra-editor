param(
    [ValidateSet('debug', 'release')][string]$Mode = 'release',
    [string]$ServerExecutable = ''
)
$ErrorActionPreference = 'Stop'
$testRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$testBuild = [IO.Path]::GetFullPath((Join-Path $testRepo 'build'))
$testRoot = Join-Path $testBuild ('cli smoke 中文 ' + [Guid]::NewGuid().ToString('N'))
$testExe = if ($ServerExecutable) { $ServerExecutable } else {
    Join-Path $testBuild "cmake/windows-$Mode/bin/astra-server.exe"
}
if (-not (Test-Path -LiteralPath $testExe -PathType Leaf)) { throw "Build astra-server ($Mode) first" }
[void][IO.Directory]::CreateDirectory($testRoot)

function New-TestProcess([string[]]$ProcessArguments) {
    $processInfo = [Diagnostics.ProcessStartInfo]::new()
    $processInfo.FileName = $testExe
    $processInfo.WorkingDirectory = $testRepo
    $processInfo.UseShellExecute = $false
    $processInfo.CreateNoWindow = $true
    $processInfo.RedirectStandardOutput = $true
    $processInfo.RedirectStandardError = $true
    $processInfo.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    $processInfo.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
    foreach ($argument in $ProcessArguments) { $processInfo.ArgumentList.Add($argument) }
    return [Diagnostics.Process]::Start($processInfo)
}

$testServer = $null
try {
    $testServer = New-TestProcess @('--workspace', $testRoot, '--port', '0', '--state-directory', (Join-Path $testRoot 'state'))
    $readyLine = $testServer.StandardOutput.ReadLineAsync()
    if (-not $readyLine.Wait(10000)) { throw 'Server startup timed out' }
    $ready = $readyLine.Result | ConvertFrom-Json
    if ($ready.application -ne 'Astra' -or $ready.port -le 0) { throw "Unexpected readiness output: $($readyLine.Result)" }
    function Invoke-TestRpc([string]$Method, [hashtable]$Params) {
        $body = @{jsonrpc='2.0'; id='cli'; method=$Method; params=$Params} | ConvertTo-Json -Depth 20 -Compress
        $client = New-TestProcess @('--connect', "127.0.0.1:$($ready.port)", '--request', $body)
        try {
            $stdout = $client.StandardOutput.ReadToEndAsync()
            $stderr = $client.StandardError.ReadToEndAsync()
            if (-not $client.WaitForExit(15000)) { throw 'CLI request timed out' }
            if ($client.ExitCode -ne 0) { throw "CLI failed: $($stderr.GetAwaiter().GetResult())" }
            $reply = $stdout.GetAwaiter().GetResult() | ConvertFrom-Json
            if ($reply.error) { throw "RPC error: $($reply.error | ConvertTo-Json -Compress)" }
            return $reply.result
        } finally {
            if (-not $client.HasExited) { $client.Kill($true); $client.WaitForExit() }
            $client.Dispose()
        }
    }
    function Wait-TestOperation([object]$Operation) {
        $operationDeadline = [DateTime]::UtcNow.AddSeconds(10)
        do {
            $completed = Invoke-TestRpc 'operation.get' @{id=$Operation.operationId}
            if ($completed.state -ne 'pending') { break }
            Start-Sleep -Milliseconds 10
        } while ([DateTime]::UtcNow -lt $operationDeadline)
        if ($completed.state -ne 'completed') { throw "Operation did not complete: $($completed | ConvertTo-Json -Depth 10)" }
        return $completed.result
    }
    $expectedText = "C:\code\中文`nemoji 😀`n"
    $created = Invoke-TestRpc 'document.create' @{text=$expectedText}
    $read = Invoke-TestRpc 'document.get' @{id=$created.id}
    if ($read.text -cne $expectedText) { throw 'CLI changed Unicode, newline, or literal backslash content' }
    $finding = Invoke-TestRpc 'document.find' @{id=$created.id; baseRevision=$created.revision; query='中文'; begin=25; select=$true}
    $found = Wait-TestOperation $finding
    if ($found.match.begin -ne 8 -or $found.match.end -ne 14 -or -not $found.wrapped -or
        $found.match.line -ne 0 -or $found.match.utf16Column -ne 8) { throw 'CLI find lost UTF-8 positions or wrap semantics' }
    $previous = Wait-TestOperation (Invoke-TestRpc 'document.find' @{id=$created.id; baseRevision=$created.revision; query='中文'; direction='backward'; select=$true})
    if ($previous.match.begin -ne 8 -or $previous.match.end -ne 14 -or -not $previous.wrapped -or
        $previous.document.revision -ne $created.revision -or $previous.document.canUndo) { throw 'CLI previous search lost selection-start wrap or history semantics' }
    $noPrevious = Wait-TestOperation (Invoke-TestRpc 'document.find' @{id=$created.id; baseRevision=$created.revision; query='中文'; direction='backward'; wrap=$false})
    if ($null -ne $noPrevious.match -or $noPrevious.wrapped) { throw 'CLI previous search ignored no-wrap at the first match' }
    $replacing = Invoke-TestRpc 'document.replaceAll' @{id=$created.id; baseRevision=$created.revision; query='(emoji)'; regex=$true; replacement='[$1]'}
    $replaced = Wait-TestOperation $replacing
    $afterReplace = Invoke-TestRpc 'document.get' @{id=$created.id}
    if ($replaced.replacements -ne 1 -or $afterReplace.text -cne $expectedText.Replace('emoji','[emoji]')) { throw 'CLI replacement lost regex capture semantics' }
    $created = Invoke-TestRpc 'document.undo' @{id=$created.id; baseRevision=$afterReplace.revision}
    $afterUndo = Invoke-TestRpc 'document.get' @{id=$created.id}
    if ($afterUndo.text -cne $expectedText -or $created.dirty -or $created.canUndo -or
        $created.selections[0].anchor -ne $found.document.selections[0].anchor -or
        $created.selections[0].head -ne $found.document.selections[0].head) { throw 'CLI replacement was not one atomic undo transaction' }
    $saving = Invoke-TestRpc 'workspace.save' @{id=$created.id; baseRevision=$created.revision; path='保存.txt'}
    $saved = Wait-TestOperation $saving
    if ([IO.File]::ReadAllText((Join-Path $testRoot '保存.txt')) -cne $expectedText) { throw 'Saved bytes differ from CLI content' }
    Write-Output 'Astra server process + TCP CLI + Unicode argv + async find/regex replacement/undo/save: PASS'
} finally {
    # This test owns the process object and only terminates its own temporary server.
    if ($null -ne $testServer) {
        if (-not $testServer.HasExited) { $testServer.Kill($true); $testServer.WaitForExit() }
        $testServer.Dispose()
    }
    $resolvedRoot = [IO.Path]::GetFullPath($testRoot)
    if ([IO.Path]::GetDirectoryName($resolvedRoot) -ne $testBuild) { throw 'Unsafe CLI fixture cleanup refused' }
    if (Test-Path -LiteralPath $resolvedRoot) { Remove-Item -LiteralPath $resolvedRoot -Recurse -Force }
}
