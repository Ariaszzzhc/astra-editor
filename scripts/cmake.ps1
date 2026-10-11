[CmdletBinding()]
param(
    [switch]$CTest,
    [switch]$CPack,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$CMakeArguments
)
$ErrorActionPreference = 'Stop'
if (-not $IsWindows) { throw 'Use native CMake/Ninja presets on other hosts.' }
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $PSScriptRoot 'release-common.ps1')
$toolManifest = Get-Content (Join-Path $repository 'dependencies/tools.json') -Raw | ConvertFrom-Json
foreach ($toolName in @('cmake','ninja')) {
    $asset = $toolManifest.$toolName
    $archiveName = ([Uri]$asset.archive).Segments[-1]
    $archive = Get-ReleaseArchive $asset @((Join-Path $repository ('.tools/downloads/' + $archiveName)))
    $destination = Join-Path $repository ('.tools/' + $toolName)
    $executable = Join-Path $destination $asset.executable
    if (-not (Test-Path -LiteralPath $executable)) {
        Expand-ReleaseZip $archive $destination
    }
    $reported = & $executable --version
    if ($LASTEXITCODE -ne 0 -or ($reported -join ' ') -notmatch [regex]::Escape($asset.version)) {
        throw "Unexpected pinned $toolName version"
    }
}
$vcpkgRoot = Join-Path $repository 'third_party/vcpkg'
$actualCommit = & git -C $vcpkgRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $actualCommit -cne $toolManifest.vcpkg.commit) {
    throw 'Initialize the pinned submodule: git submodule update --init --recursive'
}
if (-not (Test-Path -LiteralPath (Join-Path $vcpkgRoot 'vcpkg.exe'))) {
    & (Join-Path $vcpkgRoot 'bootstrap-vcpkg.bat') -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw 'vcpkg bootstrap failed' }
}
$savedEnvironment = @{}
foreach ($name in @('PATH','ASTRA_SOURCE_ROOT','VCPKG_DOWNLOADS','VCPKG_BINARY_SOURCES','VCPKG_DISABLE_METRICS','VCPKG_FORCE_SYSTEM_BINARIES','VCPKG_FORCE_DOWNLOADED_BINARIES','TEMP','TMP')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    if (-not $env:VCINSTALLDIR) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -ne 0 -or -not $visualStudio) { throw 'Visual Studio C++ tools are required' }
        $devcmd = Join-Path $visualStudio 'Common7/Tools/VsDevCmd.bat'
        if ($devcmd.Contains('"')) { throw 'Unsupported Visual Studio path' }
        $lines = & $env:COMSPEC /d /c "call `"$devcmd`" -no_logo -arch=x64 -host_arch=x64 && set"
        if ($LASTEXITCODE -ne 0) { throw 'Visual Studio environment initialization failed' }
        foreach ($line in $lines) {
            if ($line -match '^([^=]+)=(.*)$') {
                $name = $Matches[1]
                if (-not $savedEnvironment.ContainsKey($name)) {
                    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
                }
                [Environment]::SetEnvironmentVariable($name, $Matches[2], 'Process')
            }
        }
    }
    $cmake = Join-Path $repository ('.tools/cmake/' + $toolManifest.cmake.executable)
    $command = $cmake
    if ($CTest -and $CPack) { throw 'Choose CTest or CPack, not both.' }
    if ($CTest) { $command = Join-Path (Split-Path $cmake) 'ctest.exe' }
    if ($CPack) { $command = Join-Path (Split-Path $cmake) 'cpack.exe' }
    $env:PATH = (Split-Path $cmake) + ';' + (Join-Path $repository '.tools/ninja') + ';' + $env:PATH
    $env:ASTRA_SOURCE_ROOT = $repository
    $env:VCPKG_DOWNLOADS = Join-Path $repository '.tools/vcpkg/downloads'
    $binaryCache = Join-Path $repository '.tools/vcpkg/binary-cache'
    $temporary = Join-Path $repository '.tools/vcpkg/tmp'
    foreach ($path in @($env:VCPKG_DOWNLOADS, $binaryCache, $temporary)) {
        [void][IO.Directory]::CreateDirectory($path)
    }
    $env:TEMP = $temporary
    $env:TMP = $temporary
    $env:VCPKG_BINARY_SOURCES = 'clear;files,' + $binaryCache + ',readwrite'
    $env:VCPKG_DISABLE_METRICS = '1'
    Remove-Item Env:VCPKG_FORCE_SYSTEM_BINARIES -ErrorAction SilentlyContinue
    $env:VCPKG_FORCE_DOWNLOADED_BINARIES = '1'
    Push-Location $repository
    try {
        & $command @CMakeArguments
        if ($LASTEXITCODE -ne 0) { throw "CMake failed with exit code $LASTEXITCODE" }
    } finally { Pop-Location }
} finally {
    foreach ($entry in $savedEnvironment.GetEnumerator()) {
        if ($null -eq $entry.Value) {
            Remove-Item -LiteralPath ('Env:' + $entry.Key) -ErrorAction SilentlyContinue
        } else {
            [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
        }
    }
}
