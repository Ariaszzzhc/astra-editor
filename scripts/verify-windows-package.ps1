[CmdletBinding()]
param([Parameter(Mandatory)][string]$PackageDirectory, [switch]$RunSmoke)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release-common.ps1')
if (-not $IsWindows) { throw 'Windows package verification must run on Windows.' }
$package = Assert-ReleaseChild (Join-Path $ReleaseRepository 'build') $PackageDirectory
$manifest = Assert-ReleaseInventory $package 'package-manifest.json'
if ($manifest.schema -ne 1 -or $manifest.product -cne 'Astra' -or $manifest.platform -cne 'windows' -or $manifest.architecture -cne 'x64') {
    throw 'Unexpected package identity'
}
$required = @('astra.exe','astra-server.exe','astra-process-host.exe','SDL3.dll','dxcompiler.dll','dxil.dll','icudtl.dat',
    'data/defaults.js','tools/clangd/bin/clangd.exe','tools/clangd/LICENSE.TXT','tools/node/node.exe','tools/node/LICENSE',
    'tools/node_modules/typescript/lib/typescript.js','tools/node_modules/typescript/LICENSE.txt',
    'tools/node_modules/typescript/ThirdPartyNoticeText.txt','tools/node_modules/typescript-language-server/lib/cli.mjs',
    'tools/node_modules/typescript-language-server/LICENSE','tools/node_modules/basedpyright/dist/pyright-langserver.js',
    'tools/node_modules/basedpyright/LICENSE.txt','tools/node_modules/basedpyright/dist/typeshed-fallback/LICENSE',
    'tools/git/cmd/git.exe','tools/git/LICENSE.txt','THIRD-PARTY.md','README.md','licenses/SDL3/LICENSE.txt',
    'licenses/graphics/third_party/externals/icu/LICENSE','provenance/graphics-build.json','provenance/source-files.json')
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $package $relative) -PathType Leaf)) { throw "Required package file missing: $relative" }
}
foreach ($exe in @('astra.exe','astra-server.exe','astra-process-host.exe','tools/node/node.exe','tools/clangd/bin/clangd.exe','tools/git/cmd/git.exe')) {
    $bytes = [IO.File]::ReadAllBytes((Join-Path $package $exe))
    if ($bytes.Length -lt 64 -or $bytes[0] -ne 77 -or $bytes[1] -ne 90) { throw "Invalid PE file: $exe" }
    $offset = [BitConverter]::ToInt32($bytes, 60)
    if ($offset -lt 64 -or $offset + 6 -gt $bytes.Length -or [BitConverter]::ToUInt32($bytes,$offset) -ne 0x4550 -or [BitConverter]::ToUInt16($bytes,$offset + 4) -ne 0x8664) {
        throw "Expected an x64 PE executable: $exe"
    }
}
Write-Output "Windows package inventory: PASS ($($manifest.files.Count) files)"
if (-not $RunSmoke) { return }

# Fresh cwd and a minimal PATH prevent development .tools or globally installed
# language servers/Git from making an incomplete package pass. No user profile
# directories or global Git configuration are read by the child processes.
$fixture = Assert-ReleaseChild (Join-Path $ReleaseRepository 'build') (Join-Path $ReleaseRepository ('build/package smoke 中文 ' + [Guid]::NewGuid().ToString('N')))
[void][IO.Directory]::CreateDirectory($fixture)
foreach ($name in @('workspace','state','temp','profile')) { [void][IO.Directory]::CreateDirectory((Join-Path $fixture $name)) }
$workspace = Join-Path $fixture 'workspace'
$server = $null
$passed = $false
function Start-PackageProcess([string]$Executable, [string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = Join-Path $package $Executable
    $info.WorkingDirectory = $workspace
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    $info.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
    $info.Environment['PATH'] = (Join-Path $env:SystemRoot 'System32') + ';' + $env:SystemRoot
    foreach ($variable in @('ASTRA_RESOURCE_DIR','NODE_PATH','NODE_OPTIONS','PYTHONPATH','PYTHONHOME','GIT_EXEC_PATH','GIT_CONFIG','GIT_CONFIG_COUNT')) {
        [void]$info.Environment.Remove($variable)
    }
    foreach ($variable in @('TEMP','TMP')) { $info.Environment[$variable] = Join-Path $fixture 'temp' }
    foreach ($variable in @('HOME','USERPROFILE','APPDATA','LOCALAPPDATA','XDG_CONFIG_HOME','XDG_CACHE_HOME')) { $info.Environment[$variable] = Join-Path $fixture 'profile' }
    $info.Environment['GIT_CONFIG_NOSYSTEM'] = '1'
    $info.Environment['GIT_CONFIG_GLOBAL'] = 'NUL'
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    return [Diagnostics.Process]::Start($info)
}
function Invoke-PackageProcess([string]$Executable, [string[]]$Arguments) {
    $process = Start-PackageProcess $Executable $Arguments
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(20000)) { throw "Process timed out: $Executable" }
        if ($process.ExitCode -ne 0) { throw "$Executable failed ($($process.ExitCode)): $($stderr.GetAwaiter().GetResult())" }
        return $stdout.GetAwaiter().GetResult()
    } finally {
        if (-not $process.HasExited) { $process.Kill($true); $process.WaitForExit() }
        $process.Dispose()
    }
}
try {
    foreach ($exe in @('astra.exe','astra-server.exe')) {
        $version = Invoke-PackageProcess $exe @('--version')
        if ($version -notmatch 'Astra') { throw "Unexpected version output: $version" }
    }
    [void](Invoke-PackageProcess 'tools/git/cmd/git.exe' @('init','--quiet',$workspace))
    [IO.File]::WriteAllText((Join-Path $workspace 'broken.cpp'), "int main() { return astra_missing_symbol; }`n")
    [IO.File]::WriteAllText((Join-Path $workspace 'broken.ts'), "const count: number = 'not a number';`nexport {};`n")
    [IO.File]::WriteAllText((Join-Path $workspace 'broken.py'), "answer: int = 'not an integer'`n")
    $server = Start-PackageProcess 'astra-server.exe' @('--workspace',$workspace,'--port','0','--state-directory',(Join-Path $fixture 'state'))
    $stderr = $server.StandardError.ReadToEndAsync()
    $readyTask = $server.StandardOutput.ReadLineAsync()
    if (-not $readyTask.Wait(15000)) { throw 'Packaged server startup timed out' }
    $ready = $readyTask.Result | ConvertFrom-Json
    if ($ready.application -cne 'Astra' -or $ready.port -le 0) { throw "Invalid server readiness: $($readyTask.Result)" }
    function Invoke-PackageRpc([string]$Method, [hashtable]$Params) {
        $body = @{jsonrpc='2.0';id='package';method=$Method;params=$Params} | ConvertTo-Json -Depth 20 -Compress
        $reply = (Invoke-PackageProcess 'astra-server.exe' @('--connect',"127.0.0.1:$($ready.port)",'--request',$body)) | ConvertFrom-Json
        if ($reply.error) { throw "RPC $Method failed: $($reply.error | ConvertTo-Json -Compress)" }
        return $reply.result
    }
    function Wait-PackageOperation($Operation) {
        $deadline = [DateTime]::UtcNow.AddSeconds(20)
        do {
            $result = Invoke-PackageRpc 'operation.get' @{id=$Operation.operationId}
            if ($result.state -ceq 'completed') { return $result.result }
            if ($result.state -ceq 'failed') { throw "Operation failed: $($result | ConvertTo-Json -Depth 15 -Compress)" }
            Start-Sleep -Milliseconds 25
        } while ([DateTime]::UtcNow -lt $deadline)
        throw 'Packaged service operation timed out'
    }
    $text = "中文 Astra 😀`nC:\code\literal`n"
    $document = Invoke-PackageRpc 'document.create' @{text=$text}
    $saved = Wait-PackageOperation (Invoke-PackageRpc 'workspace.save' @{id=$document.id;baseRevision=$document.revision;path='保存.txt'})
    if ([IO.File]::ReadAllText((Join-Path $workspace '保存.txt')) -cne $text) { throw 'Packaged Unicode save changed content' }
    $status = Wait-PackageOperation (Invoke-PackageRpc 'git.status' @{})
    if (-not @($status.entries | Where-Object { $_.path -ceq '保存.txt' }).Count) { throw 'Bundled Git did not report the saved file' }
    foreach ($file in @('broken.cpp','broken.ts','broken.py')) {
        $opened = Wait-PackageOperation (Invoke-PackageRpc 'workspace.open' @{path=$file})
        $deadline = [DateTime]::UtcNow.AddSeconds(40)
        do {
            $diagnostics = Invoke-PackageRpc 'language.diagnostics' @{id=$opened.id}
            if ($diagnostics.available -and -not $diagnostics.pending -and @($diagnostics.diagnostics).Count -gt 0) { break }
            Start-Sleep -Milliseconds 100
        } while ([DateTime]::UtcNow -lt $deadline)
        if (-not $diagnostics.available -or $diagnostics.pending -or @($diagnostics.diagnostics).Count -eq 0) {
            throw "Bundled language server did not diagnose $file : $($diagnostics | ConvertTo-Json -Depth 12 -Compress)"
        }
        Write-Output "Bundled language diagnostics: PASS ($file)"
    }
    [void](Wait-PackageOperation (Invoke-PackageRpc 'recovery.flush' @{}))
    $passed = $true
    Write-Output 'Isolated packaged server + process host + Unicode save + Git + clangd + TypeScript + Python + recovery: PASS'
} finally {
    if ($server) {
        if (-not $server.HasExited) { $server.Kill($true); $server.WaitForExit() }
        if ($stderr) { [IO.File]::WriteAllText((Join-Path $fixture 'server.stderr.log'), $stderr.GetAwaiter().GetResult()) }
        $server.Dispose()
    }
    if ($passed) {
        $checked = Assert-ReleaseChild (Join-Path $ReleaseRepository 'build') $fixture
        Remove-Item -LiteralPath $checked -Recurse -Force
    } else { Write-Output "Failure fixture preserved: $fixture" }
}
