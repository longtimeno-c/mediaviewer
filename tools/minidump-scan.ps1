# SPDX-License-Identifier: GPL-2.0-or-later
#
# Scans a minidump (or any file) for things that must never leave the machine
# (rule 6, plan/13 Part 2): paths, filenames, folder names, the username, and
# pixel byte runs.
#
#   tools/minidump-scan.ps1 -Dump x.dmp `
#       -Forbidden 'C:\...\SECRET_FILENAME_canary_7Q3.dng','SECRET_FILENAME_canary_7Q3','PRIVATE_FOLDER_canary',$env:USERNAME `
#       -PixelHex 'C35A177EE1293B94D6F00DB8622FACD971441CCB8EA736F5'
#
# Strings are searched as UTF-8 and UTF-16LE, ASCII-case-insensitively. Pixel
# patterns are exact byte runs. Each hit is reported with its file offset and,
# for a minidump, the stream or memory range it falls in.
#
# Exit 0 = PASS (no hits), 1 = FAIL (hits), 2 = usage / unreadable file.

[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Dump,
    [string[]]$Forbidden = @(),
    [string[]]$PixelHex = @(),
    [int]$MaxHitsPerNeedle = 10
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Dump)) { Write-Host "no such file: $Dump"; exit 2 }
$bytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Dump).Path)
# Latin-1 maps each byte to one char, so string search == byte search.
$latin1 = [Text.Encoding]::GetEncoding(28591)
$hay = $latin1.GetString($bytes)

# --- minidump map (best effort) ------------------------------------------------
$regions = New-Object System.Collections.Generic.List[object]
function Add-Region([long]$start, [long]$size, [string]$label) {
    if ($size -gt 0 -and $start -ge 0 -and $start + $size -le $bytes.Length) {
        $regions.Add([pscustomobject]@{ Start = $start; End = $start + $size; Label = $label })
    }
}
$streamNames = @{ 3 = 'ThreadList'; 4 = 'ModuleList'; 5 = 'MemoryList'; 6 = 'Exception';
                  7 = 'SystemInfo'; 9 = 'Memory64List'; 12 = 'HandleData'; 14 = 'UnloadedModuleList';
                  15 = 'MiscInfo'; 16 = 'MemoryInfoList'; 0x43500001 = 'CrashpadInfo' }
$isDump = $bytes.Length -ge 32 -and [BitConverter]::ToUInt32($bytes, 0) -eq 0x504D444D
if ($isDump) {
    $n = [BitConverter]::ToUInt32($bytes, 8); $dir = [BitConverter]::ToUInt32($bytes, 12)
    for ($i = 0; $i -lt $n; $i++) {
        $e = $dir + 12 * $i
        if ($e + 12 -gt $bytes.Length) { break }
        $type = [BitConverter]::ToUInt32($bytes, $e)
        $size = [BitConverter]::ToUInt32($bytes, $e + 4)
        $rva = [BitConverter]::ToUInt32($bytes, $e + 8)
        $name = if ($streamNames.ContainsKey([int64]$type)) { $streamNames[[int64]$type] } else { 'stream 0x{0:X}' -f $type }
        if ($type -eq 5) {
            $count = [BitConverter]::ToUInt32($bytes, $rva)
            for ($m = 0; $m -lt $count; $m++) {
                $d = $rva + 4 + 16 * $m
                $va = [BitConverter]::ToUInt64($bytes, $d)
                Add-Region ([BitConverter]::ToUInt32($bytes, $d + 12)) ([BitConverter]::ToUInt32($bytes, $d + 8)) ('memory VA 0x{0:X}' -f $va)
            }
        } elseif ($type -eq 4) {
            Add-Region $rva $size $name
            $count = [BitConverter]::ToUInt32($bytes, $rva)
            for ($m = 0; $m -lt $count; $m++) {
                $mod = $rva + 4 + 108 * $m
                $nameRva = [BitConverter]::ToUInt32($bytes, $mod + 20)
                Add-Region $nameRva ([BitConverter]::ToUInt32($bytes, $nameRva) + 4) 'module name'
                Add-Region ([BitConverter]::ToUInt32($bytes, $mod + 28)) ([BitConverter]::ToUInt32($bytes, $mod + 24)) 'module CodeView'
            }
        } else {
            Add-Region $rva $size $name
        }
    }
    $checksum = [BitConverter]::ToUInt32($bytes, 16)
    Write-Host ("minidump: {0} bytes, {1} streams, scrubbed marker: {2}" -f $bytes.Length, $n, ($checksum -eq 0x4353564D))
}
function Where-Offset([long]$o) {
    $best = $null
    foreach ($r in $regions) {
        if ($o -ge $r.Start -and $o -lt $r.End) {
            if (-not $best -or ($r.End - $r.Start) -lt ($best.End - $best.Start)) { $best = $r }
        }
    }
    if ($best) { $best.Label } else { 'unmapped' }
}

# --- search ------------------------------------------------------------------
$hits = 0
function Search([string]$needle, [string]$label, [System.StringComparison]$cmp) {
    if ($needle.Length -eq 0) { return }
    $at = 0; $found = 0
    while (($at = $hay.IndexOf($needle, $at, $cmp)) -ge 0) {
        $script:hits++; $found++
        if ($found -le $MaxHitsPerNeedle) {
            Write-Host ("  HIT {0} at 0x{1:X} ({2})" -f $label, $at, (Where-Offset $at)) -ForegroundColor Red
        }
        $at += [Math]::Max(1, $needle.Length)
    }
    if ($found -gt $MaxHitsPerNeedle) { Write-Host "  ... $found hits total for $label" -ForegroundColor Red }
}

$ci = [StringComparison]::OrdinalIgnoreCase
foreach ($f in $Forbidden) {
    if (-not $f) { continue }
    Search ($latin1.GetString([Text.Encoding]::UTF8.GetBytes($f))) "'$f' (UTF-8)" $ci
    Search ($latin1.GetString([Text.Encoding]::Unicode.GetBytes($f))) "'$f' (UTF-16LE)" $ci
}
foreach ($hex in $PixelHex) {
    $clean = ($hex -replace '[^0-9A-Fa-f]', '')
    if ($clean.Length -lt 2 -or $clean.Length % 2) { Write-Host "bad hex: $hex"; exit 2 }
    $pat = New-Object byte[] ($clean.Length / 2)
    for ($i = 0; $i -lt $pat.Length; $i++) { $pat[$i] = [Convert]::ToByte($clean.Substring(2 * $i, 2), 16) }
    Search ($latin1.GetString($pat)) "pixel run $($clean.Substring(0, [Math]::Min(16, $clean.Length)))..." ([StringComparison]::Ordinal)
}

if ($hits -eq 0) {
    Write-Host ("PASS: {0} forbidden string(s) x 2 encodings, {1} pixel pattern(s): no hits" -f $Forbidden.Count, $PixelHex.Count) -ForegroundColor Green
    exit 0
}
Write-Host "FAIL: $hits hit(s)" -ForegroundColor Red
exit 1
