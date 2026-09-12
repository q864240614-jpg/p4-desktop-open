param(
    [switch]$Flash,
    [string]$Port
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $env:IDF_PATH) {
    throw 'Set IDF_PATH to an ESP-IDF 5.4.x install before building.'
}
Push-Location $projectRoot
try {
    $idfArgs = @('-B', 'build-win', '-D', 'CCACHE_ENABLE=0', 'build')
    if ($Flash) {
        if (-not $Port) { throw 'Pass -Port COMx (or /dev/ttyACM0) when flashing.' }
        $idfArgs = @('-B', 'build-win', '-D', 'CCACHE_ENABLE=0', '-p', $Port, 'build', 'flash')
    }
    & idf.py @idfArgs
    if ($LASTEXITCODE -ne 0) { throw "ESP-IDF failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}
