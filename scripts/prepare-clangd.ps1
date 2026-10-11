param(
    [string]$PackageDirectory = ""
)

$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$manifest = (Get-Content -LiteralPath (Join-Path $repository 'dependencies/runtime/manifest.json') -Raw | ConvertFrom-Json).clangd
$version = $manifest.version
$sha256 = $manifest.sha256
$url = $manifest.archive
$cache = Join-Path $repository '.tools/lsp'
$archive = Join-Path $cache "clangd-windows-$version.zip"
$extracted = Join-Path $cache $manifest.directory

if (-not $IsWindows) { throw 'This preparation script currently installs the verified Windows clangd archive only.' }
New-Item -ItemType Directory -Force -Path $cache | Out-Null
if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
    Invoke-WebRequest -Uri $url -OutFile $archive
}
$actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -ne $sha256) { throw "clangd archive SHA-256 mismatch: expected $sha256, received $actual" }
# Re-extract the verified archive so a modified cached executable cannot silently
# enter a release package. Expand-Archive does not remove unrelated files.
Expand-Archive -LiteralPath $archive -DestinationPath $cache -Force
$executable = Join-Path $extracted 'bin/clangd.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw 'Verified clangd archive did not contain bin/clangd.exe' }
foreach ($notice in $manifest.licenseFiles.psobject.Properties) {
    $noticeHash = (Get-FileHash -LiteralPath (Join-Path $extracted $notice.Name) -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($noticeHash -cne $notice.Value) { throw "clangd license hash mismatch: $($notice.Name)" }
}

if ($PackageDirectory) {
    $package = [IO.Path]::GetFullPath($PackageDirectory, $repository)
    $prefix = $repository.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $package.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'PackageDirectory must be an explicit directory inside this repository.'
    }
    $destination = Join-Path $package 'tools/clangd'
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    foreach ($item in Get-ChildItem -LiteralPath $extracted) {
        Copy-Item -LiteralPath $item.FullName -Destination $destination -Recurse -Force
    }
    foreach ($notice in $manifest.licenseFiles.psobject.Properties) {
        $noticeHash = (Get-FileHash -LiteralPath (Join-Path $destination $notice.Name) -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($noticeHash -cne $notice.Value) { throw "Packaged clangd license hash mismatch: $($notice.Name)" }
    }
    Write-Output "Packaged clangd $version at $destination (including LLVM license and resource headers)."
}
Write-Output "Verified Windows clangd $version`: $executable"
Write-Output "SHA-256: $sha256"
