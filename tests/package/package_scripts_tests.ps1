$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../../scripts/release-common.ps1')
if (-not $IsWindows) { throw 'Run this packaging test on Windows only.' }
$fixture = Assert-ReleaseChild (Join-Path $ReleaseRepository 'build') (Join-Path $ReleaseRepository ('build/package-helpers-' + [Guid]::NewGuid().ToString('N')))
[void][IO.Directory]::CreateDirectory($fixture)
$checks = 0
$defaultCache = $null
function Expect-Rejected([scriptblock]$Action, [string]$Pattern) {
    try { & $Action } catch {
        if ($_.Exception.Message -notmatch $Pattern) { throw }
        $script:checks++
        return
    }
    throw "Expected rejection: $Pattern"
}
try {
    $iconPayload = Join-Path $fixture 'icon-payload'
    $iconFiles = @('resources/icons/material/python.svg','resources/icons/associations.json')
    foreach ($relative in $iconFiles) {
        $target = Get-ArchiveTarget $iconPayload $relative
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
        Copy-Item -LiteralPath (Join-Path $ReleaseRepository $relative) -Destination $target
    }
    $noticePath = 'licenses/icons/material/LICENSE'
    $noticeTarget = Get-ArchiveTarget $iconPayload $noticePath
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($noticeTarget))
    Copy-Item -LiteralPath (Join-Path $ReleaseRepository 'dependencies/icons/licenses/material/LICENSE') -Destination $noticeTarget
    $iconReceipt = [ordered]@{
        schema=1
        assets=@([ordered]@{id='material/python';path=$iconFiles[0];sha256=(Get-ReleaseHash (Join-Path $iconPayload $iconFiles[0]))})
        associations=[ordered]@{path=$iconFiles[1];sha256=(Get-ReleaseHash (Join-Path $iconPayload $iconFiles[1]))}
        notices=@([ordered]@{path='dependencies/icons/licenses/material/LICENSE';sha256=(Get-ReleaseHash $noticeTarget)})
    }
    [void][IO.Directory]::CreateDirectory((Join-Path $iconPayload 'provenance'))
    $iconReceipt | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $iconPayload 'provenance/icons.json') -Encoding utf8
    if ((Assert-ReleaseIcons $iconPayload) -ne 1) { throw 'Installed icon receipt was not verified' }
    $checks++
    [IO.File]::AppendAllText((Join-Path $iconPayload $iconFiles[0]), 'changed')
    Expect-Rejected { Assert-ReleaseIcons $iconPayload } 'SHA-256 mismatch'
    Copy-Item -LiteralPath (Join-Path $ReleaseRepository $iconFiles[0]) -Destination (Join-Path $iconPayload $iconFiles[0]) -Force
    [IO.File]::AppendAllText($noticeTarget, 'changed')
    Expect-Rejected { Assert-ReleaseIcons $iconPayload } 'SHA-256 mismatch'
    $payload = Join-Path $fixture 'payload'
    [void][IO.Directory]::CreateDirectory($payload)
    [IO.File]::WriteAllText((Join-Path $payload 'unicode.txt'), '中文 😀')
    Write-ReleaseInventory $payload 'inventory.json' ([ordered]@{schema=1})
    [void](Assert-ReleaseInventory $payload 'inventory.json')
    $checks++
    [IO.File]::WriteAllText((Join-Path $payload 'extra.txt'), 'unexpected')
    Expect-Rejected { Assert-ReleaseInventory $payload 'inventory.json' } 'Unmanifested'
    Remove-Item -LiteralPath (Join-Path $payload 'extra.txt')
    [IO.File]::WriteAllText((Join-Path $payload 'unicode.txt'), 'mutated')
    Expect-Rejected { Assert-ReleaseInventory $payload 'inventory.json' } 'mismatch'
    Expect-Rejected { Assert-ReleaseChild $fixture (Join-Path $fixture '../outside') } 'escapes'
    foreach ($entryName in @('../outside.txt','C:/outside.txt','folder\outside.txt')) {
        $zipPath = Join-Path $fixture ([Guid]::NewGuid().ToString('N') + '.zip')
        $zip = [IO.Compression.ZipFile]::Open($zipPath,[IO.Compression.ZipArchiveMode]::Create)
        try {
            $entry = $zip.CreateEntry($entryName)
            $writer = [IO.StreamWriter]::new($entry.Open())
            try { $writer.Write('escape') } finally { $writer.Dispose() }
        } finally { $zip.Dispose() }
        $destination = Join-Path $fixture ([Guid]::NewGuid().ToString('N'))
        Expect-Rejected { Expand-ReleaseZip $zipPath $destination } 'Unsafe archive'
    }
    $zipPath = Join-Path $fixture 'normal.zip'
    $zip = [IO.Compression.ZipFile]::Open($zipPath,[IO.Compression.ZipArchiveMode]::Create)
    try {
        $entry = $zip.CreateEntry('root/ok.txt')
        $writer = [IO.StreamWriter]::new($entry.Open())
        try { $writer.Write('verified archive') } finally { $writer.Dispose() }
    } finally { $zip.Dispose() }
    Expand-ReleaseZip $zipPath (Join-Path $fixture 'normal') 'root/' @('ok.txt')
    if ([IO.File]::ReadAllText((Join-Path $fixture 'normal/ok.txt')) -cne 'verified archive') { throw 'ZIP prefix extraction changed bytes' }
    $checks++
    Expect-Rejected { Get-ReleaseArchive ([pscustomobject]@{archive='https://example.invalid/test.zip';sha256=('0'*64)}) @($zipPath) -Offline } 'SHA-256 mismatch'
    # An online preparation stores archives in this default cache, even when
    # none of the caller's historical development-cache candidates exist.
    $defaultName = 'release-cache-test-' + [Guid]::NewGuid().ToString('N') + '.zip'
    $defaultCache = Assert-ReleaseChild (Join-Path $ReleaseRepository '.tools') (Join-Path $ReleaseRepository ('.tools/release-downloads/' + $defaultName))
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($defaultCache))
    [IO.File]::Copy($zipPath, $defaultCache)
    $asset = [pscustomobject]@{archive=('https://example.invalid/' + $defaultName);sha256=(Get-ReleaseHash $zipPath)}
    $offlinePath = Get-ReleaseArchive $asset @((Join-Path $fixture 'missing-caller-cache.zip')) -Offline
    if ($offlinePath -cne $defaultCache) { throw 'Offline preparation did not reuse its own verified cache' }
    $checks++
    [IO.File]::WriteAllText($defaultCache, 'corrupt archive')
    Expect-Rejected { Get-ReleaseArchive $asset @() -Offline } 'SHA-256 mismatch'
    # Exercise the production preparation entry point in an isolated checkout:
    # a modified local dependency patch must fail before any downloads/staging.
    $patchRoot = Join-Path $fixture 'patch-guard'
    $inputs = @('scripts/prepare-windows-runtime.ps1', 'scripts/release-common.ps1',
        'dependencies/runtime/manifest.json', 'packaging/windows-tools.json',
        'dependencies/grammars/manifest.json', 'dependencies/patches.json',
        'dependencies/ports/astra-quickjs/host-async-context.patch',
        'dependencies/ports/skia/glyph-cache.patch')
    foreach ($inputFile in $inputs) {
        $target = Join-Path $patchRoot $inputFile
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
        [IO.File]::Copy((Join-Path $ReleaseRepository $inputFile), $target)
    }
    [IO.File]::AppendAllText((Join-Path $patchRoot 'dependencies/ports/astra-quickjs/host-async-context.patch'), 'modified')
    Expect-Rejected { & (Join-Path $patchRoot 'scripts/prepare-windows-runtime.ps1') -Offline } 'Dependency patch hash mismatch'
    Write-Output "Windows package helper invariants: PASS ($checks checks)"
} finally {
    if ($defaultCache -and (Test-Path -LiteralPath $defaultCache)) { Remove-Item -LiteralPath $defaultCache }
    $checked = Assert-ReleaseChild (Join-Path $ReleaseRepository 'build') $fixture
    Remove-Item -LiteralPath $checked -Recurse -Force
}
