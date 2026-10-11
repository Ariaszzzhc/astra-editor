[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/windows/x64/release',
    [string]$OutputDirectory = '',
    [switch]$Offline,
    [switch]$NoArchive
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release-common.ps1')
if (-not $IsWindows -or [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64') {
    throw 'This package script requires native Windows x64.'
}
$repo = $ReleaseRepository
$build = Assert-ReleaseChild (Join-Path $repo 'build') $BuildDirectory
$project = [IO.File]::ReadAllText((Join-Path $repo 'xmake.lua'))
if ($project -notmatch 'set_version\("([0-9]+[.][0-9]+[.][0-9]+)"\)') { throw 'Missing Astra version' }
$version = $Matches[1]
$name = 'astra-' + $version + '-windows-x64'
if (-not $OutputDirectory) { $OutputDirectory = 'build/packages/' + $name }
$output = Assert-ReleaseChild (Join-Path $repo 'build') $OutputDirectory
if (Test-Path -LiteralPath $output) { throw "Output already exists; choose a new OutputDirectory: $output" }
if (Test-Path -LiteralPath ($output + '.zip')) { throw 'Output archive already exists' }
& (Join-Path $PSScriptRoot 'prepare-windows-runtime.ps1') -Offline:$Offline
$pointer = Get-Content (Join-Path $repo '.tools/runtime/windows-x64.json') -Raw | ConvertFrom-Json
$runtime = Assert-ReleaseChild (Join-Path $repo '.tools/runtime') (Join-Path $repo $pointer.directory)
Assert-ReleaseHash (Join-Path $runtime 'runtime-manifest.json') $pointer.manifestSha256
[void](Assert-ReleaseInventory $runtime 'runtime-manifest.json')
$graphicsRoot = Join-Path $repo '.tools/graphics/install/x64/release'
$graphics = Get-Content (Join-Path $graphicsRoot 'manifest.json') -Raw | ConvertFrom-Json
if ($graphics.configuration -cne 'Release' -or $graphics.platform -cne 'windows' -or $graphics.architecture -cne 'x64' -or $graphics.runtime -cne 'MT') {
    throw 'Release Windows x64 /MT graphics manifest required'
}
$staging = Assert-ReleaseChild (Join-Path $repo 'build') ($output + '.staging-' + [Guid]::NewGuid().ToString('N'))
try {
    Copy-ReleaseTree $runtime $staging
    Copy-ReleaseTree (Join-Path $repo 'data') (Join-Path $staging 'data')
    $inputs = [ordered]@{}
    foreach ($file in @('astra.exe','astra-server.exe','astra-process-host.exe','astra-clipboard-host.exe','SDL3.dll','icudtl.dat','dxcompiler.dll','dxil.dll')) {
        $source = Assert-ReleaseChild $build (Join-Path $build $file)
        $inputs[$file] = Get-ReleaseHash $source
        [IO.File]::Copy($source, (Join-Path $staging $file))
        Assert-ReleaseHash $source $inputs[$file]
    }
    foreach ($runtimeFile in $graphics.runtime_files) {
        $record = @($graphics.artifacts | Where-Object { $_.file -ceq $runtimeFile })
        if ($record.Count -ne 1) { throw 'Missing graphics artifact hash' }
        Assert-ReleaseHash (Join-Path $staging ([IO.Path]::GetFileName($runtimeFile))) $record[0].sha256
    }
    $sdl = @(Get-ChildItem -Path (Join-Path $repo '.tools/packages/a/astra-sdl3/3.4.12/*/bin/SDL3.dll') -File)
    if (-not @($sdl | Where-Object { (Get-ReleaseHash $_.FullName) -ceq $inputs['SDL3.dll'] }).Count) {
        throw 'SDL3.dll does not match the installed pinned SDL package'
    }
    [IO.File]::Copy((Join-Path $graphicsRoot 'manifest.json'), (Join-Path $staging 'provenance/graphics-build.json'))
    [IO.File]::Copy((Join-Path $graphicsRoot 'args.gn'), (Join-Path $staging 'provenance/graphics-args.gn'))
    [IO.File]::Copy((Join-Path $repo 'release/README-WINDOWS.md'), (Join-Path $staging 'README.md'))
    $commit = (& git -C $repo rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Could not determine source revision' }
    $changes = @(& git -C $repo status --porcelain --untracked-files=normal)
    if ($LASTEXITCODE -ne 0) { throw 'Could not determine source status' }
    $sourceHashes = [ordered]@{}
    foreach ($file in @(& git -C $repo ls-files --cached --others --exclude-standard)) {
        if (Test-Path -LiteralPath (Join-Path $repo $file) -PathType Leaf) { $sourceHashes[$file] = Get-ReleaseHash (Join-Path $repo $file) }
    }
    $sourceHashes | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $staging 'provenance/source-files.json') -Encoding utf8NoBOM
    Write-ReleaseInventory $staging 'package-manifest.json' ([ordered]@{
        schema=1;product='Astra';version=$version;platform='windows';architecture='x64';configuration='Release';
        sourceCommit=$commit;sourceDirty=($changes.Count -ne 0);runtimeIdentity=([IO.Path]::GetFileName($runtime));inputs=$inputs
    })
    & (Join-Path $PSScriptRoot 'verify-windows-package.ps1') -PackageDirectory $staging
    Move-Item -LiteralPath $staging -Destination $output
    if (-not $NoArchive) {
        # Fixed entry order and timestamp make identical payloads reproduce the
        # same ZIP; this does not claim the MSVC binaries themselves are bitwise deterministic.
        $archive = [IO.Compression.ZipFile]::Open($output + '.zip', [IO.Compression.ZipArchiveMode]::Create)
        try {
            $files = @(Get-ChildItem -LiteralPath $output -Recurse -File | Sort-Object FullName)
            foreach ($file in $files) {
                $relative = [IO.Path]::GetRelativePath($output, $file.FullName).Replace('\','/')
                $entry = $archive.CreateEntry($name + '/' + $relative, [IO.Compression.CompressionLevel]::Optimal)
                $entry.LastWriteTime = [DateTimeOffset]::new(2000,1,1,0,0,0,[TimeSpan]::Zero)
                $inputStream = [IO.File]::OpenRead($file.FullName)
                $outputStream = $entry.Open()
                try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose(); $inputStream.Dispose() }
            }
        } finally { $archive.Dispose() }
        $hash = Get-ReleaseHash ($output + '.zip')
        [IO.File]::WriteAllText($output + '.zip.sha256', $hash + '  ' + [IO.Path]::GetFileName($output) + ".zip`n")
        Write-Output "Archive SHA-256: $hash"
    }
    Write-Output "Windows package: $output"
} finally {
    $checked = Assert-ReleaseChild (Join-Path $repo 'build') $staging
    if (Test-Path -LiteralPath $checked) { Remove-Item -LiteralPath $checked -Recurse -Force }
}
