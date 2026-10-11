# Measure the real stack frames of the voice module.
#
# Why: the ESP32-S31 build crashed with
#   "Stack protection fault ... task ime_voice ... handle_reply"
# because ime_voice_reply_t (12 744 B) was a function-local variable on a task
# stack of a few KB. Eyeballing code for that class of bug is unreliable, so this
# asks the compiler instead: -fstack-usage emits the frame size of every function,
# and depth.py turns those frames into worst-case call chains.
#
# NOTE: keep this file ASCII-only (Windows PowerShell reads BOM-less .ps1 as ANSI).
#
# Usage: powershell -ExecutionPolicy Bypass -File tools/check_stack_usage.ps1

$ErrorActionPreference = 'Continue'

$repo = Split-Path -Parent $PSScriptRoot
$ccjson = Join-Path $repo 'examples\k26_k9_demo\build\compile_commands.json'
if (-not (Test-Path $ccjson)) {
    Write-Error 'compile_commands.json not found; run idf.py build in examples/k26_k9_demo first'
}

$entries = [System.IO.File]::ReadAllText($ccjson, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
$probe = $entries |
    Where-Object { $_.file -like '*lv_pinyin_ime.c' -and $_.file -notlike '*voice*' } |
    Select-Object -First 1
if ($null -eq $probe) { Write-Error 'no compile command for lv_pinyin_ime.c' }

function Convert-ToForwardSlashPath([string]$text) {
    $sb = New-Object System.Text.StringBuilder
    $i = 0
    while ($i -lt $text.Length) {
        $ch = $text[$i]
        if ($ch -eq '"') {
            $end = $i + 1
            while ($end -lt $text.Length) {
                if ($text[$end] -eq '\' -and ($end + 1) -lt $text.Length) { $end += 2; continue }
                if ($text[$end] -eq '"') { $end++; break }
                $end++
            }
            [void]$sb.Append($text.Substring($i, $end - $i))
            $i = $end
            continue
        }
        if ($ch -eq '\' -or $ch -eq '/') {
            if ($ch -eq '\' -and ($i + 1) -lt $text.Length -and $text[$i + 1] -eq '"') {
                [void]$sb.Append($ch); $i++; continue
            }
            while ($i -lt $text.Length -and ($text[$i] -eq '\' -or $text[$i] -eq '/')) { $i++ }
            [void]$sb.Append('/')
            continue
        }
        [void]$sb.Append($ch)
        $i++
    }
    return $sb.ToString()
}

$base = $probe.command
$base = $base -replace '\s-o\s+"?[^"]*\.obj"?', ''
$base = $base -replace '\s-c\s+"?[^"]*"?\s*$', ''
$base = Convert-ToForwardSlashPath $base.Trim()
$compiler = ($base -split '\s+')[0]
if (-not (Test-Path -LiteralPath $compiler)) { Write-Error "compiler not found: $compiler" }
$rest = $base.Substring($compiler.Length)

$extra = @()
$cmake_lists = Join-Path $repo 'CMakeLists.txt'
$text = [System.IO.File]::ReadAllText($cmake_lists, [System.Text.Encoding]::UTF8)
$block = [regex]::Match($text,
    'target_include_directories\(\$\{COMPONENT_LIB\} PRIVATE(.*?)\)',
    [System.Text.RegularExpressions.RegexOptions]::Singleline)
if ($block.Success) {
    foreach ($m in [regex]::Matches($block.Groups[1].Value, '\$\{CMAKE_CURRENT_LIST_DIR\}/([^"\s]+)')) {
        # Parenthesise: `'a' + $x -replace 'b','c'` binds the + first and then
        # PowerShell sees three operands for -replace.
        $dir = (Join-Path $repo $m.Groups[1].Value) -replace '\\', '/'
        $extra += ('-I"' + $dir + '"')
    }
}

$cfg = Join-Path $repo 'test_apps\idf_component_build\voice_check_config.h'
$out = Join-Path $env:TEMP 'ime_voice_su'
New-Item -ItemType Directory -Force -Path $out | Out-Null
Get-ChildItem $out -Filter '*.su' | Remove-Item -Force -ErrorAction SilentlyContinue

$sources = @(
    'src\voice\ime_voice.c'
    'src\voice\ime_voice_crypto.c'
    'src\voice\ime_voice_proto.c'
    'src\voice\ime_voice_ui.c'
)

$failed = 0
foreach ($src in $sources) {
    $path = Join-Path $repo $src
    Write-Host -NoNewline "frame analysis $src ..."
    $full = $compiler + $rest + ' ' + ($extra -join ' ') +
            ' -include "' + (Convert-ToForwardSlashPath $cfg) + '"' +
            ' -Wno-error=pragma-once-outside-header' +
            ' -fstack-usage -c "' + (Convert-ToForwardSlashPath $path) + '"' +
            ' -o "' + (Convert-ToForwardSlashPath (Join-Path $out ([System.IO.Path]::GetFileNameWithoutExtension($src) + '.o'))) + '"'
    cmd /c "`"$full`"" > (Join-Path $out 'log.txt') 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host ' ok'
    } else {
        Write-Host ' FAILED'
        Get-Content (Join-Path $out 'log.txt') | Select-Object -First 20 | ForEach-Object { Write-Host ('    ' + $_) }
        $failed++
    }
}
if ($failed -gt 0) { exit 1 }

Write-Host ''
Write-Host '=== largest stack frames (bytes) ==='
$rows = @()
# Pass the full path: Get-ChildItem returns objects whose .Name is bare, and
# Get-Content would then resolve it against the current directory instead of $out.
foreach ($f in (Get-ChildItem -Path $out -Filter '*.su')) {
    foreach ($line in Get-Content -LiteralPath $f.FullName) {
        $parts = $line -split "`t"
        if ($parts.Count -ge 2) {
            $fn = ($parts[0] -split ':')[0]
            $size = 0
            if ([int]::TryParse($parts[1], [ref]$size)) {
                $rows += [pscustomobject]@{ File = $f.BaseName; Func = $fn; Frame = $size }
            }
        }
    }
}
$rows | Sort-Object Frame -Descending | Select-Object -First 15 |
    Format-Table -AutoSize | Out-String -Width 120 | Write-Host

$total = ($rows | Measure-Object Frame -Sum).Sum
$voice_c = [System.IO.File]::ReadAllText((Join-Path $repo 'src\voice\ime_voice.c'))
# IME_VOICE_TASK_STACK goes straight into xTaskCreate, whose depth argument is in
# BYTES on the ESP-IDF ports: portSTACK_TYPE is uint8_t for both riscv and xtensa
# (FreeRTOS-Kernel/portable/*/include/freertos/portmacro.h).
#
# ★ Do NOT multiply by sizeof(StackType_t) here. That mistake made this script print
#   32768 for a real 8192 byte stack, and the same confusion let a 1536 "6 KB" stack
#   ship to a device — which then crashed with Stack bounds of exactly 1524 bytes.
$stack = [int][regex]::Match($voice_c,
    '#define\s+IME_VOICE_TASK_STACK\s+(\d+)').Groups[1].Value
if ($stack -eq 0) {
    $stack = [int][regex]::Match($voice_c,
        'xTaskCreate\(voice_task, "ime_voice", (\d+)').Groups[1].Value
}
Write-Host ("functions: {0}, sum of frames: {1} B" -f $rows.Count, $total)
Write-Host ("ime_voice task stack: {0} B (xTaskCreate depth is a byte count)" -f $stack)
if ($stack -gt 0) {
    $headroom = $stack - ($rows | Sort-Object Frame -Descending | Select-Object -First 1).Frame
    Write-Host ("headroom over the largest single frame: {0} B" -f $headroom)
    if ($headroom -lt 1024) {
        Write-Host 'WARNING: less than 1 KB headroom over the largest frame' -ForegroundColor Yellow
    }
}
