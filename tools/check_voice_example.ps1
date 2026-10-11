# Syntax/type check for the example project's voice sources (net_wifi.c, net_ws.c,
# mic_i2s.c). Same trick as tools/check_voice_build.ps1: reuse the compile flags
# that examples/k26_k9_demo/build/compile_commands.json already holds, add the
# include paths of every component in that build (the voice sources pull in
# esp_event / esp_wifi / esp_driver_i2s / esp_crt_bundle, whose include dirs are
# not part of the IME component's own compile command), and run -fsyntax-only.
#
# Why this exists: the example's voice sources cannot be checked by
# check_voice_build.ps1 (they are not part of the component), and a full
# `idf.py build` with voice enabled would need the esp_websocket_client managed
# component to be downloaded. This gets 95% of the value in seconds: it validates
# every IDF API call, struct field and header name against the real ESP-IDF 6.1
# headers on this machine.
#
# What it does NOT validate: the managed component's own header
# (esp_websocket_client.h) is not in the IDF tree, so it is not available here.
# Those calls in net_ws.c are commented as version-sensitive.
#
# NOTE: keep this file ASCII-only (Windows PowerShell reads BOM-less .ps1 as ANSI).
#
# Usage: powershell -ExecutionPolicy Bypass -File tools/check_voice_example.ps1

$ErrorActionPreference = 'Continue'

$repo = Split-Path -Parent $PSScriptRoot
$ccjson = Join-Path $repo 'examples\k26_k9_demo\build\compile_commands.json'
if (-not (Test-Path $ccjson)) {
    Write-Error 'compile_commands.json not found; run idf.py build in examples/k26_k9_demo first'
}

$entries = [System.IO.File]::ReadAllText($ccjson, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
$probe = $entries |
    Where-Object { $_.file -like '*main.c' -and $_.file -like '*k26_k9_demo*' } |
    Select-Object -First 1
if ($null -eq $probe) {
    Write-Error 'no compile command for the example main.c'
}

# Normalise a command line: collapse doubled separators and use forward slashes
# (Windows accepts them, and gcc's response-file tokenizer eats backslashes). The
# quoted spans are kept verbatim so that -DIDF_VER=\"v6.1\" survives.
#
# The masking regex has to tolerate a lone backslash before the closing quote
# (cmd.exe style \" ), which a naive "(\\.|[^"\\])*" pattern does not match — with
# it, `-DIDF_VER=\"v6.1\"` was only half masked, the leftover backslash survived
# the path conversion and the tokenizer then swallowed the following arguments
# (the symptom was "no input files").
# Normalise a command line: collapse doubled separators and use forward slashes
# (Windows accepts them, and gcc's response-file tokenizer eats backslashes), while
# leaving quoted spans — and the cmd.exe style \" inside them — untouched, so that
# -DIDF_VER=\"v6.1\" survives.
#
# Implemented as a character walk rather than a regex: the regex version needed a
# match evaluator with a captured index list, and getting that right inside
# PowerShell's scope rules cost far more than this loop. Two concrete symptoms it
# produced were a mangled define (-DIDF_VER=/"v6.1\") and, from there, gcc
# reporting "no input files" because the unbalanced quote swallowed the source
# file argument.
function Convert-ToForwardSlashPath([string]$text) {
    $sb = New-Object System.Text.StringBuilder
    $i = 0
    while ($i -lt $text.Length) {
        $ch = $text[$i]
        if ($ch -eq '"') {
            # Copy the whole quoted span verbatim, honouring \" so it does not end
            # the span early.
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
            # A backslash directly in front of a quote is a cmd.exe escape (as in
            # -DIDF_VER=\"v6.1\"), not a path separator: leave it alone. Treating
            # it as a separator is what produced -DIDF_VER=/"v6.1\" earlier.
            if ($ch -eq '\' -and ($i + 1) -lt $text.Length -and $text[$i + 1] -eq '"') {
                [void]$sb.Append($ch)
                $i++
                continue
            }
            # Collapse a run of separators to a single forward slash.
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
if (-not (Test-Path -LiteralPath $compiler)) {
    Write-Error "compiler not found: $compiler"
}
$rest = $base.Substring($compiler.Length)

# Every include dir used anywhere in this build, deduplicated.
$incs = New-Object System.Collections.Generic.HashSet[string]
foreach ($e in $entries) {
    foreach ($m in [regex]::Matches($e.command, '-I("[^"]+"|\S+)')) {
        $d = $m.Value.Substring(2).Trim('"')
        [void]$incs.Add((Convert-ToForwardSlashPath $d.TrimEnd('/')))
    }
}
[void]$incs.Add((Convert-ToForwardSlashPath (Join-Path $repo 'examples\k26_k9_demo\main')))

# Stub header for the one managed component this example uses
# (espressif/esp_websocket_client). Without it net_ws.c stops at "file not found"
# and none of its API usage gets checked. See the stub's own header for why this is
# safe and what it does not cover.
[void]$incs.Add((Convert-ToForwardSlashPath (Join-Path $repo 'test_apps\idf_component_build\stub_include')))

$cfg = Convert-ToForwardSlashPath (Join-Path $repo 'test_apps\idf_component_build\voice_check_config.h')

$sources = @('net_wifi.c', 'net_ws.c', 'mic_i2s.c')
$rsp = Join-Path $env:TEMP 'ime_voice_example.rsp'
$log = Join-Path $env:TEMP 'ime_voice_example.log'

$failed = 0
foreach ($src in $sources) {
    $path = Join-Path $repo ('examples\k26_k9_demo\main\' + $src)
    if (-not (Test-Path $path)) {
        Write-Host "skip (missing): $src"
        continue
    }
    Write-Host -NoNewline "check $src ..."

    # Response file: the full include list does not fit in a cmd.exe command line.
    $lines = @($rest)
    $lines += ($incs | Sort-Object | ForEach-Object { '-I"' + $_ + '"' })
    $lines += '-include'
    $lines += '"' + $cfg + '"'
    $lines += '-Wno-error=pragma-once-outside-header'
    $lines += '-fsyntax-only'
    $lines += '"' + (Convert-ToForwardSlashPath $path) + '"'
    Set-Content -Path $rsp -Value $lines -Encoding ASCII

    & $compiler ('@' + $rsp) > $log 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host ' ok'
    } else {
        Write-Host ' FAILED'
        Write-Host ('    rsp: ' + $rsp)
        Get-Content $log | Select-Object -First 30 | ForEach-Object { Write-Host ('    ' + $_) }
        $failed++
        break       # keep the response file for inspection
    }
}
if ($failed -eq 0) {
    Remove-Item $rsp -ErrorAction SilentlyContinue
}

if ($failed -gt 0) {
    Write-Host "$failed file(s) failed"
    exit 1
}
Write-Host 'all ok'
