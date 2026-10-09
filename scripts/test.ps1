param([ValidateSet('debug', 'release', 'all')][string]$Mode = 'all')

$ErrorActionPreference = 'Stop'
$configurations = if ($Mode -eq 'all') { @('debug', 'release') } else { @($Mode) }
foreach ($configuration in $configurations) {
    & "$PSScriptRoot/xmake.ps1" f -m $configuration -y
    & "$PSScriptRoot/xmake.ps1" build -y astra-tests
    & "$PSScriptRoot/xmake.ps1" test -v
}
