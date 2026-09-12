param(
    [Parameter(Mandatory = $true)]
    [string]$Port,
    [int]$Baud = 460800,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$buildRoot = Join-Path $projectRoot 'build-win'
if (-not $env:IDF_PATH) {
    throw 'Set IDF_PATH to an ESP-IDF 5.4.x install before flashing.'
}
$python = if ($env:IDF_PYTHON_ENV_PATH) {
    Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts/python.exe'
} else {
    'python'
}
$config = Get-Content -LiteralPath (Join-Path $buildRoot 'flasher_args.json') -Raw | ConvertFrom-Json
$flashArgs = @('-m', 'esptool', '--chip', 'esp32p4', '-p', $Port, '-b', "$Baud",
    '--before', 'default_reset', '--after', 'hard_reset', 'write_flash')
$flashArgs += $config.write_flash_args
foreach ($entry in $config.flash_files.PSObject.Properties | Sort-Object { [Convert]::ToInt32($_.Name, 16) }) {
    $binary = (Resolve-Path -LiteralPath (Join-Path $buildRoot $entry.Value)).Path
    $flashArgs += @($entry.Name, $binary)
}
if ($DryRun) {
    Write-Output $python
    Write-Output ($flashArgs -join ' ')
    exit 0
}
& $python @flashArgs
if ($LASTEXITCODE -ne 0) { throw "Flash failed with exit code $LASTEXITCODE" }
