# Syntax/type check of the voice module using the compile flags of an existing IDF
# build directory.
#
# Why not idf.py: that re-runs cmake and regenerates the config (tens of seconds),
# and the project's current sdkconfig has voice input turned OFF, so the voice
# translation units would not even be compiled. This script reuses the compile
# command that examples/k26_k9_demo/build/compile_commands.json already holds for
# this component (same cross compiler, same include paths) and adds -fsyntax-only
# plus a voice-enabled config header. A few seconds, and it catches type errors,
# missing includes and bad Kconfig macro plumbing.
#
# NOTE: keep this file ASCII-only. Windows PowerShell reads a BOM-less .ps1 as
# ANSI/GBK, so non-ASCII source here turns into mojibake and breaks parsing.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools/check_voice_build.ps1

$ErrorActionPreference = 'Stop'

# Native compiler diagnostics on stderr would otherwise be turned into PowerShell
# ErrorRecords and abort the run; collect them from a file instead.
$ErrorActionPreference = 'Continue'

$repo = Split-Path -Parent $PSScriptRoot
$ccjson = Join-Path $repo 'examples\k26_k9_demo\build\compile_commands.json'

if (-not (Test-Path $ccjson)) {
    Write-Error 'compile_commands.json not found; run idf.py build in examples/k26_k9_demo first'
}

# Read as UTF-8 explicitly. Windows PowerShell defaults to the ANSI code page for
# Get-Content, which mangles a non-ASCII path out of compile_commands.json — and
# the JSON escapes backslashes, so a mis-decoded string can end up with doubled
# separators that no longer name an executable.
$entries = [System.IO.File]::ReadAllText($ccjson, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
$probe = $entries |
    Where-Object { $_.file -like '*lv_pinyin_ime.c' -and $_.file -notlike '*voice*' } |
    Select-Object -First 1
if ($null -eq $probe) {
    Write-Error 'no compile command for lv_pinyin_ime.c in compile_commands.json'
}

# Strip -o <obj> and -c <src>; keep every include / define / optimization flag.
$cmd = $probe.command
$cmd = $cmd -replace '\s-o\s+"?[^"]*\.obj"?', ''
$cmd = $cmd -replace '\s-c\s+"?[^"]*"?\s*$', ''
$cmd = $cmd.Trim()

# Normalise the compiler path (collapse any doubled separators) and use forward
# slashes: gcc's response-file tokenizer eats backslashes, and Windows accepts
# forward slashes everywhere.
$compiler = ($cmd -split '\s+')[0]
$compiler = ($compiler -replace '\\\\+', '\') -replace '\\', '/'
if (-not (Test-Path -LiteralPath $compiler)) {
    Write-Error "compiler not found: $compiler"
}
Write-Host "compiler: $compiler"

# The component's own private include dirs come from target_include_directories()
# in its CMakeLists.txt, which is NOT part of the exported compile_commands.json
# entry — so they have to be reconstructed here.
#
# ★ Read them out of CMakeLists.txt instead of hardcoding them. Hardcoding is how
#   this check once passed while the real build failed: the list here contained
#   src/ui, the CMakeLists did not, so ime_voice_ui.c compiled fine under the
#   check and then could not find lv_pinyin_ime_internal.h under idf.py build.
$cmake_lists = Join-Path $repo 'CMakeLists.txt'
$extra = @()
if (Test-Path $cmake_lists) {
    $text = [System.IO.File]::ReadAllText($cmake_lists, [System.Text.Encoding]::UTF8)
    $block = [regex]::Match($text,
        'target_include_directories\(\$\{COMPONENT_LIB\} PRIVATE(.*?)\)',
        [System.Text.RegularExpressions.RegexOptions]::Singleline)
    if ($block.Success) {
        foreach ($m in [regex]::Matches($block.Groups[1].Value,
                                        '\$\{CMAKE_CURRENT_LIST_DIR\}/([^"\s]+)')) {
            $extra += ('-I' + (Join-Path $repo $m.Groups[1].Value) -replace '\\', '/')
        }
    }
}
if ($extra.Count -eq 0) {
    Write-Error "could not read the component's include dirs from $cmake_lists"
}
Write-Host ("include dirs from CMakeLists.txt: " + ($extra -join ' '))

$cfg_dir = Join-Path $repo 'test_apps\idf_component_build'
$cfg = Join-Path $cfg_dir 'voice_check_config.h'
$cfg_off = Join-Path $cfg_dir 'voice_check_config_off.h'

$voice_defines = @(
    '#define CONFIG_LV_PINYIN_IME_VOICE_ENABLE 1'
    '#define CONFIG_LV_PINYIN_IME_VOICE_APPID "00000000"'
    '#define CONFIG_LV_PINYIN_IME_VOICE_APIKEY "0123456789abcdef0123456789abcdef"'
    '#define CONFIG_LV_PINYIN_IME_VOICE_APISECRET "fedcba9876543210fedcba9876543210"'
    '#define CONFIG_LV_PINYIN_IME_VOICE_ENDPOINT "wss://iat-api.xfyun.cn/v2/iat"'
    '#define CONFIG_LV_PINYIN_IME_VOICE_LANGUAGE "zh_cn"'
    '#define CONFIG_LV_PINYIN_IME_VOICE_DOMAIN "iat"'
    '#define CONFIG_LV_PINYIN_IME_VOICE_ACCENT "mandarin"'
    '#define CONFIG_LV_PINYIN_IME_VOICE_SAMPLE_RATE 16000'
    '#define CONFIG_LV_PINYIN_IME_VOICE_EOS_MS 2000'
    '#define CONFIG_LV_PINYIN_IME_VOICE_DWA 1'
    '#define CONFIG_LV_PINYIN_IME_VOICE_PUNCTUATION 1'
    '#define CONFIG_LV_PINYIN_IME_VOICE_LIVE_PREVIEW 1'
    '#define CONFIG_LV_PINYIN_IME_VOICE_COMMIT_APPEND 1'
    '#define CONFIG_LV_PINYIN_IME_VOICE_TAP_BOTH 1'
    '#define CONFIG_LV_PINYIN_IME_VOICE_KEY_K9 1'
)

# The base: the project's own sdkconfig.h if it has been built, minus anything
# voice- or example-related (those are what this check varies).
$sdkconfig_h = Join-Path $repo 'examples\k26_k9_demo\build\config\sdkconfig.h'
$base = New-Object System.Collections.Generic.List[string]
if (Test-Path $sdkconfig_h) {
    foreach ($l in [System.IO.File]::ReadAllLines($sdkconfig_h, [System.Text.Encoding]::UTF8)) {
        if ($l -match 'CONFIG_LV_PINYIN_IME_VOICE_|CONFIG_EXAMPLE_') { continue }
        $base.Add($l)
    }
} else {
    Write-Host "note: $sdkconfig_h not found; the check headers will carry only the voice symbols"
}

$ascii = New-Object System.Text.ASCIIEncoding

# voice ON: base + the voice symbols.
$on = New-Object System.Collections.Generic.List[string]
$on.Add('/* GENERATED by tools/check_voice_build.ps1 - do not edit. Voice ENABLED. */')
$on.AddRange($base)
$on.Add('')
$on.Add('/* Voice input enabled (what this pass checks). */')
$on.AddRange($voice_defines)
[System.IO.File]::WriteAllLines($cfg, $on, $ascii)

# voice OFF: base alone. This is the DEFAULT configuration of the component, and
# the one most integrators compile, so it gets its own pass - with voice off,
# src/voice/ is not built at all and every #if in src/ui/ takes the other branch.
$off = New-Object System.Collections.Generic.List[string]
$off.Add('/* GENERATED by tools/check_voice_build.ps1 - do not edit. Voice DISABLED. */')
$off.AddRange($base)
[System.IO.File]::WriteAllLines($cfg_off, $off, $ascii)

Write-Host "config headers: $($on.Count) lines (on), $($off.Count) lines (off)"

$ui_sources = @(
    'src\ui\lv_pinyin_ime.c'
    'src\ui\lv_pinyin_ime_kb.c'
    'src\ui\lv_pinyin_ime_cand.c'
)

# Two passes, because the default configuration is "voice off" and that is the
# code path most integrators compile: src/voice/ is not built at all then, and the
# #if branches inside src/ui/ take the other side. Checking only the enabled pass
# would leave the common case unverified.
$scenarios = @(
    @{
        Name    = 'voice ON'
        Config  = $cfg
        Sources = @(
            'src\voice\ime_voice.c'
            'src\voice\ime_voice_crypto.c'
            'src\voice\ime_voice_proto.c'
            'src\voice\ime_voice_ui.c'
        ) + $ui_sources
    }
    @{
        Name    = 'voice OFF (default)'
        Config  = $cfg_off
        Sources = $ui_sources
    }
)

# How to hand the compile command to the compiler - the two gotchas, both
# verified on this machine:
#
#   1. The command carries cmd.exe style escaped quotes (-DIDF_VER=\"v6.1\").
#      PowerShell does NOT unescape those for a native command: it passes \" on
#      verbatim, so the backslash lands in the program text and cc1 reports
#      "stray '\' in program". cmd.exe's tokenizer handles them correctly.
#   2. gcc's @response-file tokenizer treats a backslash as an escape character,
#      so a Windows path inside a response file loses its separators
#      (C:\Users\x becomes C:Usersx).
#
# So: use cmd.exe, and quote the whole command line once (the compiler path has
# spaces in it). $cmd already contains the double quotes cmd expects.
$stderr_log = Join-Path $env:TEMP 'ime_voice_check.log'

$failed = 0
foreach ($scenario in $scenarios) {
    Write-Host "--- $($scenario.Name) ---"

    $inc = '-include "' + ($scenario.Config -replace '\\', '/') + '"'

    foreach ($src in $scenario.Sources) {
        $path = Join-Path $repo $src
        if (-not (Test-Path $path)) {
            Write-Host "skip (missing): $src"
            continue
        }
        Write-Host -NoNewline "check $src ..."

        # gcc accepts forward slashes everywhere, and they survive cmd's tokenizer
        # without needing extra quoting.
        $full = $cmd + ' ' + $inc + ' ' + ($extra -join ' ') +
                ' -Wno-error=pragma-once-outside-header -fsyntax-only "' +
                ($path -replace '\\', '/') + '"'

        cmd /c "`"$full`"" > $stderr_log 2>&1
        $code = $LASTEXITCODE
        if ($code -eq 0) {
            Write-Host ' ok'
        } else {
            Write-Host ' FAILED'
            Get-Content $stderr_log | Select-Object -First 40 |
                ForEach-Object { Write-Host ('    ' + $_) }
            $failed++
        }
    }
}

if ($failed -gt 0) {
    Write-Host "$failed file(s) failed"
    exit 1
}
Write-Host 'all ok'
