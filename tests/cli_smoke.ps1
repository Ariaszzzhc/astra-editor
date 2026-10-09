param([ValidateSet('debug', 'release')][string]$Mode = 'release')
$ErrorActionPreference = 'Stop'
$testRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$testBuild = [IO.Path]::GetFullPath((Join-Path $testRepo 'build'))
$testRoot = Join-Path $testBuild ('cli smoke 中文 ' + [Guid]::NewGuid().ToString('N'))
$testExe = Join-Path $testBuild "windows/x64/$Mode/astra-server.exe"
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
    $testServer = New-TestProcess @('--workspace', $testRoot, '--port', '0')
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
    $expectedText = "C:\code\中文`nemoji 😀`n"
    $created = Invoke-TestRpc 'document.create' @{text=$expectedText}
    $read = Invoke-TestRpc 'document.get' @{id=$created.id}
    if ($read.text -cne $expectedText) { throw 'CLI changed Unicode, newline, or literal backslash content' }
    $saving = Invoke-TestRpc 'workspace.save' @{id=$created.id; baseRevision=$created.revision; path='保存.txt'}
    $saveDeadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        $saved = Invoke-TestRpc 'operation.get' @{id=$saving.operationId}
        if ($saved.state -ne 'pending') { break }
        Start-Sleep -Milliseconds 10
    } while ([DateTime]::UtcNow -lt $saveDeadline)
    if ($saved.state -ne 'completed') { throw "Save did not complete: $($saved | ConvertTo-Json -Depth 10)" }
    if ([IO.File]::ReadAllText((Join-Path $testRoot '保存.txt')) -cne $expectedText) { throw 'Saved bytes differ from CLI content' }
    Write-Output 'Astra server process + TCP CLI + Unicode argv + real async save: PASS'
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
