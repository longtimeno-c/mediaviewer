# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Enforces the module dependency direction from docs/design/02-architecture.md:
#
#   shell -> abi -> {canvas, edit, player, image, meta, addon} -> {codec, gfx, io} -> core
#   addons -> infer -> core   (the AI pack; add-ons reach the app only through
#                             the host function table)
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
    # codec does not include io. Document font bytes are a reader image/
    # installs (text::set_font_reader); fonts_*.cpp only names the file.
    'codec'  = @('codec', 'gfx', 'core')
    'image'  = @('image', 'codec', 'gfx', 'io', 'core')
    'meta'   = @('meta', 'codec', 'gfx', 'io', 'core')
    'player' = @('player', 'codec', 'gfx', 'io', 'core')
    'edit'   = @('edit', 'image', 'codec', 'gfx', 'io', 'core')
    'canvas' = @('canvas', 'image', 'gfx', 'io', 'core')
    # Milestone G (docs/design/18). addon: the add-on host in the base app, beside
    # meta/image. addons: the add-ons themselves, which reach the core only
    # through the host function table and may include only core's
    # header-only pieces (result.h, status.h, json.h) and themselves.
    'addon'  = @('addon', 'io', 'core')
    # docs/design/06 "Place": the offline reverse geocoder. geo.cpp/fold.h use
    # only core's header-only pieces so the Places add-on can link them; open.cpp
    # maps a file through io/. Nothing in the app's own graph (meta, abi, shell)
    # may include it: places arrive through the Places add-on, never hard-wired.
    'geo'    = @('geo', 'io', 'core')
    # docs/design/17: infer/ is ONNX Runtime behind our own interface, linked
    # into the AI pack only. It needs nothing but core; the AI add-on
    # (addons/ai) is its one consumer.
    'infer'  = @('infer', 'core')
    # 2026-10-15 (#410): io too. Sharepoint writes what it receives through the
    # file port's exclusive create, durable flush and no-replace rename (the
    # rules of io::verified_copy), compiled into its own module as Places
    # compiles io/mapped_file: still nothing of the core is linked, and geo,
    # which add-ons already use, sits on io.
    # 2026-10-18 (#485): nle too. The AI agents add-on edits FCPXML with the
    # timeline model (nle/timeline, nle/edit: portable, compiled into its own
    # module as mv_nle_timeline); nothing of the core is linked. The Final Cut
    # Pro add-on (docs/design/23) compiles nle's search wire into its module.
    'addons' = @('addons', 'geo', 'infer', 'io', 'nle', 'core')
    'abi'    = @('abi', 'addon', 'canvas', 'edit', 'player', 'image', 'meta', 'codec', 'gfx', 'io', 'core')
    # docs/design/23: the Mac app's main() hands `--search-agent` to the Final Cut
    # Pro add-on's agent, lending it nle's search_session.
    'shell'  = @('shell', 'abi', 'addon', 'nle', 'canvas', 'edit', 'player', 'image', 'meta', 'codec', 'gfx', 'io', 'core')
    # PR 15: Explorer's host (the thumbnail handler DLL). A sibling of shell,
    # never included by it: it reaches the core through image/ and codec/.
    'shellext' = @('shellext', 'image', 'codec', 'gfx', 'io', 'core')
    # docs/design/23: the Local search agent and FCPXML export, a host of the AI pack:
    # it loads the pack through addon/ and reads the viewer's thumbnail cache
    # (image/thumb.h's key). Below shell (the Mac app's executable also hosts
    # the agent, entered from main()); it never includes shell.
    'nle'    = @('nle', 'addon', 'image', 'io', 'core')
}

$SourceRoot = (Resolve-Path $SourceRoot).Path
$violations = @()

foreach ($module in $allowed.Keys) {
    $moduleDir = Join-Path $SourceRoot $module
    if (-not (Test-Path $moduleDir)) { continue }

    # .mm / .m: the Objective-C++ ports (image/upload_mac.mm, player/*_mac.mm,
    # nle/mac/*) include project headers too, often with #import.
    $files = Get-ChildItem -Path $moduleDir -Recurse -Include *.h, *.hpp, *.cpp, *.mm, *.m -File
    foreach ($file in $files) {
        $lineNumber = 0
        foreach ($line in (Get-Content -LiteralPath $file.FullName)) {
            $lineNumber++
            # Only project includes; angle-bracket includes are system or vcpkg.
            if ($line -notmatch '^\s*#\s*(?:include|import)\s+"([^"]+)"') { continue }
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
Write-Host 'docs/design/02-architecture.md: shell -> abi -> {canvas,edit,player,image,meta} -> {codec,gfx,io} -> core'
Write-Host ''
foreach ($v in $violations) {
    Write-Host ("  {0}:{1}" -f $v.File, $v.Line) -ForegroundColor Red
    Write-Host ("    {0}/ must not include {1}/ : {2}" -f $v.From, $v.To, $v.Text)
}
Write-Host ''
exit 1
