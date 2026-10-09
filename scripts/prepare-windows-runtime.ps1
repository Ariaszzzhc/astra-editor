[CmdletBinding()]
param([switch]$Offline)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release-common.ps1')
if (-not $IsWindows -or [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64') {
    throw 'Only native Windows x64 runtime preparation is supported by this script.'
}
$repo = $ReleaseRepository
$languages = Get-Content (Join-Path $repo 'languages/manifest.json') -Raw | ConvertFrom-Json
$windows = Get-Content (Join-Path $repo 'release/windows-tools.json') -Raw | ConvertFrom-Json
$core = Get-Content (Join-Path $repo 'release/core-notices.json') -Raw | ConvertFrom-Json
$graphics = Get-Content (Join-Path $repo 'release/graphics-notices.json') -Raw | ConvertFrom-Json
$grammars = Get-Content (Join-Path $repo 'grammars/manifest.json') -Raw | ConvertFrom-Json
$lockFiles = @('languages/manifest.json','release/windows-tools.json','release/core-notices.json','release/graphics-notices.json','grammars/manifest.json','dependencies.lock.json','release/THIRD-PARTY.md','scripts/prepare-windows-runtime.ps1','scripts/release-common.ps1')
$lockHashes = [ordered]@{}
foreach ($file in $lockFiles) { $lockHashes[$file] = Get-ReleaseHash (Join-Path $repo $file) }
$identityBytes = [Text.Encoding]::UTF8.GetBytes(($lockHashes | ConvertTo-Json -Compress))
$identity = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($identityBytes)).ToLowerInvariant()
$runtimeRoot = Assert-ReleaseChild $repo (Join-Path $repo '.tools/runtime')
$destination = Assert-ReleaseChild $runtimeRoot (Join-Path $runtimeRoot ('windows-x64-' + $identity.Substring(0,16)))
$staging = Assert-ReleaseChild $runtimeRoot (Join-Path $runtimeRoot ('staging-' + [Guid]::NewGuid().ToString('N')))
if (Test-Path -LiteralPath $destination) {
    $cached = Assert-ReleaseInventory $destination 'runtime-manifest.json'
    if ($cached.identity -cne $identity) { throw 'Runtime cache identity mismatch' }
} else {
    [void][IO.Directory]::CreateDirectory($staging)
    try {
        $tools = Join-Path $staging 'tools'
        $nodeArchive = Get-ReleaseArchive $languages.node @((Join-Path $repo ('.tools/lsp/downloads/' + ([Uri]$languages.node.archive).Segments[-1]))) -Offline:$Offline
        Expand-ReleaseZip $nodeArchive (Join-Path $tools 'node') ($languages.node.directory + '/') @('node.exe','LICENSE','README.md')
        Assert-ReleaseNotices (Join-Path $tools 'node') $languages.node
        Write-Output 'Prepared verified Node runtime'
        foreach ($package in $languages.packages) {
            $archive = Get-ReleaseArchive $package @((Join-Path $repo ('.tools/lsp/downloads/' + ([Uri]$package.archive).Segments[-1]))) -Offline:$Offline
            $stream = [IO.File]::OpenRead($archive)
            try { $sri = 'sha512-' + [Convert]::ToBase64String([Security.Cryptography.SHA512]::HashData($stream)) } finally { $stream.Dispose() }
            if ($sri -cne $package.integrity) { throw "npm integrity mismatch: $($package.name)" }
            $target = Join-Path $tools ('node_modules/' + $package.name)
            Expand-ReleaseNpm $archive $target
            $metadata = Get-Content (Join-Path $target 'package.json') -Raw | ConvertFrom-Json
            if ($metadata.name -cne $package.name -or $metadata.version -cne $package.version -or ($metadata.dependencies -and @($metadata.dependencies.psobject.Properties).Count)) {
                throw "Unexpected npm identity or unpinned dependency: $($package.name)"
            }
            Assert-ReleaseNotices $target $package
            Write-Output "Prepared verified $($package.name)"
        }
        $clangdArchive = Get-ReleaseArchive $languages.clangd @((Join-Path $repo ('.tools/lsp/' + ([Uri]$languages.clangd.archive).Segments[-1]))) -Offline:$Offline
        Expand-ReleaseZip $clangdArchive (Join-Path $tools 'clangd') ($languages.clangd.directory + '/')
        Assert-ReleaseNotices (Join-Path $tools 'clangd') $languages.clangd
        Write-Output 'Prepared verified clangd'
        $gitArchive = Get-ReleaseArchive $windows.git @((Join-Path $repo ('.tools/downloads/' + ([Uri]$windows.git.archive).Segments[-1]))) -Offline:$Offline
        Expand-ReleaseZip $gitArchive (Join-Path $tools 'git')
        foreach ($required in @($windows.git.executable, 'LICENSE.txt', 'ucrt64/share/licenses')) {
            if (-not (Test-Path -LiteralPath (Join-Path $tools ('git/' + $required)))) { throw "MinGit archive missing $required" }
        }
        Write-Output 'Prepared verified MinGit'
        foreach ($package in $core.packages) {
            $candidates = @((Join-Path $repo ('.tools/downloads/' + $package.cacheName)), (Join-Path $repo ('.tools/release-downloads/' + $package.cacheName)))
            $candidates += @(Get-ChildItem (Join-Path $repo '.tools/package-cache') -Recurse -Filter $package.cacheName -File | Select-Object -ExpandProperty FullName)
            $archive = Get-ReleaseArchive $package $candidates -Offline:$Offline
            $target = Join-Path $staging ('licenses/' + $package.name)
            if ($package.archive.EndsWith('.zip')) { Expand-ReleaseZip $archive $target $package.prefix $package.files }
            else { Expand-ReleaseTarNotices $archive $target $package.prefix $package.files }
            foreach ($file in $package.files) {
                if (-not (Test-Path -LiteralPath (Join-Path $target $file) -PathType Leaf)) { throw "Missing $($package.name) license: $file" }
            }
        }
        foreach ($entry in $graphics.notices) {
            $source = Assert-ReleaseChild $repo (Join-Path $repo ('.tools/graphics/src/skia/' + $entry.path))
            Assert-ReleaseHash $source $entry.sha256
            $target = Assert-ReleaseChild $staging (Join-Path $staging ('licenses/graphics/' + $entry.path))
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
            [IO.File]::Copy($source, $target)
        }
        foreach ($grammar in $grammars.packages) {
            $archive = Get-ReleaseArchive $grammar @((Join-Path $repo ('.tools/grammars/' + $grammar.name + '.zip'))) -Offline:$Offline
            $target = Join-Path $staging ('licenses/grammars/' + $grammar.name)
            Expand-ReleaseZip $archive $target ($grammar.directory + '/') @('LICENSE')
            Assert-ReleaseHash (Join-Path $target 'LICENSE') $grammar.files.LICENSE
        }
        foreach ($file in $lockFiles) {
            Assert-ReleaseHash (Join-Path $repo $file) $lockHashes[$file]
            $target = Join-Path $staging ('provenance/' + $file)
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
            [IO.File]::Copy((Join-Path $repo $file), $target)
            Assert-ReleaseHash $target $lockHashes[$file]
        }
        [IO.File]::Copy((Join-Path $repo 'languages/manifest.json'), (Join-Path $tools 'language-manifest.json'))
        [IO.File]::Copy((Join-Path $repo 'release/THIRD-PARTY.md'), (Join-Path $staging 'THIRD-PARTY.md'))
        Write-ReleaseInventory $staging 'runtime-manifest.json' ([ordered]@{schema=1;platform='windows';architecture='x64';identity=$identity;locks=$lockHashes})
        [void](Assert-ReleaseInventory $staging 'runtime-manifest.json')
        Move-Item -LiteralPath $staging -Destination $destination
    } finally {
        $checked = Assert-ReleaseChild $runtimeRoot $staging
        if (Test-Path -LiteralPath $checked) { Remove-Item -LiteralPath $checked -Recurse -Force }
    }
}
$pointer = [ordered]@{schema=1;directory=[IO.Path]::GetRelativePath($repo,$destination).Replace('\','/');manifestSha256=Get-ReleaseHash (Join-Path $destination 'runtime-manifest.json')}
$pointer | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runtimeRoot 'windows-x64.json') -Encoding utf8NoBOM
Write-Output "Verified Windows runtime: $destination"
