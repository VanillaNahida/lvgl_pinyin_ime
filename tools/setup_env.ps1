# SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
# SPDX-License-Identifier: Apache-2.0
#
# Create the isolated Python environment used by the tools/ scripts.
#
# Nothing is installed globally: the venv lives in tools/venv and every script
# is invoked as tools\venv\Scripts\python.exe <script>.py.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools/setup_env.ps1
#   powershell -ExecutionPolicy Bypass -File tools/setup_env.ps1 -Recreate

[CmdletBinding()]
param(
    [string]$Python = "",
    [switch]$Recreate
)

$ErrorActionPreference = "Stop"
$toolsDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$venvDir = Join-Path $toolsDir "venv"
$requirements = Join-Path $toolsDir "requirements.txt"

if ($Recreate -and (Test-Path $venvDir)) {
    Write-Host "removing $venvDir"
    Remove-Item -Recurse -Force $venvDir
}

if (-not $Python) {
    # Prefer the ESP-IDF Python so the same interpreter builds the tooling and
    # the firmware side scripts, but any CPython 3.8+ works.
    $candidates = @(
        "D:\Espressif\EIM\tools\idf-python\3.11.2\python.exe",
        "D:\Espressif\python_env\idf6.1_py3.12_env\Scripts\python.exe"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { $Python = $candidate; break }
    }
}
if (-not $Python) {
    $found = Get-Command python -ErrorAction SilentlyContinue
    if (-not $found) { throw "no python found; pass -Python <path>" }
    $Python = $found.Source
}

Write-Host "python : $Python"
if (-not (Test-Path (Join-Path $venvDir "Scripts\python.exe"))) {
    Write-Host "creating $venvDir"
    & $Python -m venv $venvDir
    if ($LASTEXITCODE -ne 0) { throw "venv creation failed" }
}

$venvPython = Join-Path $venvDir "Scripts\python.exe"
Write-Host "installing $requirements into the venv only"
& $venvPython -m pip install --upgrade pip | Out-Null
& $venvPython -m pip install -r $requirements
if ($LASTEXITCODE -ne 0) { throw "pip install failed" }

Write-Host ""
Write-Host "ready. use it as:"
Write-Host "  .\tools\venv\Scripts\python.exe tools\gen_charset.py"
Write-Host "  .\tools\venv\Scripts\python.exe tools\gen_font.py"
