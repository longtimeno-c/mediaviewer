# SPDX-License-Identifier: GPL-2.0-or-later
#
# The licence gate from plan/11-licensing.md's PR 1 checklist.
#
# MediaViewer is GPL-2.0-or-later, which makes Exiv2, FFmpeg, libheif, libde265
# and LibRaw all compliant. It does NOT make everything permissible: two
# categories are still forbidden, for reasons that have nothing to do with our
# own licence.
#
#   1. Patent exposure. A software HEVC or AAC ENCODER is a pool-enforced
#      liability (Access Advance, Via LA). Decode is what users need; encode is
#      delegated to the GPU or Media Foundation. x264, x265 and fdk-aac must
#      never appear in the graph.
#
#   2. FFmpeg built with --enable-gpl pulls exactly those encoders in. The
#      configure line is checked directly rather than trusted.
#
# It also enforces the LGPL dynamic-linkage rule, which survives our GPL choice
# because it is about the user's ability to substitute their own build.
#
# Exit 0 clean, 1 on a violation.

[CmdletBinding()]
param(
    # Resolved in the body, not in the default: Windows PowerShell 5.1 does not
    # reliably populate $PSScriptRoot while binding parameter defaults.
    [string]$RepoRoot,
    [string]$VcpkgInstalledRoot = ''
)

$ErrorActionPreference = 'Stop'

if (-not $RepoRoot) {
    $RepoRoot = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..'
}
$RepoRoot = (Resolve-Path $RepoRoot).Path
$violations = @()

function Add-Violation([string]$rule, [string]$detail) {
    $script:violations += [pscustomobject]@{ Rule = $rule; Detail = $detail }
}

# --- 1. Forbidden encoders anywhere in the manifest ------------------------
$manifestPath = Join-Path $RepoRoot 'vcpkg.json'
if (Test-Path $manifestPath) {
    $manifest = Get-Content -Raw -LiteralPath $manifestPath

    # Only the real dependency list matters; the $comment-* keys deliberately
    # NAME these things in order to forbid them, and flagging our own
    # documentation would be the fastest way to get this check disabled.
    $parsed = $manifest | ConvertFrom-Json
    $declared = @()
    foreach ($dep in $parsed.dependencies) {
        $declared += if ($dep -is [string]) { $dep } else { $dep.name }
    }

    foreach ($forbidden in @('x264', 'x265', 'fdk-aac', 'libbluray')) {
        if ($declared -contains $forbidden) {
            Add-Violation 'forbidden encoder / GPL-only port' `
                "vcpkg.json declares '$forbidden'. plan/11: never bundle a software HEVC or AAC encoder."
        }
    }

    foreach ($dep in $declared) {
        if ($dep -like 'libraw-demosaic-pack*') {
            Add-Violation 'LibRaw GPL demosaic pack' `
                "vcpkg.json declares '$dep'. These packs are GPL-2/3 and are not used."
        }
        if ($dep -match 'ffmpeg' -and $dep -match 'gpl|nonfree') {
            Add-Violation 'FFmpeg feature' `
                "vcpkg.json declares '$dep'. FFmpeg is LGPL-only: no gpl, no nonfree features."
        }
    }
}

# --- 2. FFmpeg configure line, as actually built ---------------------------
# vcpkg records the configure line in the port's build log and in FFMPEG's own
# ffmpeg_version / config. Check whatever is present rather than assuming.
if (-not $VcpkgInstalledRoot -and $env:VCPKG_ROOT) {
    $VcpkgInstalledRoot = Join-Path $env:VCPKG_ROOT 'installed'
}
if ($VcpkgInstalledRoot -and (Test-Path $VcpkgInstalledRoot)) {
    $configFiles = Get-ChildItem -Path $VcpkgInstalledRoot -Recurse -File `
        -Include 'FFMPEG_CONFIGURE*', 'ffmpeg-config*', 'config.h' -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match 'ffmpeg' }

    foreach ($file in $configFiles) {
        $content = Get-Content -Raw -LiteralPath $file.FullName
        if ($content -match '--enable-gpl') {
            Add-Violation 'FFmpeg configured GPL' "$($file.FullName) carries --enable-gpl."
        }
        if ($content -match '--enable-nonfree') {
            Add-Violation 'FFmpeg configured nonfree' "$($file.FullName) carries --enable-nonfree."
        }
    }

    # --- 3. LGPL components must be DLLs, not static libs ------------------
    foreach ($lgpl in @('avcodec', 'avformat', 'avutil', 'swscale', 'swresample',
                        'heif', 'de265', 'raw', 'exiv2')) {
        $staticLibs = Get-ChildItem -Path $VcpkgInstalledRoot -Recurse -File -Filter "*$lgpl*.lib" `
            -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notmatch 'x64-windows\\' }
        $dlls = Get-ChildItem -Path $VcpkgInstalledRoot -Recurse -File -Filter "*$lgpl*.dll" `
            -ErrorAction SilentlyContinue

        if ($staticLibs -and -not $dlls) {
            Add-Violation 'LGPL component linked statically' `
                "$lgpl appears as a static library with no DLL. plan/11: LGPL requires the user be able to replace it."
        }
    }
}

# --- 4. Our own licence is declared and present ----------------------------
$licensePath = Join-Path $RepoRoot 'LICENSE'
if (-not (Test-Path $licensePath)) {
    Add-Violation 'missing LICENSE' 'The repository has no LICENSE file.'
} elseif ((Get-Content -Raw -LiteralPath $licensePath) -notmatch 'GNU GENERAL PUBLIC LICENSE') {
    Add-Violation 'unexpected LICENSE' 'LICENSE is not the GPL text the plan settled on.'
}

if (-not (Test-Path (Join-Path $RepoRoot 'THIRD-PARTY.md'))) {
    Add-Violation 'missing THIRD-PARTY.md' 'plan/11 requires a generated attribution file.'
}

# --- Report -----------------------------------------------------------------
if ($violations.Count -eq 0) {
    Write-Host 'licence check: clean'
    exit 0
}

Write-Host ''
Write-Host 'LICENCE VIOLATIONS' -ForegroundColor Red
Write-Host 'plan/11-licensing.md. These are not style preferences.'
Write-Host ''
foreach ($v in $violations) {
    Write-Host ("  [{0}]" -f $v.Rule) -ForegroundColor Red
    Write-Host ("    {0}" -f $v.Detail)
}
Write-Host ''
exit 1
