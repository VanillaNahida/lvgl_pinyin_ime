# SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
# SPDX-License-Identifier: Apache-2.0
#
# Flash the generated system dictionary to the "dict" partition and the
# generated fonts to the "font" partition.
#
# Both images are produced by the tools:
#   python tools/gen_dict.py    -> data/dict/dict_pinyin.dat   (1.0 MB)
#   python tools/gen_font.py    -> generated/font_partition.bin (2.4 MB)
#
# Usage (from the repository root or from examples/k26_k9_demo):
#   powershell -ExecutionPolicy Bypass -File tools/flash_dict.ps1 -Port COM17
#   powershell -ExecutionPolicy Bypass -File tools/flash_dict.ps1 -Port COM17 -Font
#   powershell -ExecutionPolicy Bypass -File tools/flash_dict.ps1 -Port COM17 -Dict -Font
#
# The partition table must already be flashed (idf.py flash does that).

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Port,
    [switch]$Dict,
    [switch]$Font,
    [string]$Project = "",
    [int]$Baud = 921600
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

if (-not $Dict -and -not $Font) {
    $Dict = $true
    $Font = $true
}

if (-not $Project) {
    $Project = Join-Path $root "examples\k26_k9_demo"
}
if (-not (Test-Path $Project)) {
    throw "project not found: $Project"
}

$idfPath = $env:IDF_PATH
if (-not $idfPath) {
    throw "IDF_PATH is not set; run D:\Espressif\EIM\v6.1\esp-idf\export.ps1 first"
}

$parttool = Join-Path $idfPath "components\partition_table\parttool.py"
if (-not (Test-Path $parttool)) {
    throw "parttool.py not found at $parttool"
}

function Write-Partition([string]$Name, [string]$Image) {
    if (-not (Test-Path $Image)) {
        throw "image not found: $Image (run the matching tools/ script first)"
    }
    $size = (Get-Item $Image).Length
    Write-Host ("writing {0} ({1:N0} bytes) to partition '{2}' on {3}" -f $Image, $size, $Name, $Port)
    & python $parttool --port $Port --baud $Baud write_partition --partition-name $Name --input $Image
    if ($LASTEXITCODE -ne 0) { throw "parttool failed for partition $Name" }
}

Push-Location $Project
try {
    if ($Dict) {
        Write-Partition "dict" (Join-Path $root "data\dict\dict_pinyin.dat")
    }
    if ($Font) {
        Write-Partition "font" (Join-Path $root "generated\font_partition.bin")
    }
}
finally {
    Pop-Location
}

Write-Host "done"
