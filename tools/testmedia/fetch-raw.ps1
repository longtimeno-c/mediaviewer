# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetch the PR 7 RAW sample set into tools/testmedia/raw/ and verify it.
#
# plan/09: the corpus does not live in git. The manifest beside this script —
# raw-manifest.json, one (url, sha256, licence) row per file — does. Every
# sample is from raw.pixls.us and CC0-1.0 there (checked per file when the
# manifest was written; the repository marks non-CC0 samples separately).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/testmedia/fetch-raw.ps1
#
# Re-running is cheap: a file already present with the right sha256 is kept.
# A present file with the wrong hash is re-downloaded, never trusted.
# tests/test_raw.cpp SKIPs visibly without these (MV_REQUIRE_CORPUS=1 fails).
[CmdletBinding()]
param(
    [string]$Manifest = '',
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 leaves $PSScriptRoot empty inside param() defaults.
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Manifest) { $Manifest = Join-Path $here 'raw-manifest.json' }
if (-not $OutDir) { $OutDir = Join-Path $here 'raw' }
$ProgressPreference = 'SilentlyContinue'  # Invoke-WebRequest's bar is 10x slower
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$entries = Get-Content -Raw -LiteralPath $Manifest | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$failed = 0
foreach ($e in $entries) {
    if ($e.licence -ne 'CC0-1.0') {
        Write-Error "$($e.file): licence '$($e.licence)' is not CC0-1.0; refusing"
    }
    $dest = Join-Path $OutDir $e.file
    if (Test-Path -LiteralPath $dest) {
        $have = (Get-FileHash -Algorithm SHA256 -LiteralPath $dest).Hash.ToLowerInvariant()
        if ($have -eq $e.sha256) {
            Write-Host "ok      $($e.file)"
            continue
        }
        Write-Host "stale   $($e.file) (sha256 $have) - re-downloading"
    }
    $tmp = "$dest.part"
    Write-Host "fetch   $($e.file)  ($($e.size_mb) MB, $($e.make) $($e.model))"
    try {
        Invoke-WebRequest -UseBasicParsing -Uri $e.url -OutFile $tmp
    } catch {
        Write-Warning "$($e.file): download failed: $($_.Exception.Message)"
        Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $tmp
        $failed++
        continue
    }
    $got = (Get-FileHash -Algorithm SHA256 -LiteralPath $tmp).Hash.ToLowerInvariant()
    if ($got -ne $e.sha256) {
        Write-Warning "$($e.file): sha256 mismatch (got $got, want $($e.sha256))"
        Remove-Item -Force -LiteralPath $tmp
        $failed++
        continue
    }
    Move-Item -Force -LiteralPath $tmp -Destination $dest
    Write-Host "ok      $($e.file)"
}

if ($failed -gt 0) {
    Write-Error "$failed RAW sample(s) missing or unverified"
    exit 1
}
Write-Host "RAW samples verified in $OutDir"
