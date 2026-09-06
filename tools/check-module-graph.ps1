# SPDX-License-Identifier: GPL-2.0-or-later
#
# Enforces the module dependency direction from plan/02-architecture.md:
#
#   shell -> abi -> {canvas, edit, player, image, meta} -> {codec, gfx, io} -> core
#
# "No back-edges, and no native module may depend on shell. Enforce with a CI
# script that greps includes; it takes 20 lines and saves the project."
#
# That rule is what keeps the core testable headlessly and re-hostable. A single
# `#include "shell/..."` from image/ is enough to make the decoders untestable
# without a window, and it will be added by accident, in a hurry, by someone who
# just needs one thing.
#
# Exit 0 clean, 1 on a violation.

[CmdletBinding()]
param(
    # Resolved in the body, not in the default: Windows PowerShell 5.1 does not
    # reliably populate $PSScriptRoot while binding parameter defaults, and
    # three-argument Join-Path is PowerShell 6+.
    [string]$SourceRoot
)

$ErrorActionPreference = 'Stop'

if (-not $SourceRoot) {
    $SourceRoot = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\src'
}

# Each module may include from itself and from anything to its right.
$allowed = [ordered]@{
    'core'   = @('core')
    'io'     = @('io', 'core')
    'gfx'    = @('gfx', 'core')
    'codec'  = @('codec', 'gfx', 'io', 'core')
    'image'  = @('image', 'codec', 'gfx', 'io', 'core')
    'meta'   = @('meta', 'codec', 'gfx', 'io', 'core')
    'player' = @('player', 'codec', 'gfx', 'io', 'core')
    'edit'   = @('edit', 'image', 'codec', 'gfx', 'io', 'core')
    'canvas' = @('canvas', 'image', 'gfx', 'io', 'core')
    'abi'    = @('abi', 'canvas', 'edit', 'player', 'image', 'meta', 'codec', 'gfx', 'io', 'core')
    'shell'  = @('shell', 'abi', 'canvas', 'edit', 'player', 'image', 'meta', 'codec', 'gfx', 'io', 'core')
}

$SourceRoot = (Resolve-Path $SourceRoot).Path
$violations = @()

foreach ($module in $allowed.Keys) {
    $moduleDir = Join-Path $SourceRoot $module
    if (-not (Test-Path $moduleDir)) { continue }

    $files = Get-ChildItem -Path $moduleDir -Recurse -Include *.h, *.hpp, *.cpp -File
    foreach ($file in $files) {
        $lineNumber = 0
        foreach ($line in (Get-Content -LiteralPath $file.FullName)) {
            $lineNumber++
            # Only project includes; angle-bracket includes are system or vcpkg.
            if ($line -notmatch '^\s*#\s*include\s+"([^"]+)"') { continue }
            $included = $matches[1]
            if ($included -notmatch '^([a-z]+)/') { continue }

            $target = $matches[1]
            if (-not $allowed.Contains($target)) { continue }

            if ($allowed[$module] -notcontains $target) {
                $violations += [pscustomobject]@{
                    File   = $file.FullName.Substring($SourceRoot.Length + 1)
                    Line   = $lineNumber
                    From   = $module
                    To     = $target
                    Text   = $line.Trim()
                }
            }
        }
    }
}

if ($violations.Count -eq 0) {
    Write-Host "module graph: clean ($($allowed.Keys.Count) modules checked)"
    exit 0
}

Write-Host ''
Write-Host 'MODULE GRAPH VIOLATIONS — dependencies must point downward only.' -ForegroundColor Red
Write-Host 'plan/02-architecture.md: shell -> abi -> {canvas,edit,player,image,meta} -> {codec,gfx,io} -> core'
Write-Host ''
foreach ($v in $violations) {
    Write-Host ("  {0}:{1}" -f $v.File, $v.Line) -ForegroundColor Red
    Write-Host ("    {0}/ must not include {1}/ : {2}" -f $v.From, $v.To, $v.Text)
}
Write-Host ''
exit 1
