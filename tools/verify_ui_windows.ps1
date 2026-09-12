$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $env:IDF_PATH) {
    throw 'Set IDF_PATH to an ESP-IDF 5.4.x install before the host UI review.'
}
$cmake = if ($env:CMAKE) { $env:CMAKE } else { 'cmake' }
Push-Location $projectRoot
try {
    & $cmake -S tools/host -B build-win/host-windows -G 'Visual Studio 17 2022' -A x64 "-DIDF_PATH=$env:IDF_PATH"
    if ($LASTEXITCODE -ne 0) { throw 'Host configure failed' }
    & $cmake --build build-win/host-windows --config Release -j 8
    if ($LASTEXITCODE -ne 0) { throw 'Host build failed' }
    & './build-win/host-windows/Release/rtp_review.exe'
    if ($LASTEXITCODE -ne 0) { throw 'RTP verification failed' }
    & './build-win/host-windows/Release/receipt_review.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Receipt verification failed' }
    & './build-win/host-windows/Release/latest_job.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Latest IDR verification failed' }
    New-Item -ItemType Directory -Force assets/preview-review | Out-Null
    Push-Location assets/preview-review
    try {
        & '../../build-win/host-windows/Release/smoke.exe'
        if ($LASTEXITCODE -ne 0) { throw 'LVGL verification failed' }
    } finally { Pop-Location }
} finally { Pop-Location }
