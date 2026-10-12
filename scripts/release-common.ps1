# Shared, side-effect-free Windows release helpers. Sources and destinations must
# remain inside this checkout. Never traverse junctions while copying a package.
$ReleaseRepository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

function Assert-ReleaseChild([string]$Root, [string]$Path) {
    $absolute = [IO.Path]::GetFullPath($Path, $ReleaseRepository)
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $absolute.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escapes intended directory: $Path"
    }
    $ancestor = $absolute
    while ($ancestor -and $ancestor.Length -ge $ReleaseRepository.Length) {
        if (Test-Path -LiteralPath $ancestor) {
            if ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Reparse point refused: $ancestor"
            }
        }
        $ancestor = [IO.Path]::GetDirectoryName($ancestor)
    }
    return $absolute
}
function Get-ReleaseHash([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Assert-ReleaseIcons([string]$Directory) {
    $catalog = Get-Content (Get-ArchiveTarget $Directory 'provenance/icons.json') -Raw | ConvertFrom-Json
    if ($catalog.schema -ne 1 -or $catalog.assets.Count -lt 1 -or $catalog.assets.Count -gt 4096) {
        throw 'Invalid installed icon provenance'
    }
    $ids = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($asset in $catalog.assets) {
        if (-not $ids.Add($asset.id) -or -not $asset.path.StartsWith('resources/icons/', [StringComparison]::Ordinal)) {
            throw 'Invalid installed icon identity'
        }
        Assert-ReleaseHash (Get-ArchiveTarget $Directory $asset.path) $asset.sha256
    }
    Assert-ReleaseHash (Get-ArchiveTarget $Directory $catalog.associations.path) $catalog.associations.sha256
    foreach ($notice in $catalog.notices) {
        $prefix = 'dependencies/icons/licenses/'
        if (-not $notice.path.StartsWith($prefix, [StringComparison]::Ordinal)) { throw 'Invalid icon notice path' }
        $path = 'licenses/icons/' + $notice.path.Substring($prefix.Length)
        Assert-ReleaseHash (Get-ArchiveTarget $Directory $path) $notice.sha256
    }
    return $catalog.assets.Count
}
function Assert-ReleaseHash([string]$Path, [string]$Expected) {
    if ($Expected -notmatch '^[a-f0-9]{64}$' -or (Get-ReleaseHash $Path) -cne $Expected) {
        throw "SHA-256 mismatch: $Path"
    }
}
function Get-ReleaseArchive($Asset, [string[]]$Candidates, [switch]$Offline) {
    $directory = Assert-ReleaseChild $ReleaseRepository (Join-Path $ReleaseRepository '.tools/release-downloads')
    $path = Assert-ReleaseChild $directory (Join-Path $directory (([Uri]$Asset.archive).Segments[-1]))
    foreach ($candidate in @($Candidates) + @($path)) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            [void](Assert-ReleaseChild $ReleaseRepository $candidate)
            Assert-ReleaseHash $candidate $Asset.sha256
            return $candidate
        }
    }
    if ($Offline) { throw "Missing verified archive: $($Asset.archive)" }
    [void][IO.Directory]::CreateDirectory($directory)
    if (-not (Test-Path -LiteralPath $path)) {
        $partial = $path + '.' + [Guid]::NewGuid().ToString('N') + '.download'
        Invoke-WebRequest -Uri $Asset.archive -OutFile $partial -TimeoutSec 180
        Assert-ReleaseHash $partial $Asset.sha256
        Move-Item -LiteralPath $partial -Destination $path
    }
    Assert-ReleaseHash $path $Asset.sha256
    return $path
}
function Get-ArchiveTarget([string]$Root, [string]$Name) {
    if ($Name.Contains('\') -or $Name.Contains(':') -or $Name.StartsWith('/') -or $Name.Split('/') -contains '..') {
        throw "Unsafe archive entry: $Name"
    }
    # Root is a new private directory and all archive links are rejected.
    $target = [IO.Path]::GetFullPath((Join-Path $Root $Name))
    if (-not $target.StartsWith($Root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe archive entry: $Name"
    }
    return $target
}
function Expand-ReleaseZip([string]$Archive, [string]$Destination, [string]$Prefix = '', [string[]]$Only = @()) {
    [void](Assert-ReleaseChild $ReleaseRepository $Destination)
    [void][IO.Directory]::CreateDirectory($Destination)
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        foreach ($entry in $zip.Entries) {
            if (-not $entry.FullName.StartsWith($Prefix, [StringComparison]::Ordinal)) { continue }
            $relative = $entry.FullName.Substring($Prefix.Length)
            if (-not $relative -or $relative.EndsWith('/')) { continue }
            if ($Only.Count -and $relative -cnotin $Only) { continue }
            if ((($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000) { throw 'Archive symlink refused' }
            $target = Get-ArchiveTarget $Destination $relative
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
            $input = $entry.Open()
            $output = [IO.File]::Open($target, [IO.FileMode]::CreateNew)
            try { $input.CopyTo($output) } finally { $output.Dispose(); $input.Dispose() }
        }
    } finally { $zip.Dispose() }
}
function Expand-ReleaseNpm([string]$Archive, [string]$Destination) {
    [void](Assert-ReleaseChild $ReleaseRepository $Destination)
    [void][IO.Directory]::CreateDirectory($Destination)
    $source = [IO.File]::OpenRead($Archive)
    $gzip = [IO.Compression.GZipStream]::new($source, [IO.Compression.CompressionMode]::Decompress)
    $reader = [System.Formats.Tar.TarReader]::new($gzip)
    try {
        while ($entry = $reader.GetNextEntry()) {
            if (-not $entry.Name.StartsWith('package/', [StringComparison]::Ordinal)) { throw 'Unexpected npm archive root' }
            $relative = $entry.Name.Substring(8)
            if (-not $relative) { continue }
            $target = Get-ArchiveTarget $Destination $relative
            if ($entry.EntryType -eq [System.Formats.Tar.TarEntryType]::Directory) {
                [void][IO.Directory]::CreateDirectory($target)
            } elseif ($entry.EntryType -in @([System.Formats.Tar.TarEntryType]::RegularFile, [System.Formats.Tar.TarEntryType]::V7RegularFile)) {
                [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
                $output = [IO.File]::Open($target, [IO.FileMode]::CreateNew)
                try { if ($entry.DataStream) { $entry.DataStream.CopyTo($output) } } finally { $output.Dispose() }
            } else { throw "Archive link/special file refused: $($entry.Name)" }
        }
    } finally { $reader.Dispose(); $gzip.Dispose(); $source.Dispose() }
}
function Assert-ReleaseNotices([string]$Directory, $Asset) {
    foreach ($notice in $Asset.licenseFiles.psobject.Properties) {
        Assert-ReleaseHash (Assert-ReleaseChild $Directory (Join-Path $Directory $notice.Name)) $notice.Value
    }
}
function Expand-ReleaseTarNotices([string]$Archive, [string]$Destination, [string]$Prefix, [string[]]$Only) {
    [void][IO.Directory]::CreateDirectory($Destination)
    $source = [IO.File]::OpenRead($Archive)
    $gzip = [IO.Compression.GZipStream]::new($source, [IO.Compression.CompressionMode]::Decompress)
    $reader = [System.Formats.Tar.TarReader]::new($gzip)
    $remaining = [Collections.Generic.HashSet[string]]::new([string[]]$Only, [StringComparer]::Ordinal)
    try {
        while ($entry = $reader.GetNextEntry()) {
            if (-not $entry.Name.StartsWith($Prefix, [StringComparison]::Ordinal)) { continue }
            $relative = $entry.Name.Substring($Prefix.Length)
            if ($relative -cnotin $Only) { continue }
            if ($entry.EntryType -notin @([System.Formats.Tar.TarEntryType]::RegularFile, [System.Formats.Tar.TarEntryType]::V7RegularFile)) {
                throw 'Notice is not a regular archive file'
            }
            $target = Get-ArchiveTarget $Destination $relative
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
            $output = [IO.File]::Open($target, [IO.FileMode]::CreateNew)
            try { $entry.DataStream.CopyTo($output) } finally { $output.Dispose() }
            [void]$remaining.Remove($relative)
            if ($remaining.Count -eq 0) { break }
        }
    } finally { $reader.Dispose(); $gzip.Dispose(); $source.Dispose() }
}
function Copy-ReleaseTree([string]$Source, [string]$Destination) {
    [void](Assert-ReleaseChild $ReleaseRepository $Source)
    [void](Assert-ReleaseChild $ReleaseRepository $Destination)
    $items = @(Get-ChildItem -LiteralPath $Source -Recurse -Force)
    foreach ($item in $items) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point refused: $($item.FullName)" }
    }
    [void][IO.Directory]::CreateDirectory($Destination)
    foreach ($item in $items | Where-Object { -not $_.PSIsContainer }) {
        $target = Join-Path $Destination ([IO.Path]::GetRelativePath($Source, $item.FullName))
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
        [IO.File]::Copy($item.FullName, $target, $false)
    }
}
function Write-ReleaseInventory([string]$Directory, [string]$Name, $Metadata) {
    $files = @(Get-ChildItem -LiteralPath $Directory -File -Recurse -Force | ForEach-Object {
        if ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Manifest cannot contain links' }
        [ordered]@{path=[IO.Path]::GetRelativePath($Directory, $_.FullName).Replace('\','/'); bytes=$_.Length; sha256=Get-ReleaseHash $_.FullName}
    } | Sort-Object { $_.path })
    $Metadata['files'] = $files
    $json = $Metadata | ConvertTo-Json -Depth 30
    [IO.File]::WriteAllText((Join-Path $Directory $Name), $json + "`n", [Text.UTF8Encoding]::new($false))
}
function Assert-ReleaseInventory([string]$Directory, [string]$Name) {
    [void](Assert-ReleaseChild $ReleaseRepository $Directory)
    $inventory = Get-Content -LiteralPath (Join-Path $Directory $Name) -Raw | ConvertFrom-Json
    # Enumerate once, rejecting every link before hashing. Rechecking every
    # ancestor for each of the thousands of typeshed files is needlessly costly.
    $actual = [Collections.Generic.Dictionary[string,IO.FileInfo]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in Get-ChildItem -LiteralPath $Directory -Recurse -Force) {
        if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Package links are forbidden' }
        if (-not $file.PSIsContainer) { $actual.Add([IO.Path]::GetRelativePath($Directory, $file.FullName).Replace('\','/'), $file) }
    }
    $expected = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    [void]$expected.Add($Name)
    foreach ($entry in $inventory.files) {
        if (-not $expected.Add($entry.path)) { throw "Duplicate manifest path: $($entry.path)" }
        $file = Get-ArchiveTarget $Directory $entry.path
        if (-not $actual.ContainsKey($entry.path)) { throw "Missing package file: $file" }
        if ($actual[$entry.path].Length -ne $entry.bytes) { throw "Size mismatch: $file" }
        Assert-ReleaseHash $file $entry.sha256
    }
    foreach ($file in $actual.Keys) {
        if (-not $expected.Contains($file)) {
            throw "Unmanifested package file: $file"
        }
    }
    return $inventory
}
