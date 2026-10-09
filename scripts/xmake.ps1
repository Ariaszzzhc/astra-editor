param([Parameter(ValueFromRemainingArguments = $true)][string[]]$XmakeArguments)

$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$toolsDirectory = Join-Path $repository '.tools'
$tool = Join-Path $toolsDirectory 'xmake/xmake/xmake.exe'
$manifest = Get-Content -LiteralPath (Join-Path $repository 'dependencies.lock.json') -Raw | ConvertFrom-Json
$version = $manifest.xmake.version
$expectedHash = $manifest.xmake.windowsArchiveSha256

if (-not $IsWindows) { throw 'This bootstrap is for Windows. Use native xmake on other systems.' }
if (-not (Test-Path -LiteralPath $tool)) {
    $downloads = Join-Path $toolsDirectory 'downloads'
    New-Item -ItemType Directory -Force $downloads | Out-Null
    $archive = Join-Path $downloads "xmake-v$version.win64.zip"
    if (-not (Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest "https://github.com/xmake-io/xmake/releases/download/v$version/xmake-v$version.win64.zip" -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expectedHash) {
        throw "SHA-256 mismatch for $archive"
    }
    Expand-Archive -LiteralPath $archive -DestinationPath (Join-Path $toolsDirectory 'xmake') -Force
}

$recipeRevision = $manifest.recipes.revision
$recipeDirectory = Join-Path $toolsDirectory "recipes/xmake-repo-$recipeRevision"
if (-not (Test-Path -LiteralPath (Join-Path $recipeDirectory '.astra-verified'))) {
    $downloads = Join-Path $toolsDirectory 'downloads'
    New-Item -ItemType Directory -Force $downloads | Out-Null
    $archive = Join-Path $downloads "xmake-repo-$recipeRevision.zip"
    if (-not (Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest "https://codeload.github.com/xmake-io/xmake-repo/zip/$recipeRevision" -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $manifest.recipes.archiveSha256) {
        throw "SHA-256 mismatch for $archive"
    }
    Expand-Archive -LiteralPath $archive -DestinationPath (Join-Path $toolsDirectory 'recipes') -Force
    Set-Content -LiteralPath (Join-Path $recipeDirectory '.astra-verified') -Value $recipeRevision
}

$env:XMAKE_GLOBALDIR = Join-Path $toolsDirectory 'xmake-state'
$env:XMAKE_PKG_CACHEDIR = Join-Path $toolsDirectory 'package-cache'
$env:XMAKE_PKG_INSTALLDIR = Join-Path $toolsDirectory 'packages'
$env:XMAKE_COLORTERM = 'nocolor'
Push-Location $repository
try {
    & $tool @XmakeArguments
    if ($LASTEXITCODE -ne 0) { throw "xmake failed with exit code $LASTEXITCODE" }
} finally { Pop-Location }
