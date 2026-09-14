# SPDX-License-Identifier: GPL-2.0-or-later
# Downloads the real-world HEIC samples listed in heif-manifest.json into
# tools/testmedia/ (gitignored; plan/09: media is not committed) and verifies
# each SHA-256. A mismatch deletes the file and fails — a sample that changed
# upstream is not the sample the tests were written against.
#
#   powershell -ExecutionPolicy Bypass -File tools/testmedia/fetch-heif.ps1
#
# The tests that use these files SKIP visibly when they are absent, and FAIL
# under MV_REQUIRE_CORPUS=1.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$manifest = Get-Content -Raw -Path (Join-Path $here 'heif-manifest.json') | ConvertFrom-Json
$failed = 0
foreach ($f in $manifest.files) {
  $dest = Join-Path $here $f.dest
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
  if (Test-Path $dest) {
    $have = (Get-FileHash -Algorithm SHA256 -Path $dest).Hash.ToLowerInvariant()
    if ($have -eq $f.sha256) { Write-Host "ok (cached)  $($f.dest)  [$($f.licence)]"; continue }
  }
  Write-Host "fetch        $($f.dest)  <- $($f.url)"
  Invoke-WebRequest -UseBasicParsing -Uri $f.url -OutFile $dest
  $got = (Get-FileHash -Algorithm SHA256 -Path $dest).Hash.ToLowerInvariant()
  if ($got -ne $f.sha256) {
    Remove-Item -Force $dest
    Write-Error "sha256 mismatch for $($f.dest): expected $($f.sha256), got $got" -ErrorAction Continue
    $failed++
  } else {
    Write-Host "ok           $($f.dest)  [$($f.licence)]"
  }
}
foreach ($m in $manifest.missing) { Write-Warning $m }
if ($failed -gt 0) { exit 1 }
