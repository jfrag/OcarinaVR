# Puts the lines of hooks.json into a Longshot source tree, and a copy of our adapter beside Longshot's renderer.
# Every line put in carries the mark "// (retraced <id>)" ("# (retraced <id>)" in a Python file). A missing or doubled
# anchor makes the script FAIL without writing anything: that is the alarm for a Longshot that has changed.
# Files that do not change are not written again (the build's object cache stays valid).
#   -Longshot : the Longshot source folder (default: _build\LongshotBuilder\tools, where the installer puts it)
#   -Check    : says what would be done, writes nothing
#   -Remove   : takes all our lines and the adapter out (back to Longshot as it was)
param([string]$Longshot = '', [switch]$Check, [switch]$Remove)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Longshot) { $Longshot = Join-Path $root '_build\LongshotBuilder\tools' }
$cfg = Get-Content (Join-Path $root 'hooks.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$utf8 = New-Object System.Text.UTF8Encoding $false
$mark = '// (retraced '

function Read-Text([string]$p) { [IO.File]::ReadAllText($p, $utf8) }
function Write-Text([string]$p, [string]$t) {
    if ((Test-Path $p) -and ((Read-Text $p) -eq $t)) { return $false }
    if (-not $Check) { [IO.File]::WriteAllText($p, $t, $utf8) }
    return $true
}

$files = @{}
foreach ($h in $cfg.hooks) {
    if (-not $files.ContainsKey($h.file)) { $files[$h.file] = @() }
    $files[$h.file] += $h
}

$changes = 0
foreach ($rel in $files.Keys) {
    $path = Join-Path $Longshot $rel
    if (-not (Test-Path $path)) { throw "FAILED: file not found: $rel" }
    $text = Read-Text $path
    $nl = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
    $m = if ($rel -like '*.py') { '# (retraced ' } else { $mark }
    $lines = [System.Collections.Generic.List[string]]::new()
    foreach ($l in ($text -split "`r?`n")) { if (-not $l.Contains($m)) { $lines.Add($l) } }   # our lines are always put in again
    if (-not $Remove) {
        foreach ($h in $files[$rel]) {
            $idx = @()
            for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i].Contains($h.anchor)) { $idx += $i } }
            if ($idx.Count -ne 1) { throw "FAILED: the anchor of $($h.id) was found $($idx.Count) times in $rel : $($h.anchor)" }
            $i = $idx[0]
            $indent = [regex]::Match($lines[$i], '^\s*').Value
            $new = "$indent$($h.line)   $m$($h.id))"
            if ($h.where -eq 'before') { $lines.Insert($i, $new) } else { $lines.Insert($i + 1, $new) }
        }
    }
    if (Write-Text $path ([string]::Join($nl, $lines))) { $changes++; "changed: $rel" } else { "unchanged: $rel" }
}

# the adapter, with rt_api.h written in place of its #include (Longshot only follows its own headers)
$dst = Join-Path $Longshot $cfg.adapter.target
if ($Remove) {
    if (Test-Path $dst) {
        if (-not $Check) { Remove-Item -LiteralPath $dst }
        $changes++; "adapter removed: $($cfg.adapter.target)"
    }
} else {
    $src = Read-Text (Join-Path $root $cfg.adapter.source)
    $api = Read-Text (Join-Path $root $cfg.adapter.api)
    $inc = [regex]::Match($src, '(?m)^#include "rt_api\.h".*$')
    if (-not $inc.Success) { throw 'FAILED: #include "rt_api.h" not found in the adapter' }
    $body = "// ---- src/rt_api.h, copied by tools/apply-hooks.ps1 ----`n" + $api.Replace("`r`n", "`n").TrimEnd() + "`n// ---- end of rt_api.h ----"
    $src = $src.Replace("`r`n", "`n").Replace($inc.Value, $body)
    $src = $src.Replace('#ifndef RTX_HOST_REVISION', "#define RTX_HOST_REVISION `"$($cfg.base)`"`n#ifndef RTX_HOST_REVISION")
    $src = "// GENERATED from $($cfg.adapter.source) by tools/apply-hooks.ps1: edit the source, not this copy.`n" + $src
    if (Write-Text $dst $src) { $changes++; "adapter copied: $($cfg.adapter.target)" } else { "adapter unchanged" }
}
if ($Check) { "check: $changes file(s) would change" } else { "done: $changes file(s) changed" }
