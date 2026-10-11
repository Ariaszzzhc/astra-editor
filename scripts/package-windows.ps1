[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/cmake/windows-package',
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
$offlineOption = if ($Offline) { 'ON' } else { 'OFF' }
if ($BuildDirectory -eq 'build/cmake/windows-package') {
    & "$PSScriptRoot/cmake.ps1" --preset windows-package "-DASTRA_OFFLINE=$offlineOption"
}
$cache = Get-Content (Join-Path $build 'CMakeCache.txt') -Raw
if ($cache -notmatch '(?m)^CMAKE_BUILD_TYPE:STRING=Release\r?$') { throw 'Packaging requires a configured Release build.' }
if ($cache -notmatch '(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)\r?$' -or
    [IO.Path]::GetFullPath($Matches[1].Trim()) -ne $repo) { throw 'Build belongs to another source checkout.' }
if ($cache -notmatch '(?m)^CMAKE_GENERATOR:INTERNAL=Ninja\r?$' -or
    $cache -notmatch '(?m)^VCPKG_TARGET_TRIPLET:STRING=x64-windows-astra\r?$') {
    throw 'Use the pinned Ninja toolchain and x64-windows-astra triplet.'
}
if ($BuildDirectory -ne 'build/cmake/windows-package') {
    & "$PSScriptRoot/cmake.ps1" -S $repo -B $build "-DASTRA_OFFLINE=$offlineOption"
}
& "$PSScriptRoot/cmake.ps1" --build $build --target astra astra-server astra-process-host astra-clipboard-host --parallel 8
$version = (Get-Content (Join-Path $build 'astra-version.txt') -Raw).Trim()
$name = 'astra-' + $version + '-windows-x64'
if (-not $OutputDirectory) { $OutputDirectory = 'build/packages/' + $name }
$output = Assert-ReleaseChild (Join-Path $repo 'build') $OutputDirectory
if ((Test-Path -LiteralPath $output) -or (Test-Path -LiteralPath ($output + '.zip'))) {
    throw "Output already exists; choose a new OutputDirectory: $output"
}
$staging = Assert-ReleaseChild (Join-Path $repo 'build') ($output + '.staging-' + [Guid]::NewGuid().ToString('N'))
try {
    & "$PSScriptRoot/cmake.ps1" --install $build --prefix $staging --component Runtime
    $commit = (& git -C $repo rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Could not determine source revision' }
    $changes = @(& git -C $repo status --porcelain --untracked-files=normal)
    if ($LASTEXITCODE -ne 0) { throw 'Could not determine source status' }
    $files = @(& git -C $repo ls-files --cached --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) { throw 'Could not enumerate source files' }
    $sourceHashes = [ordered]@{}
    foreach ($file in $files) {
        if (Test-Path -LiteralPath (Join-Path $repo $file) -PathType Leaf) {
            $sourceHashes[$file] = Get-ReleaseHash (Join-Path $repo $file)
        }
    }
    $sourceHashes | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $staging 'provenance/source-files.json') -Encoding utf8NoBOM
    $vcpkgCommit = (& git -C (Join-Path $repo 'third_party/vcpkg') rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Could not determine vcpkg revision' }
    Write-ReleaseInventory $staging 'package-manifest.json' ([ordered]@{
        schema=1;product='Astra';version=$version;platform='windows';architecture='x64';configuration='Release';
        sourceCommit=$commit;sourceDirty=($changes.Count -ne 0);vcpkgCommit=$vcpkgCommit;crt='MD'
    })
    & "$PSScriptRoot/verify-windows-package.ps1" -PackageDirectory $staging
    Move-Item -LiteralPath $staging -Destination $output
    if (-not $NoArchive) {
        # Archive the verified installation, including its inventory. CPack does
        # not run a second install or choose a separate list of payload files.
        $packArguments = @('--config', (Join-Path $build 'CPackConfig.cmake'),
            '-D', 'CPACK_INSTALL_CMAKE_PROJECTS=', '-D', "CPACK_INSTALLED_DIRECTORIES=$output;/",
            '-D', "CPACK_PACKAGE_DIRECTORY=$([IO.Path]::GetDirectoryName($output))",
            '-D', "CPACK_PACKAGE_FILE_NAME=$([IO.Path]::GetFileName($output))")
        & "$PSScriptRoot/cmake.ps1" -CPack @packArguments
        $hash = Get-ReleaseHash ($output + '.zip')
        [IO.File]::WriteAllText($output + '.zip.sha256', $hash + '  ' + [IO.Path]::GetFileName($output) + ".zip`n")
        Write-Output "Archive SHA-256: $hash"
    }
    Write-Output "Windows package: $output"
} finally {
    $checked = Assert-ReleaseChild (Join-Path $repo 'build') $staging
    if (Test-Path -LiteralPath $checked) { Remove-Item -LiteralPath $checked -Recurse -Force }
}
