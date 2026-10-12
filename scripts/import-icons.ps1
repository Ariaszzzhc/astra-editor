[CmdletBinding()]
param([switch]$Offline)
$ErrorActionPreference = 'Stop'
if (-not $IsWindows) { throw 'Use this maintenance entry point on Windows.' }
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $PSScriptRoot 'release-common.ps1')
$manifest = Get-Content (Join-Path $repository 'dependencies/icons/manifest.json') -Raw | ConvertFrom-Json
$cache = Join-Path $repository '.tools/icons'
[void][IO.Directory]::CreateDirectory($cache)
$fresh = Assert-ReleaseChild $cache (Join-Path $cache ('import-' + [Guid]::NewGuid().ToString('N')))
[void][IO.Directory]::CreateDirectory($fresh)
foreach ($property in $manifest.sources.psobject.Properties) {
    $source = $property.Value
    $archive = Get-ReleaseArchive $source @((Join-Path $cache $source.cacheFile)) -Offline:$Offline
    $localArchive = Assert-ReleaseChild $cache (Join-Path $cache $source.cacheFile)
    if ($archive -ne $localArchive) { Copy-Item -LiteralPath $archive -Destination $localArchive }
    Copy-Item -LiteralPath $localArchive -Destination (Join-Path $fresh $source.cacheFile)
    $directory = Assert-ReleaseChild $fresh (Join-Path $fresh $source.cacheDirectory)
    if ($property.Name -eq 'codicons') {
        Expand-ReleaseZip $localArchive $directory ($source.cacheDirectory + '/')
    } else {
        Expand-ReleaseNpm $localArchive $directory
    }
}
$pointer = Get-Content (Join-Path $repository '.tools/runtime/windows-x64.json') -Raw | ConvertFrom-Json
$runtime = Assert-ReleaseChild (Join-Path $repository '.tools/runtime') (Join-Path $repository $pointer.directory)
Assert-ReleaseHash (Join-Path $runtime 'runtime-manifest.json') $pointer.manifestSha256
[void](Assert-ReleaseInventory $runtime 'runtime-manifest.json')
& (Join-Path $runtime 'tools/node/node.exe') (Join-Path $PSScriptRoot 'import-icons.mjs') $fresh
if ($LASTEXITCODE -ne 0) { throw 'Icon import failed' }
