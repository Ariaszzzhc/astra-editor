[CmdletBinding()]
param([string]$PackageDirectory = '', [switch]$IncludeClangd)

$ErrorActionPreference = 'Stop'
if (-not $IsWindows -or [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64') {
    throw 'This script prepares the verified Windows x64 language tools only.'
}
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$manifestPath = Join-Path $repository 'languages/manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$cache = Join-Path $repository '.tools/lsp'
$downloads = Join-Path $cache 'downloads'
$staging = Join-Path $cache ('staging-' + [Guid]::NewGuid().ToString('N'))
$tools = Join-Path $cache 'tools'

function Assert-ChildPath([string]$Root, [string]$Path, [bool]$CheckLinks = $true) {
    $absolute = [IO.Path]::GetFullPath($Path)
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $absolute.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Path escapes intended directory: $Path" }
    $ancestor = $absolute
    while ($CheckLinks -and $ancestor -and $ancestor.StartsWith($repository, [StringComparison]::OrdinalIgnoreCase)) {
        if (Test-Path -LiteralPath $ancestor) {
            $item = Get-Item -LiteralPath $ancestor -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse-point destination refused: $ancestor" }
        }
        $ancestor = [IO.Path]::GetDirectoryName($ancestor)
    }
    return $absolute
}
function Assert-Archive([string]$Path, $Asset) {
    $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -cne $Asset.sha256) { throw "SHA-256 mismatch for $Path" }
    if ($Asset.integrity) {
        $stream = [IO.File]::OpenRead($Path)
        try { $sri = 'sha512-' + [Convert]::ToBase64String([Security.Cryptography.SHA512]::HashData($stream)) }
        finally { $stream.Dispose() }
        if ($sri -cne $Asset.integrity) { throw "npm SHA-512 integrity mismatch for $Path" }
    }
}
function Get-Archive($Asset) {
    $name = ([Uri]$Asset.archive).Segments[-1]
    $archive = Assert-ChildPath $downloads (Join-Path $downloads $name)
    if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
        $partial = $archive + '.download'
        Invoke-WebRequest -Uri $Asset.archive -OutFile $partial -TimeoutSec 180
        Assert-Archive $partial $Asset
        Move-Item -LiteralPath $partial -Destination $archive -Force
    }
    Assert-Archive $archive $Asset
    return $archive
}
function Assert-Notices([string]$Directory, $Asset) {
    foreach ($notice in $Asset.licenseFiles.psobject.Properties) {
        $path = Assert-ChildPath $Directory (Join-Path $Directory $notice.Name)
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -cne $notice.Value) { throw "License/notice hash mismatch: $path" }
    }
}
function Expand-NpmArchive([string]$Archive, [string]$Destination) {
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $source = [IO.File]::OpenRead($Archive)
    $gzip = [IO.Compression.GZipStream]::new($source, [IO.Compression.CompressionMode]::Decompress)
    $reader = [System.Formats.Tar.TarReader]::new($gzip)
    try {
        while ($entry = $reader.GetNextEntry()) {
            if (-not $entry.Name.StartsWith('package/', [StringComparison]::Ordinal)) { throw 'npm archive has an unexpected root' }
            $relative = $entry.Name.Substring(8)
            if (-not $relative) { continue }
            if ($relative.Contains('\') -or $relative.Contains(':') -or ($relative.Split('/') -contains '..')) { throw 'Unsafe npm archive path' }
            # Destination is a fresh private staging directory and archive links
            # are rejected below; avoid repeating filesystem probes per entry.
            $target = Assert-ChildPath $Destination (Join-Path $Destination $relative) $false
            if ($entry.EntryType -eq [System.Formats.Tar.TarEntryType]::Directory) {
                New-Item -ItemType Directory -Path $target -Force | Out-Null
            } elseif ($entry.EntryType -in @([System.Formats.Tar.TarEntryType]::RegularFile, [System.Formats.Tar.TarEntryType]::V7RegularFile)) {
                New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($target)) -Force | Out-Null
                $output = [IO.File]::Create($target)
                try { if ($entry.DataStream) { $entry.DataStream.CopyTo($output) } } finally { $output.Dispose() }
            } else { throw "Unsupported npm archive entry type: $($entry.EntryType)" }
        }
    } finally { $reader.Dispose(); $gzip.Dispose(); $source.Dispose() }
}

try {
    [void](Assert-ChildPath $repository $downloads)
    [void](Assert-ChildPath $repository $staging)
    New-Item -ItemType Directory -Path $downloads, $staging -Force | Out-Null
    $nodeArchive = Get-Archive $manifest.node
    Expand-Archive -LiteralPath $nodeArchive -DestinationPath (Join-Path $staging 'node-archive')
    $nodeSource = Join-Path (Join-Path $staging 'node-archive') $manifest.node.directory
    $prepared = Join-Path $staging 'tools'
    $nodeDestination = Join-Path $prepared 'node'
    New-Item -ItemType Directory -Path $nodeDestination -Force | Out-Null
    foreach ($file in @('node.exe', 'LICENSE', 'README.md')) {
        Copy-Item -LiteralPath (Join-Path $nodeSource $file) -Destination $nodeDestination
    }
    Assert-Notices $nodeDestination $manifest.node
    foreach ($package in $manifest.packages) {
        $destination = Assert-ChildPath $prepared (Join-Path $prepared ('node_modules/' + $package.name))
        Expand-NpmArchive (Get-Archive $package) $destination
        $metadata = Get-Content -LiteralPath (Join-Path $destination 'package.json') -Raw | ConvertFrom-Json
        if ($metadata.name -cne $package.name -or $metadata.version -cne $package.version) { throw 'npm package identity mismatch' }
        if ($metadata.dependencies -and @($metadata.dependencies.psobject.Properties).Count -ne 0) { throw 'Unpinned npm runtime dependency found' }
        if (-not (Test-Path -LiteralPath (Join-Path $destination $package.licenseFile) -PathType Leaf)) { throw "Missing package license: $($package.name)" }
        Assert-Notices $destination $package
        Write-Output "Verified $($package.name) $($package.version) ($($package.license))"
    }
    Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $prepared 'language-manifest.json')
    # Replace only the resolved, repository-owned cache subtree. A running editor
    # must be closed before preparing the tools it currently uses.
    $checkedTools = Assert-ChildPath $cache $tools
    if (Test-Path -LiteralPath $checkedTools) { Remove-Item -LiteralPath $checkedTools -Recurse -Force }
    Move-Item -LiteralPath $prepared -Destination $checkedTools
    Write-Output "Verified Node.js $($manifest.node.version) Windows x64: $checkedTools"
    if ($PackageDirectory) {
        $packageRoot = Assert-ChildPath $repository ([IO.Path]::GetFullPath($PackageDirectory, $repository))
        $packageTools = Join-Path $packageRoot 'tools'
        New-Item -ItemType Directory -Path $packageTools -Force | Out-Null
        foreach ($name in @('node', 'node_modules')) {
            $destination = Assert-ChildPath $packageTools (Join-Path $packageTools $name)
            if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Recurse -Force }
            Copy-Item -LiteralPath (Join-Path $tools $name) -Destination $destination -Recurse
        }
        Copy-Item -LiteralPath (Join-Path $tools 'language-manifest.json') -Destination $packageTools -Force
        Assert-Notices (Join-Path $packageTools 'node') $manifest.node
        foreach ($package in $manifest.packages) { Assert-Notices (Join-Path $packageTools ('node_modules/' + $package.name)) $package }
        Write-Output "Packaged all Node runtime and language package licenses at $packageTools"
    }
    if ($IncludeClangd) { & (Join-Path $PSScriptRoot 'prepare-clangd.ps1') -PackageDirectory $PackageDirectory }
} finally {
    $checkedStaging = Assert-ChildPath $cache $staging
    if (Test-Path -LiteralPath $checkedStaging) { Remove-Item -LiteralPath $checkedStaging -Recurse -Force }
}
