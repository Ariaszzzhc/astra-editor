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
        $guiOption = if ($Gui) { 'ON' } else { 'OFF' }
        $environmentOption = if ($IsolatedClipboard -or $InstalledIme) { 'ON' } else { 'OFF' }
        $benchmarkOption = if ($Benchmarks) { 'ON' } else { 'OFF' }
        & "$PSScriptRoot/cmake.ps1" --preset "windows-$configuration" "-DASTRA_BUILD_GUI_TESTS=$guiOption" "-DASTRA_BUILD_ENVIRONMENT_TESTS=$environmentOption" "-DASTRA_BUILD_BENCHMARKS=$benchmarkOption"
        $targets = @('astra-tests','astra-server')
        if ($Gui) { $targets += @('astra','graphics-smoke','renderer-smoke','text-layout-tests','viewport-layout-tests','gui-smoke','gui-services-smoke','gui-recovery-smoke','gui-workspace-smoke','gui-presentation-smoke','gui-visual-smoke','gui-body-smoke','gui-clipboard-smoke') }
        if ($IsolatedClipboard) { $targets += 'gui-clipboard-async-smoke' }
        if ($InstalledIme) { $targets += 'gui-ime-smoke' }
        if ($Benchmarks -and $configuration -eq 'release') { $targets += @('astra-core-benchmark', 'astra-view-benchmark') }
        & "$PSScriptRoot/cmake.ps1" --build --preset "windows-$configuration" --target @targets
        $directory = Join-Path $repository ("build/cmake/windows-$configuration/bin")
        & "$PSScriptRoot/cmake.ps1" -CTest --preset "windows-$configuration" -LE environment
        if ($Gui) {
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
} finally {
    Pop-Location
}
