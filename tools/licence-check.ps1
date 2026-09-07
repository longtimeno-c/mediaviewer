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
# plan/11: "The configure line is checked directly rather than trusted."
#
# Scope matters as much as the check. This must look ONLY at the FFmpeg this
# repo links, never at whatever ffmpeg happens to be on PATH -- a developer with
# a GPL ffmpeg in PATH (a normal thing to have) must not fail an unrelated
# build, and a GPL ffmpeg in a classic vcpkg root must not be mistaken for ours.
# So: the explicit parameter, else the repo's own manifest-mode install tree.
# Never $env:VCPKG_ROOT/installed, never PATH.
if (-not $VcpkgInstalledRoot) {
    $candidates = Get-ChildItem -Path $RepoRoot -Directory -Filter 'build*' -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName 'vcpkg_installed' } |
        Where-Object { Test-Path $_ }
    if ($candidates) { $VcpkgInstalledRoot = @($candidates)[0] }
}

if ($VcpkgInstalledRoot -and (Test-Path $VcpkgInstalledRoot)) {
    # FFmpeg embeds its full configure string in the built libraries, so the
    # binary we actually link is its own evidence. This survives vcpkg cleaning
    # buildtrees, which the old log-file check did not -- that check passed
    # vacuously because nothing it looked for is ever installed.
    $ffmpegBinaries = Get-ChildItem -Path $VcpkgInstalledRoot -Recurse -File `
        -Include 'avutil*.dll', 'avcodec*.dll', 'avformat*.dll' -ErrorAction SilentlyContinue

    $sawConfigureLine = $false
    foreach ($binary in $ffmpegBinaries) {
        $bytes = [System.IO.File]::ReadAllBytes($binary.FullName)
        $text  = [System.Text.Encoding]::ASCII.GetString($bytes)
        if ($text -notmatch '--toolchain|--prefix|--enable-') { continue }
        $sawConfigureLine = $true

        if ($text -match '--enable-gpl') {
            Add-Violation 'FFmpeg configured GPL' `
                "$($binary.Name) was built with --enable-gpl. plan/11: LGPL only, and --enable-gpl pulls in x264/x265."
        }
        if ($text -match '--enable-nonfree') {
            Add-Violation 'FFmpeg configured nonfree' `
                "$($binary.Name) was built with --enable-nonfree."
        }
        foreach ($encoder in @('--enable-libx264', '--enable-libx265', '--enable-libfdk-aac')) {
            if ($text -match [regex]::Escape($encoder)) {
                Add-Violation 'forbidden encoder in FFmpeg' `
                    "$($binary.Name) was built with $encoder. plan/11: never bundle a software HEVC or AAC encoder."
            }
        }
    }

    # An FFmpeg present but unreadable is a silent pass, which is the failure
    # mode this whole file exists to prevent. Say so rather than exit 0.
    if ($ffmpegBinaries -and -not $sawConfigureLine) {
        Add-Violation 'FFmpeg configure line not found' `
            'FFmpeg libraries are installed but carry no readable configure string; the LGPL gate could not be evaluated.'
    }

    # Also honour any text form, if a future port installs one.
    $configFiles = Get-ChildItem -Path $VcpkgInstalledRoot -Recurse -File `
        -Include 'FFMPEG_CONFIGURE*', 'ffmpeg-config*' -ErrorAction SilentlyContinue |
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
        # An import library beside a DLL is normal and correct; what plan/11
        # forbids is a .lib with NO .dll, which means the component was linked
        # statically and the user cannot substitute their own build.
        $staticLibs = Get-ChildItem -Path $VcpkgInstalledRoot -Recurse -File -Filter "*$lgpl*.lib" `
            -ErrorAction SilentlyContinue
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
