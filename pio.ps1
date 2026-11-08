<#
SonicSync32 PlatformIO helper.

ESP-IDF builds reject project paths containing spaces, and this project
lives in "C:\Project Aurora". PlatformIO therefore runs through the
space-free junction C:\ProjectAurora -> C:\Project Aurora (created on
first use; junctions do not require administrator rights). The real
project folder stays exactly where it is.

Usage (from this folder):
    .\pio.ps1 run                          # compile
    .\pio.ps1 run -t upload                # compile and flash
    .\pio.ps1 run -t upload --upload-port COM5
    .\pio.ps1 run -t upload -t monitor     # flash + serial console
    .\pio.ps1 run -t menuconfig            # tune SonicSync32 settings
#>
$ErrorActionPreference = 'Stop'
$real = 'C:\Project Aurora'
$link = 'C:\ProjectAurora'
$pio  = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\platformio.exe'

if (-not (Test-Path $link)) {
    New-Item -ItemType Junction -Path $link -Target $real | Out-Null
    Write-Host "Created junction ${link} -> ${real}"
}
if (-not (Test-Path $pio)) {
    throw "PlatformIO not found at $pio"
}

Push-Location (Join-Path $link 'SonicSync32')
try {
    & $pio @args
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
