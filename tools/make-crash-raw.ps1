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
# Without -Source: synthesises a small uncompressed DNG-flavoured TIFF
# (DNGVersion tag, 8-bit RGB strips) whose pixels are a distinctive repeating
# pattern, so the dump scan can also look for pixel data. The marker sits in
# ImageDescription. The pixel patterns to scan for are printed.
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
$ms = New-Object IO.MemoryStream
$w = New-Object IO.BinaryWriter($ms)
$desc = [Text.Encoding]::ASCII.GetBytes('MV-DELIBERATE-CRASH synthetic canary') + [byte]0
$pixelBytes = $Width * $Height * 3

$entries = @(
    # tag, type, count, value-or-offset placeholder
    @(254, 4, 1, 0),                 # NewSubfileType
    @(256, 4, 1, $Width),
    @(257, 4, 1, $Height),
    @(258, 3, 3, 'BPS'),             # BitsPerSample 8,8,8
    @(259, 3, 1, 1),                 # Compression none
    @(262, 3, 1, 2),                 # RGB
    @(270, 2, $desc.Length, 'DESC'), # ImageDescription (marker)
    @(273, 4, 1, 'PIX'),             # StripOffsets
    @(277, 3, 1, 3),                 # SamplesPerPixel
    @(278, 4, 1, $Height),           # RowsPerStrip
    @(279, 4, 1, $pixelBytes),       # StripByteCounts
    @(50706, 1, 4, 0x00040101)       # DNGVersion 1.4.0.0 (bytes 1,4,0,0)
)
$ifdOffset = 8
$ifdSize = 2 + 12 * $entries.Count + 4
$bpsOffset = $ifdOffset + $ifdSize
$descOffset = $bpsOffset + 6
$pixOffset = $descOffset + $desc.Length
if ($pixOffset % 2) { $pixOffset++ }

$w.Write([byte[]](0x49, 0x49, 42, 0)); $w.Write([uint32]$ifdOffset)
$w.Write([uint16]$entries.Count)
foreach ($e in $entries) {
    $w.Write([uint16]$e[0]); $w.Write([uint16]$e[1]); $w.Write([uint32]$e[2])
    switch ($e[3]) {
        'BPS'  { $w.Write([uint32]$bpsOffset) }
        'DESC' { $w.Write([uint32]$descOffset) }
        'PIX'  { $w.Write([uint32]$pixOffset) }
        default {
            if ($e[1] -eq 3 -and $e[2] -eq 1) { $w.Write([uint16]$e[3]); $w.Write([uint16]0) }
            elseif ($e[0] -eq 50706) { $w.Write([byte[]](1, 4, 0, 0)) }
            else { $w.Write([uint32]$e[3]) }
        }
    }
}
$w.Write([uint32]0)
$w.Write([uint16]8); $w.Write([uint16]8); $w.Write([uint16]8)
$w.Write($desc)
while ($ms.Position -lt $pixOffset) { $w.Write([byte]0) }
$row = New-Object byte[] ($Width * 3)
for ($x = 0; $x -lt $Width; $x++) {
    $p = ($x % 8) * 3
    $row[$x * 3] = $palette[$p]; $row[$x * 3 + 1] = $palette[$p + 1]; $row[$x * 3 + 2] = $palette[$p + 2]
}
for ($y = 0; $y -lt $Height; $y++) { $w.Write($row) }
$w.Flush()
[IO.File]::WriteAllBytes($out, $ms.ToArray())

$rgb = ($palette | ForEach-Object { '{0:X2}' -f $_ }) -join ''
$rgba = (0..7 | ForEach-Object { ($palette[$_*3..($_*3+2)] | ForEach-Object { '{0:X2}' -f $_ }) -join '' } |
    ForEach-Object { $_ + 'FF' }) -join ''
Write-Host "wrote $out ($Width x $Height synthetic DNG, marker in ImageDescription)"
Write-Host "pixel pattern RGB : $rgb"
Write-Host "pixel pattern RGBA: $rgba"
