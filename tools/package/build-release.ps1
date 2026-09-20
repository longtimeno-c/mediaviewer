# SPDX-License-Identifier: GPL-2.0-or-later
#
# PR 8 release pipeline (plan/13 Part 1, plan/11).
#
#   payload  -> the files that go in a version folder
#   vpk pack -> the Velopack release set (.nupkg + delta + *-win-Setup.exe)
#   manifest -> mediaviewer-manifest.json, signed with the release Ed25519 key
#   ISCC     -> MediaViewer-<version>-Setup.exe, the first-install wizard
#
# Signing is a seam, not a fake. Every artefact is signed when -SigningMetadata
# (Azure Trusted Signing) or -SignParams (signtool) is given, and the script
# says loudly that it produced an UNSIGNED build when neither is. It never
# invents a certificate and never silently skips.
#
# Usage:
#   tools/package/build-release.ps1 -BuildDir build-pr8c            # unsigned local
#   tools/package/build-release.ps1 -BuildDir build -SigningMetadata trusted-signing.json `
#                                   -ManifestKey C:\offline\release.key
[CmdletBinding()]
param(
    # A configured CMake build directory whose Release config is already built.
    [string]$BuildDir = "build",
    [string]$Config = "Release",
    # Defaults to project(VERSION) in CMakeLists.txt.
    [string]$Version,
    # Where the release set and the wizard land.
    [string]$OutputDir,
    # Azure Trusted Signing metadata.json (plan/13 "Signing"). Signs the
    # payload binaries, the Velopack bundle and the wizard.
    [string]$SigningMetadata,
    # Alternative: raw signtool.exe parameters, for an EV cert or a test cert.
    [string]$SignParams,
    # The release Ed25519 PRIVATE key (hex, one line) that signs the update
    # manifest. Never in the repo, never on a build agent that runs PR code -
    # see update-signing.md.
    [string]$ManifestKey,
    # Versions withdrawn from the channel, comma separated.
    [string]$Blocklist = "",
    # Oldest version allowed to stay on the channel.
    [string]$MinVersion,
    [string]$Iscc,
    # Replace this version in the LOCAL release set. For iterating on the
    # packaging; never for a channel anyone has installed from.
    [switch]$Republish,
    # Skip the wizard (release set only), e.g. when iterating on the updater.
    [switch]$NoWizard
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$packId = "MediaViewer"
$channel = "win"

function Fail($message) { Write-Error $message; exit 1 }

# ---- version ---------------------------------------------------------------
if (-not $Version) {
    $m = [regex]::Match((Get-Content (Join-Path $repo "CMakeLists.txt") -Raw), '(?ms)^project\(mediaviewer\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
    if (-not $m.Success) { Fail "could not read project(VERSION) from CMakeLists.txt; pass -Version" }
    $Version = $m.Groups[1].Value
}
if (-not $MinVersion) { $MinVersion = $Version }
if (-not $OutputDir) { $OutputDir = Join-Path $repo "dist" }
$releases = Join-Path $OutputDir "releases"
$payload = Join-Path $OutputDir "payload-$Version"
Write-Host "MediaViewer $Version -> $OutputDir"

# ---- tools -----------------------------------------------------------------
if (-not (Get-Command vpk -ErrorAction SilentlyContinue)) {
    Fail "vpk not found. Install it: dotnet tool install -g vpk --version 1.2.0"
}
if (-not $NoWizard) {
    if (-not $Iscc) {
        $Iscc = @(
            "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
            "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
            "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
        ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    }
    if (-not $Iscc) { Fail "Inno Setup 6 (ISCC.exe) not found. winget install JRSoftware.InnoSetup, or pass -Iscc." }
}

# ---- payload ---------------------------------------------------------------
# What goes in a version folder: the host exe (renamed to the product name, so
# the Velopack stub and the taskbar agree), the chrome, every codec DLL, the
# crash handler, and the licence texts About links to. What does NOT: the test
# binaries, the frame-time harness, the fuzzers, and PDBs. plan/13 "Symbols":
# PDBs go to the symbol server, never in the payload.
$bin = Join-Path $repo "$BuildDir\bin\$Config"
if (-not (Test-Path $bin)) { Fail "no build output at $bin - configure and build $Config first" }
if (-not (Test-Path (Join-Path $bin "mediaviewer_lab.exe"))) { Fail "no mediaviewer_lab.exe in $bin" }
if (-not (Test-Path (Join-Path $bin "MediaViewer.Chrome.dll"))) { Fail "chrome not published into $bin (is dotnet on PATH?)" }

if (Test-Path $payload) { Remove-Item -Recurse -Force $payload }
New-Item -ItemType Directory -Force $payload, $releases | Out-Null

$skipFile = '^(mv_.*|.*_tests?|frametime.*|mediaviewer_lab\.exe)$'

# plan/13, "Delta patches earn their keep here": "Do not ship Windows App SDK
# AI / ONNX / DirectML / WebView2: they are not a dependency, and they are
# currently the largest files in a framework-dependent publish."
#
# They arrive anyway, because `dotnet publish` of a Windows App SDK project
# copies the whole framework's projection set whether the app touches it or
# not. Measured on this build: 43.4 MB of 119.6 MB - 36 % of every download,
# and of every delta that happens to touch them - for an image viewer that
# does no inference and hosts no browser.
$forbiddenBloat = '(?i)^(DirectML|onnxruntime|Microsoft\.ML\.OnnxRuntime|Microsoft\.Web\.WebView2|Microsoft\.Windows\.AI\.|Microsoft\.Graphics\.Imaging)'

Get-ChildItem $bin -File |
    Where-Object {
        $_.Extension -notin ".pdb", ".ilk", ".exp", ".lib" -and
        $_.Name -notmatch $skipFile -and
        $_.Name -notmatch $forbiddenBloat
    } |
    Copy-Item -Destination $payload
Get-ChildItem $bin -Directory |
    Where-Object { $_.Name -notmatch '^(tests?|frametime|fuzz)$' } |
    Copy-Item -Destination $payload -Recurse
Copy-Item (Join-Path $bin "mediaviewer_lab.exe") (Join-Path $payload "MediaViewer.exe")

foreach ($required in "MediaViewer.exe", "MediaViewer.Chrome.dll", "crashpad_handler.exe", "LICENSE", "THIRD-PARTY.md") {
    if (-not (Test-Path (Join-Path $payload $required))) { Fail "payload is missing $required" }
}
# The recursive copy above could reintroduce them from a subdirectory, so the
# ban is asserted on the finished tree rather than trusted to the filter.
$bloat = Get-ChildItem $payload -Recurse -File | Where-Object { $_.Name -match $forbiddenBloat }
if ($bloat) {
    Fail ("plan/13 forbids shipping these: " + (($bloat | Select-Object -Expand Name) -join ", "))
}

$size = [math]::Round((Get-ChildItem $payload -Recurse -File | Measure-Object Length -Sum).Sum / 1MB, 1)
Write-Host "payload: $size MB"
# plan/09 caps the installed size. A payload over it is a release problem, not
# a warning to scroll past.
if ($size -gt 250) { Fail "payload is $size MB; plan/09 caps the install at 250 MB" }

# ---- licence gate ----------------------------------------------------------
# plan/11 is enforced in the build, not in review: an --enable-gpl FFmpeg or a
# LibRaw GPL demosaic pack must never reach a release artefact.
# It reads the configure string out of the built FFmpeg DLLs in the vcpkg
# install tree those payload DLLs were copied from.
& (Join-Path $repo "tools\licence-check.ps1") -RepoRoot $repo
if ($LASTEXITCODE) { Fail "licence-check failed (plan/11)" }
# Belt and braces on the artefact itself: the forbidden encoders must not have
# reached the payload by any route.
$forbidden = Get-ChildItem $payload -Recurse -File |
    Where-Object { $_.Name -match '(?i)(x264|x265|fdk-aac|libfdk)' }
if ($forbidden) { Fail ("forbidden encoder in the payload: " + ($forbidden.Name -join ", ")) }

# ---- signing ---------------------------------------------------------------
# vpk signs the payload binaries and its own bundle with whichever of these it
# is given; ISCC signs the wizard through the same parameters.
$signArgs = @()
$signedBuild = $false
if ($SigningMetadata) {
    if (-not (Test-Path $SigningMetadata)) { Fail "Azure Trusted Signing metadata not found: $SigningMetadata" }
    $signArgs = @("--azureTrustedSignFile", (Resolve-Path $SigningMetadata).Path)
    $signedBuild = $true
} elseif ($SignParams) {
    $signArgs = @("--signParams", $SignParams)
    $signedBuild = $true
} else {
    Write-Warning "UNSIGNED BUILD. No -SigningMetadata and no -SignParams."
    Write-Warning "Every user of this artefact gets a SmartScreen block on first run."
    Write-Warning "This is a local build only. See tools/package/update-signing.md."
}

# ---- velopack release set --------------------------------------------------
$packArgs = @(
    "pack",
    "--packId", $packId,
    "--packTitle", "MediaViewer",
    "--packAuthors", "MediaViewer contributors",
    "--packVersion", $Version,
    "--packDir", $payload,
    "--mainExe", "MediaViewer.exe",
    "--icon", (Join-Path $repo "assets\icon\mediaviewer.ico"),
    "--channel", $channel,
    "--outputDir", $releases,
    # The wizard creates the shortcuts and owns the Apps & features entry.
    # Velopack must not create a second set.
    "--shortcuts", "None",
    "--exclude", ".*\.(pdb|ilk|exp)$"
) + $signArgs
# The release directory is the channel's history: vpk needs the previous
# versions there to build deltas, and it refuses to pack a version that is
# already in it. That refusal is right for a real channel - re-cutting a
# version people may already have installed is how you ship two different
# builds under one number - but it blocks re-running this script while
# iterating locally, so say which it is.
$existing = Join-Path $releases "$packId-$Version-full.nupkg"
if (Test-Path $existing) {
    if ($Republish) {
        Write-Warning "Removing the existing $Version from the local release set (-Republish)."
        Remove-Item (Join-Path $releases "$packId-$Version-*.nupkg") -Force
    } else {
        Fail "$Version is already in $releases. Bump project(VERSION), or pass -Republish to replace it locally (never on a published channel)."
    }
}

& vpk @packArgs
if ($LASTEXITCODE) { Fail "vpk pack failed" }

$setup = Get-ChildItem $releases -Filter "$packId-$channel-Setup.exe" | Select-Object -First 1
if (-not $setup) { Fail "vpk produced no Setup bundle" }

# ---- signed update manifest ------------------------------------------------
# The updater verifies this before it reads Velopack's index at all
# (SignedManifestSource). No key here means the release set is not publishable;
# say so rather than shipping an unverifiable channel.
$tool = Join-Path $repo "src.managed\MediaViewer.Updater.Tests"
& dotnet run --project $tool -c Release -- manifest $releases $Version $MinVersion $Blocklist $channel
if ($LASTEXITCODE) { Fail "manifest generation failed" }
$manifest = Join-Path $releases "mediaviewer-manifest.json"
if ($ManifestKey) {
    & dotnet run --project $tool -c Release -- sign $manifest $ManifestKey
    if ($LASTEXITCODE) { Fail "manifest signing failed" }
    Write-Host "manifest signed"
} else {
    Write-Warning "Manifest is NOT signed (-ManifestKey not given). Clients reject it."
}

# ---- wizard ----------------------------------------------------------------
if (-not $NoWizard) {
    $isccArgs = @(
        "/DMvVersion=$Version",
        "/DMvRepoRoot=$repo",
        "/DMvPayloadSetup=$($setup.FullName)",
        "/O$OutputDir",
        (Join-Path $repo "tools\package\mediaviewer.iss")
    )
    if ($SigningMetadata -or $SignParams) {
        # ISCC signs through a named sign tool configured in the Inno IDE or on
        # the command line. Rather than guess at the operator's configuration,
        # the wizard is signed as a separate step, which is also how a release
        # pipeline holds the credential for the shortest time.
        Write-Host "note: sign the wizard after this step (see update-signing.md)"
    }
    & $Iscc @isccArgs
    if ($LASTEXITCODE) { Fail "ISCC failed" }
}

Write-Host ""
Write-Host "release set: $releases"
if (-not $NoWizard) { Write-Host "wizard:      $OutputDir\$packId-$Version-Setup.exe" }
if (-not $signedBuild) { Write-Host "NOT SIGNED - do not publish this build." }
