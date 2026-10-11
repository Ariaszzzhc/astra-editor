[CmdletBinding()]
param([switch]$Offline)

$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$cache = Join-Path $repository '.tools/grammars'
$manifest = Get-Content -LiteralPath (Join-Path $repository 'grammars/manifest.json') -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path $cache | Out-Null

function Assert-Hash([string] $Path, [string] $Expected) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing grammar source: $Path" }
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -cne $Expected) { throw "Grammar SHA256 mismatch: $Path" }
}

foreach ($package in $manifest.packages) {
    $archive = Join-Path $cache ($package.name + '.zip')
    if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
        if ($Offline) { throw "Missing cached grammar archive: $archive" }
        $download = $archive + '.download'
        Invoke-WebRequest -Uri $package.archive -OutFile $download -TimeoutSec 180
        Assert-Hash $download $package.sha256
        Move-Item -LiteralPath $download -Destination $archive
    }
    Assert-Hash $archive $package.sha256
    $source = Join-Path $cache $package.directory
    if (-not (Test-Path -LiteralPath $source -PathType Container)) {
        Expand-Archive -LiteralPath $archive -DestinationPath $cache
    }
    foreach ($property in $package.files.psobject.Properties) {
        $file = [IO.Path]::GetFullPath((Join-Path $source $property.Name))
        $prefix = [IO.Path]::GetFullPath($source).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
        if (-not $file.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Grammar manifest path escapes package directory: $($property.Name)"
        }
        Assert-Hash $file $property.Value
    }
    Write-Output "Verified $($package.name) $($package.tag) ($($package.commit))"
}
