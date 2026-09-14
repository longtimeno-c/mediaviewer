# SPDX-License-Identifier: GPL-2.0-or-later
#
# Makes the "deliberately-corrupted RAW" for PR 7's crash-report verify
# (plan/10: "a deliberately-corrupted RAW produces a minidump containing no
# path, filename, or pixel data").
#
# Writes a COPY — never touches the original (rule 5) — named so a leak is
# unmistakable in a byte scan:
#   <OutRoot>\PRIVATE_FOLDER_canary\SECRET_FILENAME_canary_7Q3.dng
#
# With -Source: copies that file and overwrites bytes at offset 0x1000 (or the
# end, for a tiny file) with the ASCII marker MV-DELIBERATE-CRASH, which
# codec/crash_test_hook.cpp looks for in the first 64 KB. That is a corrupted
# camera file in the ordinary sense.
#
# Without -Source: the pixel-data check. Writes two uncompressed BMPs whose
# pixels are a distinctive repeating pattern (probe is by magic bytes, so the
# .dng name does not matter):
#   SECRET_COMPANION_canary.bmp        no marker: opens, decodes to RGBA, stays live
#   SECRET_FILENAME_canary_7Q3.dng     marker in its first row: crashes on decode
# Open the companion; neighbour prefetch decodes the canary and crashes while
# the companion's decoded pixels are in process memory. The byte patterns to
# scan for (file BGR and decoded RGBA) are printed.
#
# The hook is inert unless MV_CRASH_TEST=decode is set in the app's
# environment. Exit 0 on success.

[CmdletBinding()]
param(
    [string]$Source,
    [string]$OutRoot = (Join-Path $env:LOCALAPPDATA 'Temp\mv-crash-canary'),
    [int]$Width = 256,
    [int]$Height = 256
)

$ErrorActionPreference = 'Stop'
$marker = [Text.Encoding]::ASCII.GetBytes('MV-DELIBERATE-CRASH')
$folder = Join-Path $OutRoot 'PRIVATE_FOLDER_canary'
$out = Join-Path $folder 'SECRET_FILENAME_canary_7Q3.dng'
New-Item -ItemType Directory -Force -Path $folder | Out-Null

# Eight distinctive RGB triples; the RGBA form (A=255) is what a decoder holds.
$palette = @(0xC3,0x5A,0x17, 0x7E,0xE1,0x29, 0x3B,0x94,0xD6, 0xF0,0x0D,0xB8,
             0x62,0x2F,0xAC, 0xD9,0x71,0x44, 0x1C,0xCB,0x8E, 0xA7,0x36,0xF5)

if ($Source) {
    $src = (Resolve-Path -LiteralPath $Source).Path
    if ([IO.Path]::GetFullPath($src) -ieq [IO.Path]::GetFullPath($out)) {
        throw 'refusing to overwrite the source file'
    }
    $bytes = [IO.File]::ReadAllBytes($src)
    $offset = [Math]::Min(0x1000, [Math]::Max(0, $bytes.Length - $marker.Length))
    if ($bytes.Length -lt $marker.Length) { $bytes = New-Object byte[] $marker.Length; $offset = 0 }
    [Array]::Copy($marker, 0, $bytes, $offset, $marker.Length)
    [IO.File]::WriteAllBytes($out, $bytes)
    Write-Host "wrote $out (copy of source, marker at 0x$('{0:X}' -f $offset))"
    exit 0
}


# --- synthesise ---------------------------------------------------------------
function New-PatternBmp([int]$w, [int]$h, [bool]$withMarker) {
    $stride = (($w * 3) + 3) -band -4
    $pixOff = 54
    $size = $pixOff + $stride * $h
    $b = New-Object byte[] $size
    $b[0] = 0x42; $b[1] = 0x4D
    [BitConverter]::GetBytes([uint32]$size).CopyTo($b, 2)
    [BitConverter]::GetBytes([uint32]$pixOff).CopyTo($b, 10)
    [BitConverter]::GetBytes([uint32]40).CopyTo($b, 14)
    [BitConverter]::GetBytes([int32]$w).CopyTo($b, 18)
    [BitConverter]::GetBytes([int32]$h).CopyTo($b, 22)
    [BitConverter]::GetBytes([uint16]1).CopyTo($b, 26)
    [BitConverter]::GetBytes([uint16]24).CopyTo($b, 28)
    for ($y = 0; $y -lt $h; $y++) {
        $row = $pixOff + $y * $stride
        for ($x = 0; $x -lt $w; $x++) {
            $p = ($x % 8) * 3
            $b[$row + $x * 3] = $palette[$p + 2]      # BMP stores B,G,R
            $b[$row + $x * 3 + 1] = $palette[$p + 1]
            $b[$row + $x * 3 + 2] = $palette[$p]
        }
    }
    if ($withMarker) { [Array]::Copy($marker, 0, $b, $pixOff, $marker.Length) }
    return , $b
}

[IO.File]::WriteAllBytes((Join-Path $folder 'SECRET_COMPANION_canary.bmp'), (New-PatternBmp $Width $Height $false))
[IO.File]::WriteAllBytes($out, (New-PatternBmp $Width $Height $true))

$bgr = ''; $rgba = ''
for ($i = 0; $i -lt 8; $i++) {
    $bgr += '{2:X2}{1:X2}{0:X2}' -f $palette[3 * $i], $palette[3 * $i + 1], $palette[3 * $i + 2]
    $rgba += '{0:X2}{1:X2}{2:X2}FF' -f $palette[3 * $i], $palette[3 * $i + 1], $palette[3 * $i + 2]
}
Write-Host "wrote $out and SECRET_COMPANION_canary.bmp ($Width x $Height, BMP bytes)"
Write-Host "pixel pattern file BGR   : $bgr"
Write-Host "pixel pattern decoded RGBA: $rgba"
Write-Host "open the companion (MV_CRASH_TEST=decode); prefetch crashes on the canary"
