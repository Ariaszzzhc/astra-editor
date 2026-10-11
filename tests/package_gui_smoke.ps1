[CmdletBinding()]
param([Parameter(Mandatory)][string]$PackageDirectory)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../scripts/release-common.ps1')
if (-not $IsWindows -or [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64') {
    throw 'Packaged GUI acceptance requires native Windows x64 and an interactive desktop.'
}
$packageRoot = Assert-ReleaseChild (Join-Path $ReleaseRepository 'build') $PackageDirectory
[void](Assert-ReleaseInventory $packageRoot 'package-manifest.json')
$fixtureCandidate = Join-Path $ReleaseRepository ('build/package-gui-' + [Guid]::NewGuid().ToString('N'))
$fixtureRoot = Assert-ReleaseChild (Join-Path $ReleaseRepository 'build') $fixtureCandidate
[void][IO.Directory]::CreateDirectory($fixtureRoot)
foreach ($directory in @('workspace', 'state', 'profile', 'temp')) {
    [void][IO.Directory]::CreateDirectory((Join-Path $fixtureRoot $directory))
}
$workspaceRoot = Join-Path $fixtureRoot 'workspace'
[IO.File]::WriteAllText((Join-Path $workspaceRoot 'source.txt'), "baseline`n")
$scriptText = @'
if (astra.context === 'client') {
    astra.registerCommand('workbench.recoveryScan', async () => {
        await astra.ui.execute('workbench.open', { path: 'source.txt' });
        await astra.ui.native('editor.insert_text', { text: 'Packaged 中🙂\n' });
        await astra.ui.native('editor.undo');
        const before = await astra.ui.read();
        const undone = await astra.request('document.get', { id: before.document.id });
        if (undone.text !== 'baseline\n') {
            throw new Error('Packaged GUI atomic undo mismatch');
        }
        await astra.ui.native('editor.redo');
        await astra.ui.execute('ui.save');
        const { document } = await astra.ui.read();
        const saved = await astra.request('document.get', { id: document.id });
        if (saved.text !== 'Packaged 中🙂\nbaseline\n') {
            throw new Error('Packaged GUI content mismatch');
        }
        await astra.ui.execute('ui.closeWindow');
    });
}
'@
[IO.File]::WriteAllText((Join-Path $workspaceRoot 'acceptance.js'), $scriptText)
$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = Join-Path $packageRoot 'astra.exe'
$startInfo.WorkingDirectory = $workspaceRoot
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$startInfo.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
$startInfo.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
$startInfo.Environment['PATH'] = (Join-Path $env:SystemRoot 'System32') + ';' + $env:SystemRoot
foreach ($variable in @('ASTRA_RESOURCE_DIR', 'NODE_PATH', 'NODE_OPTIONS', 'PYTHONPATH', 'PYTHONHOME', 'GIT_EXEC_PATH', 'GIT_CONFIG', 'GIT_CONFIG_COUNT')) {
    [void]$startInfo.Environment.Remove($variable)
}
foreach ($variable in @('HOME', 'USERPROFILE', 'APPDATA', 'LOCALAPPDATA', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME')) {
    $startInfo.Environment[$variable] = Join-Path $fixtureRoot 'profile'
}
foreach ($variable in @('TEMP', 'TMP')) {
    $startInfo.Environment[$variable] = Join-Path $fixtureRoot 'temp'
}
$startInfo.Environment['GIT_CONFIG_GLOBAL'] = 'NUL'
$startInfo.Environment['GIT_CONFIG_NOSYSTEM'] = '1'
foreach ($argument in @('--workspace', $workspaceRoot, '--state-directory', (Join-Path $fixtureRoot 'state'), '--config', 'acceptance.js')) {
    $startInfo.ArgumentList.Add($argument)
}
$ownedProcess = [Diagnostics.Process]::Start($startInfo)
try {
    $outputTask = $ownedProcess.StandardOutput.ReadToEndAsync()
    $errorTask = $ownedProcess.StandardError.ReadToEndAsync()
    if (-not $ownedProcess.WaitForExit(30000)) {
        throw 'Packaged GUI timed out'
    }
    if ($ownedProcess.ExitCode -ne 0) {
        throw "Packaged GUI failed: $($ownedProcess.ExitCode)"
    }
    if ([IO.File]::ReadAllText((Join-Path $workspaceRoot 'source.txt')) -cne "Packaged 中🙂`nbaseline`n") {
        throw 'Packaged GUI durable Unicode save mismatch'
    }
    Write-Output "Packaged production GUI + shipped JS + native renderer + atomic Unicode edit/undo/redo/save + guarded close: PASS ($fixtureRoot)"
} finally {
    if (-not $ownedProcess.HasExited) {
        $ownedProcess.Kill($true)
        $ownedProcess.WaitForExit()
    }
    [IO.File]::WriteAllText((Join-Path $fixtureRoot 'stdout.log'), $outputTask.GetAwaiter().GetResult())
    [IO.File]::WriteAllText((Join-Path $fixtureRoot 'stderr.log'), $errorTask.GetAwaiter().GetResult())
    $ownedProcess.Dispose()
    Write-Output "Owned fixture retained: $fixtureRoot"
}
