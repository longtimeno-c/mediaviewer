# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The vcpkg baseline is one pin written in five places (docs/design/09,
# Toolchain): the three manifests' builtin-baseline and the
# VCPKG_COMMIT the CI and release workflows check vcpkg out at. Moving it in
# one file and not the others builds the Mac, the portable core or a release
# from a different port set than the one CI tested. This fails when any of the
# five is missing or differs from vcpkg.json.
#
# Exit 0 clean, 1 on a mismatch.

[CmdletBinding()]
param(
    [string]$RepoRoot
)

$ErrorActionPreference = 'Stop'

if (-not $RepoRoot) {
    $RepoRoot = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..'
}
$RepoRoot = (Resolve-Path $RepoRoot).Path

$manifests = @('vcpkg.json', 'tools/mac/dependencies/vcpkg.json', 'tools/portable/vcpkg.json')
$workflows = @('.github/workflows/ci.yml', '.github/workflows/release.yml')

$pins = @()

foreach ($rel in $manifests) {
    $path = Join-Path $RepoRoot $rel
    $value = $null
    if (Test-Path -LiteralPath $path) {
        $json = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
        $value = $json.'builtin-baseline'
    }
    $pins += [pscustomobject]@{ File = $rel; Pin = $value }
}

foreach ($rel in $workflows) {
    $path = Join-Path $RepoRoot $rel
    $value = $null
    if (Test-Path -LiteralPath $path) {
        foreach ($line in (Get-Content -LiteralPath $path)) {
            if ($line -match '^\s*VCPKG_COMMIT:\s*([0-9a-fA-F]+)\s*$') {
                $value = $matches[1]
                break
            }
        }
    }
    $pins += [pscustomobject]@{ File = $rel; Pin = $value }
}

$expected = $pins[0].Pin
$bad = @($pins | Where-Object { -not $_.Pin -or $_.Pin -ne $expected })

if ($expected -and $bad.Count -eq 0) {
    Write-Host "vcpkg baseline: $expected in all $($pins.Count) pins"
    exit 0
}

Write-Host ''
Write-Host 'VCPKG BASELINE MISMATCH: every pin must name the same vcpkg commit.' -ForegroundColor Red
Write-Host ''
foreach ($p in $pins) {
    $shown = if ($p.Pin) { $p.Pin } else { '(missing)' }
    Write-Host ("  {0,-38} {1}" -f $p.File, $shown)
}
Write-Host ''
exit 1
