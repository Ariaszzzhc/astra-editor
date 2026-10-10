[CmdletBinding()]
param([switch]$Check, [switch]$Offline)

$ErrorActionPreference = 'Stop'
if (-not $IsWindows -or [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64') {
    throw 'This formatting entry point supports native Windows x64.'
}
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$manifest = Get-Content (Join-Path $repository 'tools/formatters.json') -Raw | ConvertFrom-Json
$cache = Join-Path $repository '.tools/format'
New-Item -ItemType Directory -Path $cache -Force | Out-Null

function Assert-Archive([string]$Path, $Asset) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        if ($Asset.sha256) {
            $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)).ToLowerInvariant()
            if ($hash -cne $Asset.sha256) {
                throw "SHA-256 mismatch: $Path"
            }
        } else {
            $hash = 'sha512-' + [Convert]::ToBase64String([Security.Cryptography.SHA512]::HashData($stream))
            if ($hash -cne $Asset.integrity) {
                throw "SHA-512 integrity mismatch: $Path"
            }
        }
    } finally {
        $stream.Dispose()
    }
}

function Get-Archive($Asset) {
    $name = ([Uri]$Asset.archive).Segments[-1]
    $path = Join-Path $cache $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        if ($Offline) {
            throw "Missing cached formatting archive: $name"
        }
        $partial = $path + '.download'
        Invoke-WebRequest -Uri $Asset.archive -OutFile $partial -TimeoutSec 180
        Assert-Archive $partial $Asset
        Move-Item -LiteralPath $partial -Destination $path -Force
    }
    Assert-Archive $path $Asset
    return $path
}

function Expand-NpmArchive([string]$Archive, [string]$Destination) {
    $source = [IO.File]::OpenRead($Archive)
    $gzip = [IO.Compression.GZipStream]::new($source, [IO.Compression.CompressionMode]::Decompress)
    $reader = [System.Formats.Tar.TarReader]::new($gzip)
    $prefix = [IO.Path]::GetFullPath($Destination).TrimEnd('\') + '\'
    try {
        while ($entry = $reader.GetNextEntry()) {
            if (-not $entry.Name.StartsWith('package/', [StringComparison]::Ordinal)) {
                throw 'Unexpected npm archive root'
            }
            $relative = $entry.Name.Substring(8)
            if (-not $relative) {
                continue
            }
            if ($relative.Contains('\') -or $relative.Contains(':') -or ($relative.Split('/') -contains '..')) {
                throw 'Unsafe npm archive path'
            }
            $target = [IO.Path]::GetFullPath((Join-Path $Destination $relative))
            if (-not $target.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Archive path escapes formatting cache'
            }
            if ($entry.EntryType -eq [System.Formats.Tar.TarEntryType]::Directory) {
                New-Item -ItemType Directory -Path $target -Force | Out-Null
            } elseif ($entry.EntryType -in @([System.Formats.Tar.TarEntryType]::RegularFile, [System.Formats.Tar.TarEntryType]::V7RegularFile)) {
                New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($target)) -Force | Out-Null
                $output = [IO.File]::Create($target)
                try {
                    if ($entry.DataStream) {
                        $entry.DataStream.CopyTo($output)
                    }
                } finally {
                    $output.Dispose()
                }
            } else {
                throw 'Unsupported npm archive entry type'
            }
        }
    } finally {
        $reader.Dispose()
        $gzip.Dispose()
        $source.Dispose()
    }
}

$clangArchive = Get-Archive $manifest.clangFormat
$prettierArchive = Get-Archive $manifest.prettier
$clangDirectory = Join-Path $cache ('clang-format-' + $manifest.clangFormat.version)
$prettierDirectory = Join-Path $cache ('prettier-' + $manifest.prettier.version)
# Re-extract verified packages so edited cache contents cannot change formatting.
[IO.Compression.ZipFile]::ExtractToDirectory($clangArchive, $clangDirectory, $true)
Expand-NpmArchive $prettierArchive $prettierDirectory
$clang = Join-Path $clangDirectory $manifest.clangFormat.executable
$node = Join-Path $repository '.tools/lsp/tools/node/node.exe'
if (-not (Test-Path -LiteralPath $node -PathType Leaf)) {
    throw 'Prepare the pinned Node runtime first: ./scripts/prepare-language-tools.ps1'
}
$expectedNode = (Get-Content (Join-Path $repository 'languages/manifest.json') -Raw | ConvertFrom-Json).node.version
$actualNode = & $node --version
if ($LASTEXITCODE -ne 0 -or $actualNode -cne ('v' + $expectedNode)) {
    throw 'Formatting requires the repository-pinned Node runtime'
}

$cpp = @()
$javascript = @()
foreach ($directory in @('include', 'src', 'tests', 'data')) {
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $repository $directory) -Recurse -File) {
        if ($file.Extension -in @('.cpp', '.hpp', '.h', '.cc')) {
            $cpp += $file.FullName
        }
        if ($file.Extension -in @('.js', '.mjs')) {
            $javascript += $file.FullName
        }
    }
}
Push-Location $repository
try {
    $clangArguments = @('-i')
    if ($Check) {
        $clangArguments = @('--dry-run', '--Werror')
    }
    & $clang '--style=file' '--fallback-style=none' @clangArguments @cpp
    if ($LASTEXITCODE -ne 0) {
        throw 'C++ formatting failed; run ./scripts/format.ps1'
    }
    $prettierMode = if ($Check) { '--check' } else { '--write' }
    $prettierScript = Join-Path $prettierDirectory 'bin/prettier.cjs'
    $prettierConfig = Join-Path $repository '.prettierrc.json'
    & $node $prettierScript '--config' $prettierConfig $prettierMode @javascript
    if ($LASTEXITCODE -ne 0) {
        throw 'JS formatting failed; run ./scripts/format.ps1'
    }
    Write-Output "Formatting passed: $($cpp.Count) C++ and $($javascript.Count) JS files."
} finally {
    Pop-Location
}
