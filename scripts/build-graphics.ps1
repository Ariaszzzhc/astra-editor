#Requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Bootstrap', 'Configure', 'Build')]
    [string] $Stage = 'Build',
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [ValidateRange(1, 128)]
    [int] $Jobs = [Math]::Max(1, [Math]::Min(8, [Environment]::ProcessorCount)),
    [string] $VisualStudioPath = '',
    [switch] $Resync
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $IsWindows -or [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64') {
    throw 'This bootstrap supports a native Windows x64 build only.'
}
$Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$Tools = Join-Path $Root '.tools/graphics'
$Downloads = Join-Path $Root '.tools/downloads'
$Source = Join-Path $Tools 'src/skia'
$Output = Join-Path $Root "build/graphics/x64/$($Configuration.ToLowerInvariant())"
$Install = Join-Path $Tools "install/x64/$($Configuration.ToLowerInvariant())"
$SkiaRevision = '525f94ac3418fd0b0dae42caa220422a94b18b49'
$SkiaTree = '4924cfdc267cd1fcdd7ea63981a0fcb0e10df010'
$DawnRevision = 'ab3c12305e776fa707503f7ecfab07364e9c7697'
$GnRevision = 'b2afae122eeb6ce09c52d63f67dc53fc517dbdc8'

function Invoke-Native {
    param([string] $Executable, [string[]] $Arguments)
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Executable exited with code $LASTEXITCODE."
    }
}

function Get-CheckedArchive {
    param([string] $Name, [string] $Uri, [string] $Sha256)
    $destination = Join-Path $Downloads $Name
    if (-not (Test-Path -LiteralPath $destination)) {
        $partial = "$destination.partial"
        Write-Host "Downloading $Name"
        Invoke-WebRequest -Uri $Uri -OutFile $partial -MaximumRetryCount 3 -RetryIntervalSec 2
        if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash -ne $Sha256) {
            throw "SHA256 mismatch for $Name; the partial file is retained for diagnosis."
        }
        Move-Item -LiteralPath $partial -Destination $destination -Force
    }
    if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $Sha256) {
        throw "SHA256 mismatch for cached $Name. Remove the invalid archive and retry."
    }
    return $destination
}

function Expand-CheckedTool {
    param([string] $Archive, [string] $Directory, [string] $Executable)
    $expected = Join-Path $Directory $Executable
    if (-not (Test-Path -LiteralPath $expected)) {
        New-Item -ItemType Directory -Force -Path $Directory | Out-Null
        Expand-Archive -LiteralPath $Archive -DestinationPath $Directory -Force
    }
    if (-not (Test-Path -LiteralPath $expected)) { throw "Archive did not contain $Executable." }
    return $expected
}

function ConvertTo-GnString {
    param([string] $Value)
    if ($Value.Contains('"') -or $Value.Contains('$') -or $Value.Contains("`n")) {
        throw 'Unsupported character in a GN path.'
    }
    return '"' + $Value.Replace('\', '/') + '"'
}

foreach ($directory in @($Tools, $Downloads, $Output, (Join-Path $Tools 'tmp'))) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}
$oldEnvironment = @{}
foreach ($name in @('PATH', 'TEMP', 'TMP', 'PYTHONDONTWRITEBYTECODE', 'GIT_CONFIG_COUNT',
                    'GIT_CONFIG_KEY_0', 'GIT_CONFIG_VALUE_0')) {
    $oldEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
Push-Location $Root
try {
    $env:TEMP = Join-Path $Tools 'tmp'
    $env:TMP = $env:TEMP
    $env:PYTHONDONTWRITEBYTECODE = '1'
    # Long dependency paths are common; this is process-local and changes no Git config file.
    $env:GIT_CONFIG_COUNT = '1'
    $env:GIT_CONFIG_KEY_0 = 'core.longpaths'
    $env:GIT_CONFIG_VALUE_0 = 'true'
    $Git = (Get-Command git -ErrorAction Stop).Source

    $archive = Get-CheckedArchive 'python-3.13.7-embed-amd64.zip' `
        'https://www.python.org/ftp/python/3.13.7/python-3.13.7-embed-amd64.zip' `
        'f6cca216a359be84797cabb54149ce5e062afb16cc7567eb7fc51cacb2d86b65'
    $Python = Expand-CheckedTool $archive (Join-Path $Tools 'python') 'python.exe'
    # Some upstream GN actions invoke python3 by name instead of using GN's
    # --script-executable. Both names must resolve to this same pinned runtime.
    Copy-Item -LiteralPath $Python -Destination (Join-Path $Tools 'python/python3.exe') -Force
    # Dawn's generators import sibling scripts and DEPS-provided Python modules. The
    # embeddable distribution's isolated-path override would hide both of those.
    $isolatedPath = Join-Path $Tools 'python/python313._pth'
    if (Test-Path -LiteralPath $isolatedPath) {
        Move-Item -LiteralPath $isolatedPath -Destination "$isolatedPath.disabled" -Force
    }
    $archive = Get-CheckedArchive 'cmake-3.31.8-windows-x86_64.zip' `
        'https://github.com/Kitware/CMake/releases/download/v3.31.8/cmake-3.31.8-windows-x86_64.zip' `
        '81aa9964dbabd71fe02e7ec50472fd3ad56138c49944515ece9001efbff8d719'
    $Cmake = Expand-CheckedTool $archive (Join-Path $Tools 'cmake') 'cmake-3.31.8-windows-x86_64/bin/cmake.exe'
    $archive = Get-CheckedArchive 'ninja-1.13.1-win.zip' `
        'https://github.com/ninja-build/ninja/releases/download/v1.13.1/ninja-win.zip' `
        '26a40fa8595694dec2fad4911e62d29e10525d2133c9a4230b66397774ae25bf'
    $Ninja = Expand-CheckedTool $archive (Join-Path $Tools 'ninja') 'ninja.exe'
    $archive = Get-CheckedArchive "gn-$GnRevision-windows-amd64.zip" `
        "https://chrome-infra-packages.appspot.com/dl/gn/gn/windows-amd64/+/git_revision:$GnRevision" `
        '7249f55d6f22b55affa68aaf19d2f597c39946299216e12d78eac6cc1469f8a1'
    $Gn = Expand-CheckedTool $archive (Join-Path $Tools 'gn') 'gn.exe'
    $archive = Get-CheckedArchive 'dxc_2026_09_29.zip' `
        'https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip' `
        'ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1'
    $Dxc = Expand-CheckedTool $archive (Join-Path $Tools 'dxc') 'bin/x64/dxc.exe'
    $env:PATH = ((@($Python, $Cmake, $Ninja, $Gn) | ForEach-Object { Split-Path $_ }) -join ';') + ';' + $env:PATH
    Invoke-Native $Python @('--version')
    Invoke-Native $Cmake @('--version')
    Invoke-Native $Ninja @('--version')
    Invoke-Native $Gn @('--version')

    if (-not (Test-Path -LiteralPath (Join-Path $Source '.git'))) {
        if (Test-Path -LiteralPath $Source) { throw "Unmanaged source directory exists: $Source" }
        New-Item -ItemType Directory -Path $Source -Force | Out-Null
        Invoke-Native $Git @('init', '--quiet', $Source)
        Invoke-Native $Git @('-C', $Source, 'remote', 'add', 'origin', 'https://skia.googlesource.com/skia.git')
    }
    $head = & $Git -C $Source rev-parse --verify HEAD 2>$null
    if ($LASTEXITCODE -ne 0) {
        # Resume an initialization interrupted during the first network fetch.
        Invoke-Native $Git @('-C', $Source, 'fetch', '--depth=1', 'origin', $SkiaRevision)
        Invoke-Native $Git @('-C', $Source, 'checkout', '--quiet', '--detach', $SkiaRevision)
    }
    $head = & $Git -C $Source rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $head -ne $SkiaRevision) {
        throw "Skia checkout must be at pinned revision $SkiaRevision."
    }
    $tree = & $Git -C $Source rev-parse 'HEAD^{tree}'
    if ($LASTEXITCODE -ne 0 -or $tree -ne $SkiaTree) { throw 'Pinned Skia tree hash mismatch.' }
    $dirty = & $Git -C $Source status --porcelain --untracked-files=no
    if ($LASTEXITCODE -ne 0 -or $dirty) { throw 'Pinned Skia source has tracked modifications.' }
    $depsText = Get-Content -LiteralPath (Join-Path $Source 'DEPS') -Raw
    if (-not $depsText.Contains("dawn.git@$DawnRevision")) { throw 'Unexpected Dawn version in Skia DEPS.' }
    if ($Stage -eq 'Bootstrap') {
        Write-Host "Graphics tools and pinned Skia source verified: $Source"
        return
    }

    $depsMarker = Join-Path $Tools 'dependencies-verified.json'
    Push-Location $Source
    try {
        if ($Resync -or -not (Test-Path -LiteralPath $depsMarker)) {
            Write-Host 'Synchronizing the pinned Skia DEPS; the first run downloads several GB.'
            # Invoke the unmodified upstream sync function without its GN/emsdk bootstrap.
            # Bound its worker pool: launching every repository at once can trigger
            # upstream HTTP 429 responses. Transient failures get bounded backoff.
            # GN is already hash-verified above; this native build does not use emsdk.
            $syncDeps = @'
import concurrent.futures, runpy, subprocess, time
m = runpy.run_path('tools/git-sync-deps')
def bounded_sync(function, arguments):
    def run(args):
        for attempt in range(4):
            try:
                return function(*args)
            except subprocess.CalledProcessError:
                if attempt == 3:
                    raise
                time.sleep(3 * (2 ** attempt))
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(run, arguments))
sync = m['git_sync_deps']
sync.__globals__['multithread'] = bounded_sync
sync(m['DEFAULT_DEPS_PATH'], [], True, True)
'@
            Invoke-Native $Python @('-u', '-c', $syncDeps)
        }
        $verifyDeps = @'
import json, pathlib, runpy, subprocess, sys
m = runpy.run_path('tools/git-sync-deps')
deps = m['parse_file_to_dict']('DEPS')['deps']
locked = {}
for relative, spec in deps.items():
    if not isinstance(spec, str):
        continue
    url, expected = spec.rsplit('@', 1)
    directory = pathlib.Path(relative)
    # DEPS can pin an annotated tag object (for example Perfetto). Verify its
    # immutable object identity, then compare HEAD with the peeled commit.
    pinned_commit = subprocess.check_output(['git', '-C', str(directory), 'rev-parse', expected + '^{commit}'], text=True).strip()
    actual = subprocess.check_output(['git', '-C', str(directory), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != pinned_commit:
        raise RuntimeError(f'{relative}: expected {pinned_commit}, found {actual}')
    changes = subprocess.check_output(['git', '-C', str(directory), 'status', '--porcelain', '--untracked-files=no'], text=True)
    if changes:
        raise RuntimeError(f'{relative}: tracked dependency files were modified')
    locked[relative] = {'url': url, 'revision': expected, 'commit': pinned_commit}
pathlib.Path(sys.argv[1]).write_text(json.dumps(locked, indent=2), encoding='utf-8')
print(f'Verified {len(locked)} pinned dependency checkouts.')
'@
        Invoke-Native $Python @('-u', '-c', $verifyDeps, $depsMarker)
    } finally { Pop-Location }

    if (-not $env:VCINSTALLDIR -or -not $env:WindowsSdkDir) {
        if (-not $VisualStudioPath) {
            $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
            if (-not (Test-Path -LiteralPath $vswhere)) {
                throw 'Run from a Visual Studio Developer PowerShell or pass -VisualStudioPath.'
            }
            $VisualStudioPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($LASTEXITCODE -ne 0 -or -not $VisualStudioPath) { throw 'No Windows C++ toolchain was found.' }
        }
        $devcmd = Join-Path $VisualStudioPath 'Common7/Tools/VsDevCmd.bat'
        if ($devcmd.Contains('"')) { throw 'Invalid Visual Studio path.' }
        $environmentLines = & $env:COMSPEC /d /c "call `"$devcmd`" -no_logo -arch=x64 -host_arch=x64 && set"
        if ($LASTEXITCODE -ne 0) { throw 'Visual Studio environment initialization failed.' }
        foreach ($line in $environmentLines) {
            if ($line -match '^([^=]+)=(.*)$') {
                if (-not $oldEnvironment.ContainsKey($Matches[1])) {
                    $oldEnvironment[$Matches[1]] = [Environment]::GetEnvironmentVariable($Matches[1], 'Process')
                }
                [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
            }
        }
        $env:PATH = ((@($Python, $Cmake, $Ninja, $Gn) | ForEach-Object { Split-Path $_ }) -join ';') + ';' + $env:PATH
    }
    foreach ($variable in @('VCINSTALLDIR', 'VCToolsVersion', 'WindowsSdkDir', 'WindowsSDKVersion')) {
        if (-not [Environment]::GetEnvironmentVariable($variable, 'Process')) {
            throw "Windows toolchain did not define $variable."
        }
    }
    $debug = ($Configuration -eq 'Debug').ToString().ToLowerInvariant()
    $gnArgs = @(
        'target_os="win"', 'target_cpu="x64"', "is_debug=$debug", 'is_official_build=false',
        'is_component_build=false', 'is_trivial_abi=false',
        ('win_vc=' + (ConvertTo-GnString $env:VCINSTALLDIR.TrimEnd('\'))),
        ('win_toolchain_version=' + (ConvertTo-GnString $env:VCToolsVersion.TrimEnd('\'))),
        ('win_sdk=' + (ConvertTo-GnString $env:WindowsSdkDir.TrimEnd('\'))),
        ('win_sdk_version=' + (ConvertTo-GnString $env:WindowsSDKVersion.TrimEnd('\'))),
        'skia_enable_graphite=true', 'skia_use_dawn=true', 'skia_enable_ganesh=false',
        'dawn_enable_d3d12=true', 'dawn_enable_d3d11=false', 'dawn_enable_vulkan=false',
        'dawn_enable_opengles=false', 'dawn_enable_metal=false', 'dawn_use_agility_sdk=false',
        'skia_enable_tools=false', 'skia_enable_android_utils=false', 'skia_use_gl=false', 'skia_use_direct3d=false',
        'skia_enable_pdf=false', 'skia_enable_skottie=false', 'skia_enable_svg=false',
        'skia_enable_fontmgr_win=true', 'skia_use_icu=true', 'skia_use_system_icu=false',
        'skia_use_harfbuzz=true', 'skia_use_system_harfbuzz=false',
        'skia_use_system_libjpeg_turbo=false', 'skia_use_system_libpng=false',
        'skia_use_system_libwebp=false', 'skia_use_system_zlib=false', 'skia_use_system_expat=false',
        'skia_use_partition_alloc=false', 'skia_enable_vello_shaders=false',
        'skia_use_fontations=false', 'skia_build_rust_targets=false',
        'extra_cflags=["/MT", "/D_ITERATOR_DEBUG_LEVEL=0"]'
    )
    $argsFile = Join-Path $Output 'args.gn'
    $gnArgs | Set-Content -LiteralPath $argsFile -Encoding utf8NoBOM
    Push-Location $Source
    try {
        Invoke-Native $Gn @('gen', $Output, "--script-executable=$Python", '--fail-on-unused-args')
        if ($Stage -eq 'Configure') {
            Write-Host "Graphics build configured: $Output"
            return
        }
        Write-Host "Building the real Graphite/Dawn and text libraries with $Jobs parallel jobs."
        # The upstream Dawn script prints its environment. Build with an explicit
        # compiler environment so unrelated application credentials are never logged.
        $buildEnvironment = @{}
        $allowedBuildVariables = @('PATH', 'TEMP', 'TMP', 'SystemRoot', 'SystemDrive', 'COMSPEC',
            'PATHEXT', 'windir', 'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE', 'ProgramFiles',
            'ProgramFiles(x86)', 'ProgramW6432', 'APPDATA', 'LOCALAPPDATA', 'USERPROFILE',
            'HOMEDRIVE', 'HOMEPATH', 'INCLUDE', 'LIB', 'LIBPATH', 'VCINSTALLDIR', 'VCToolsVersion',
            'VCToolsInstallDir', 'VSINSTALLDIR', 'VisualStudioVersion', 'WindowsSdkDir',
            'WindowsSDKVersion', 'WindowsSDKLibVersion', 'WindowsLibPath', 'UCRTVersion',
            'UniversalCRTSdkDir', 'PYTHONDONTWRITEBYTECODE')
        try {
            foreach ($entry in [Environment]::GetEnvironmentVariables('Process').GetEnumerator()) {
                if ($entry.Key -notin $allowedBuildVariables) {
                    $buildEnvironment[$entry.Key] = $entry.Value
                    [Environment]::SetEnvironmentVariable($entry.Key, $null, 'Process')
                }
            }
            Invoke-Native $Ninja @('-C', $Output, '-j', "$Jobs", 'skia', 'skparagraph', 'skshaper', 'skunicode_core', 'skunicode_icu')
        } finally {
            foreach ($entry in $buildEnvironment.GetEnumerator()) {
                [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
            }
        }
    } finally { Pop-Location }

    foreach ($directory in @('lib', 'bin', 'licenses', 'include/skia', 'include/skia/modules', 'include/dawn')) {
        New-Item -ItemType Directory -Force -Path (Join-Path $Install $directory) | Out-Null
    }
    Copy-Item -LiteralPath (Join-Path $Source 'include') -Destination (Join-Path $Install 'include/skia') -Recurse -Force
    foreach ($module in @('skparagraph', 'skshaper', 'skunicode')) {
        $destination = Join-Path $Install "include/skia/modules/$module"
        New-Item -ItemType Directory -Force -Path $destination | Out-Null
        Copy-Item -LiteralPath (Join-Path $Source "modules/$module/include") -Destination $destination -Recurse -Force
    }
    # These headers are referenced by Skia's public color-space and Unicode APIs.
    foreach ($directory in @('include/skia/modules/skcms/src', 'include/skia/src/core')) {
        New-Item -ItemType Directory -Force -Path (Join-Path $Install $directory) | Out-Null
    }
    Copy-Item -LiteralPath (Join-Path $Source 'modules/skcms/skcms.h') -Destination (Join-Path $Install 'include/skia/modules/skcms') -Force
    Copy-Item -Path (Join-Path $Source 'modules/skcms/src/*.h') -Destination (Join-Path $Install 'include/skia/modules/skcms/src') -Force
    Copy-Item -LiteralPath (Join-Path $Source 'src/core/SkUTF.h') -Destination (Join-Path $Install 'include/skia/src/core') -Force
    Copy-Item -Path (Join-Path $Source 'third_party/externals/dawn/include/*') -Destination (Join-Path $Install 'include/dawn') -Recurse -Force
    Copy-Item -Path (Join-Path $Output 'gen/third_party/dawn/include/*') -Destination (Join-Path $Install 'include/dawn') -Recurse -Force
    $libraries = @(Get-ChildItem -LiteralPath $Output -Filter '*.lib' -File)
    if (-not ($libraries.Name -contains 'skia.lib') -or -not ($libraries.Name -contains 'dawn_combined.lib')) {
        throw 'The graphics build did not produce both skia.lib and dawn_combined.lib.'
    }
    foreach ($library in $libraries) {
        Copy-Item -LiteralPath $library.FullName -Destination (Join-Path $Install 'lib') -Force
    }
    Copy-Item -LiteralPath (Join-Path $Output 'icudtl.dat') -Destination (Join-Path $Install 'bin') -Force
    foreach ($dll in @('dxcompiler.dll', 'dxil.dll')) {
        Copy-Item -LiteralPath (Join-Path (Split-Path $Dxc) $dll) -Destination (Join-Path $Install 'bin') -Force
    }
    Copy-Item -LiteralPath (Join-Path $Source 'LICENSE') -Destination (Join-Path $Install 'licenses/Skia-LICENSE.txt') -Force
    Copy-Item -LiteralPath (Join-Path $Source 'third_party/externals/dawn/LICENSE') -Destination (Join-Path $Install 'licenses/Dawn-LICENSE.txt') -Force
    Get-ChildItem -LiteralPath (Join-Path $Tools 'dxc') -Filter 'LICENSE*' -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $Install "licenses/DXC-$($_.Name)") -Force
    }
    $runtimeFiles = @('bin/icudtl.dat', 'bin/dxcompiler.dll', 'bin/dxil.dll')
    $artifacts = @($libraries | ForEach-Object { @{ file = "lib/$($_.Name)"; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() } })
    $artifacts += @($runtimeFiles | ForEach-Object { @{ file = $_; sha256 = (Get-FileHash -LiteralPath (Join-Path $Install $_) -Algorithm SHA256).Hash.ToLowerInvariant() } })
    $skiaMode = 'SK_RELEASE'
    if ($Configuration -eq 'Debug') { $skiaMode = 'SK_DEBUG' }
    $manifest = [ordered]@{
        schema = 1; platform = 'windows'; architecture = 'x64'; configuration = $Configuration
        skia = $SkiaRevision; skia_tree = $SkiaTree; dawn = $DawnRevision; gn = $GnRevision
        runtime = 'MT'; iterator_debug_level = 0; trivial_abi = $false; dxc = 'v1.9.2609'
        libraries = @($libraries.BaseName | Sort-Object)
        syslinks = @('onecore', 'user32', 'gdi32', 'delayimp', 'dxguid', 'libcpmt', 'ole32', 'oleaut32', 'uuid', 'fontsub', 'usp10', 'advapi32')
        includes = @('include/skia', 'include/dawn')
        defines = @('SK_GRAPHITE', 'SK_DAWN', 'SK_DAWN_HAS_D3D12', '_ITERATOR_DEBUG_LEVEL=0', $skiaMode)
        runtime_files = $runtimeFiles
        artifacts = $artifacts
    }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Install 'manifest.json') -Encoding utf8NoBOM
    Copy-Item -LiteralPath $argsFile -Destination (Join-Path $Install 'args.gn') -Force
    Copy-Item -LiteralPath $depsMarker -Destination (Join-Path $Install 'dependencies.json') -Force
    Write-Host "Graphite/Dawn package built and installed at $Install"
} finally {
    Pop-Location
    foreach ($entry in $oldEnvironment.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
    }
}
