param(
    [ValidateSet('debug', 'release', 'all')][string]$Mode = 'all',
    [switch]$Gui,
    [switch]$IsolatedClipboard,
    [switch]$InstalledIme,
    [switch]$Benchmarks
)

$ErrorActionPreference = 'Stop'
if (-not $IsWindows -or [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64') {
    throw 'This acceptance entry point is for native Windows x64.'
}
if ($InstalledIme -and -not $Gui) {
    throw 'Installed IME acceptance requires -Gui and an interactive Windows desktop.'
}
if ($IsolatedClipboard) {
    if (-not $Gui) { throw 'Isolated clipboard acceptance requires -Gui.' }
    $acceptanceIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
    try {
        $acceptancePrincipal = [Security.Principal.WindowsPrincipal]::new($acceptanceIdentity)
        if (-not $acceptancePrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
            throw 'Isolated clipboard acceptance requires an administrator token to create its own Windows station. Run the dedicated fixture elevated or this entry point from an elevated test session.'
        }
    } finally { $acceptanceIdentity.Dispose() }
}
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$configurations = if ($Mode -eq 'all') { @('debug', 'release') } else { @($Mode) }
function Invoke-AcceptanceBinary([string]$Directory, [string]$Name, [string[]]$Arguments = @()) {
    & (Join-Path $Directory ($Name + '.exe')) @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Name failed with exit code $LASTEXITCODE" }
}
Push-Location $repository
try {
    & "$PSScriptRoot/format.ps1" -Check
    foreach ($configuration in $configurations) {
        & "$PSScriptRoot/xmake.ps1" f -m $configuration -y
        $targets = @('astra-tests','astra-server')
        if ($Gui) { $targets += @('astra','graphics-smoke','renderer-smoke','text-layout-tests','viewport-layout-tests','gui-smoke','gui-services-smoke','gui-recovery-smoke','gui-workspace-smoke','gui-presentation-smoke','gui-body-smoke','gui-clipboard-smoke') }
        if ($IsolatedClipboard) { $targets += 'gui-clipboard-async-smoke' }
        if ($InstalledIme) { $targets += 'gui-ime-smoke' }
        if ($Benchmarks -and $configuration -eq 'release') { $targets += @('astra-core-benchmark', 'astra-view-benchmark') }
        & "$PSScriptRoot/xmake.ps1" build -y @targets
        $directory = Join-Path $repository ('build/windows/x64/' + $configuration)
        Invoke-AcceptanceBinary $directory 'astra-tests' @('--no-version','--duration=true')
        & "$repository/tests/cli_smoke.ps1" -Mode $configuration
        if ($Gui) {
            foreach ($name in @('graphics-smoke','renderer-smoke','text-layout-tests','viewport-layout-tests','gui-smoke','gui-services-smoke','gui-recovery-smoke','gui-workspace-smoke','gui-presentation-smoke','gui-body-smoke','gui-clipboard-smoke')) {
                Invoke-AcceptanceBinary $directory $name
            }
            Invoke-AcceptanceBinary $directory 'gui-smoke' @('--tcp')
            if ($IsolatedClipboard) { Invoke-AcceptanceBinary $directory 'gui-clipboard-async-smoke' }
            if ($InstalledIme) { Invoke-AcceptanceBinary $directory 'gui-ime-smoke' }
        }
        if ($Benchmarks -and $configuration -eq 'release') {
            Invoke-AcceptanceBinary $directory 'astra-core-benchmark'
            Invoke-AcceptanceBinary $directory 'astra-core-benchmark' @('--history')
            Invoke-AcceptanceBinary $directory 'astra-view-benchmark'
        }
    }
    & "$repository/tests/package_scripts_tests.ps1"
    & "$repository/.tools/graphics/python/python.exe" "$repository/tests/skia_patch_tests.py"
    if ($LASTEXITCODE -ne 0) { throw 'Skia shaping provenance tests failed' }
} finally {
    Pop-Location
}
