# SPDX-License-Identifier: GPL-2.0-or-later
# PR 8 verify, local end to end: "An update downloads and stages without
# showing the wizard" and "Exercise update signature rejection and rollback".
#
#   1. Pack 0.1.0 / 0.1.1 / 0.1.2 of the real payload with vpk (pack id
#      MediaViewerE2E, so a real MediaViewer install is never touched).
#   2. Install 0.1.0 silently into a temp root (--installto).
#   3. Publish 0.1.1 with a dev-signed manifest; a TAMPERED manifest first must
#      be rejected (nothing staged); then the signed one stages 0.1.1 and keeps
#      0.1.0's package for rollback. Closing the viewer applies it (no wizard).
#   4. Publish 0.1.2 whose chrome is corrupt (never finishes starting). It is
#      applied, fails to start twice, and the third start rolls back to 0.1.1.
#
# Needs: a Release build (build-pr8d), vpk 1.2.0, dotnet. Uses a dev build of
# the chrome (-p:MvUpdaterDev=true) so MV_UPDATE_FEED_DIR / MV_UPDATE_DEV_PUBKEY
# are honoured; the signature check itself is the shipping code.
param(
    [string]$Payload = "$PSScriptRoot\..\..\build-pr8d\bin\Release",
    [string]$Vpk = "vpk",
    [string]$Work = (Join-Path ([IO.Path]::GetTempPath()) ("mv-e2e-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))),
    [switch]$Keep
)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$Payload = (Resolve-Path $Payload).Path
$packId = "MediaViewerE2E"
$root = Join-Path $Work "install"
$feed = Join-Path $Work "feed"
$keys = Join-Path $Work "keys"
$tool = Join-Path $repo "src.managed\MediaViewer.Updater.Tests"
New-Item -ItemType Directory -Force $Work, $feed, $keys | Out-Null
$results = [ordered]@{}

function Step($name, [bool]$ok) {
    $results[$name] = $ok
    Write-Host ("{0} {1}" -f ($(if ($ok) { "PASS" } else { "FAIL" }), $name))
}

function Tool { dotnet run --project $tool -c Release --no-build -- @args; if ($LASTEXITCODE) { throw "tool $args failed" } }

function Wait-Until([scriptblock]$cond, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) { if (& $cond) { return $true }; Start-Sleep -Milliseconds 500 }
    return [bool](& $cond)
}

function Installed-Version { $f = Join-Path $root "current\sq.version"; if (Test-Path $f) { ([xml](Get-Content $f -Raw)).package.metadata.version } }

function Stage-Payload($version, [switch]$Broken) {
    $dir = Join-Path $Work "payload-$version"
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory $dir | Out-Null
    Get-ChildItem $Payload -File | Where-Object {
        $_.Extension -ne ".pdb" -and $_.Name -notmatch '^(mv_|frametime|mediaviewer_lab\.exe$)'
    } | Copy-Item -Destination $dir
    Get-ChildItem $Payload -Directory | Where-Object { $_.Name -notmatch 'tests?$|frametime' } |
        Copy-Item -Destination $dir -Recurse
    Copy-Item (Join-Path $Payload "mediaviewer_lab.exe") (Join-Path $dir "MediaViewer.exe")
    dotnet publish (Join-Path $repo "src.managed\MediaViewer.Chrome\MediaViewer.Chrome.csproj") --nologo -c Release `
        -r win-x64 --no-self-contained -p:Platform=x64 -p:MvUpdaterDev=true -o $dir | Out-Null
    if ($LASTEXITCODE) { throw "dev chrome publish failed" }
    if ($Broken) {
        # A build that never finishes starting: the chrome assembly is garbage.
        [IO.File]::WriteAllBytes((Join-Path $dir "MediaViewer.Chrome.dll"), [byte[]](1..4096 | ForEach-Object { 0x5A }))
    }
    & $Vpk pack --packId $packId --packVersion $version --packDir $dir --mainExe MediaViewer.exe `
        --channel win --outputDir $feed --shortcuts None --skipVeloAppCheck | Out-Host
    if ($LASTEXITCODE) { throw "vpk pack $version failed" }
}

function Publish-Manifest($version, [switch]$Tamper) {
    Tool manifest $feed $version "0.1.0"
    $m = Join-Path $feed "mediaviewer-manifest.json"
    Tool sign $m (Join-Path $keys "dev.key")
    if ($Tamper) {
        # One byte after signing: "min_version" 0.1.0 -> 0.1.1.
        (Get-Content $m -Raw).Replace('"min_version": "0.1.0"', '"min_version": "0.1.1"') |
            Set-Content -NoNewline -Encoding utf8NoBOM $m -ErrorAction SilentlyContinue
        if (-not $?) { [IO.File]::WriteAllText($m, (Get-Content $m -Raw).Replace('"min_version": "0.1.0"', '"min_version": "0.1.1"')) }
    }
}

function Start-Viewer { Start-Process (Join-Path $root "current\MediaViewer.exe") -PassThru }

function Close-Viewer($p, [int]$after = 6) {
    Start-Sleep -Seconds $after
    if (-not $p.HasExited) {
        $p.Refresh()
        [void](Wait-Until { $p.Refresh(); $p.MainWindowHandle -ne 0 -or $p.HasExited } 20)
        if (-not $p.HasExited) { [void]$p.CloseMainWindow() }
    }
    if (-not $p.WaitForExit(60000)) { $p.Kill(); throw "viewer did not exit" }
}

function Trial([string]$section, [string]$key) {
    $f = Join-Path $root "updater\trial.ini"
    if (-not (Test-Path $f)) { return "" }
    $in = $false
    foreach ($line in Get-Content $f) {
        if ($line -match '^\[(.+)\]') { $in = $Matches[1] -eq $section; continue }
        if ($in -and $line -match "^$key=(.*)$") { return $Matches[1] }
    }
    return ""
}

try {
    dotnet build $tool -c Release --nologo | Out-Null
    $pub = (Tool keygen $keys | Select-Object -Last 1).Trim()
    $env:MV_UPDATE_FEED_DIR = $feed
    $env:MV_UPDATE_DEV_PUBKEY = $pub
    $env:MV_UPDATE_CHECK_DELAY_MS = "1500"

    # --- install 0.1.0, no wizard -----------------------------------------
    Stage-Payload "0.1.0"
    $setup = Get-ChildItem $feed -Filter "*Setup.exe" | Select-Object -First 1
    $sp = Start-Process $setup.FullName -ArgumentList "--silent", "--installto", "`"$root`"" -PassThru -Wait
    # Setup launches the app when done; close any instance it started.
    Get-Process MediaViewer -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root*" } |
        ForEach-Object { Close-Viewer $_ 3 }
    Step "silent install of 0.1.0 into temp root" ((Installed-Version) -eq "0.1.0")

    # --- 0.1.1 with a tampered manifest: rejected, nothing staged ----------
    Stage-Payload "0.1.1"
    Publish-Manifest "0.1.1" -Tamper
    $p = Start-Viewer
    Close-Viewer $p 12
    Step "tampered manifest: nothing staged" (-not (Test-Path (Join-Path $root "packages\$packId-0.1.1-full.nupkg")))
    Step "tampered manifest: still 0.1.0 after exit" ((Installed-Version) -eq "0.1.0")

    # --- signed 0.1.1: stages in background, applies on exit ----------------
    Publish-Manifest "0.1.1"
    $p = Start-Viewer
    $staged = Wait-Until { Test-Path (Join-Path $root "packages\$packId-0.1.1-full.nupkg") } 90
    Step "signed 0.1.1 downloaded and staged while running" $staged
    Step "0.1.0 full package kept for rollback" (Test-Path (Join-Path $root "updater\rollback\$packId-0.1.0-full.nupkg"))
    Close-Viewer $p 2
    Step "0.1.1 applied after exit, no wizard" (Wait-Until { (Installed-Version) -eq "0.1.1" } 120)
    $p = Start-Viewer
    Close-Viewer $p 14   # past the 10 s confirm timer
    Step "0.1.1 start confirmed (trial cleared)" ((Trial "trial" "version") -eq "")

    # --- broken 0.1.2: two failed starts, third rolls back -----------------
    Stage-Payload "0.1.2" -Broken
    Publish-Manifest "0.1.2"
    $p = Start-Viewer
    $staged = Wait-Until { Test-Path (Join-Path $root "packages\$packId-0.1.2-full.nupkg") } 90
    Step "0.1.2 staged" $staged
    Close-Viewer $p 2
    Step "0.1.2 applied after exit" (Wait-Until { (Installed-Version) -eq "0.1.2" } 120)
    Step "trial armed for 0.1.2" ((Trial "trial" "version") -eq "0.1.2")
    for ($i = 1; $i -le 2; $i++) {
        $p = Start-Viewer
        Close-Viewer $p 6
        Step "broken start $i counted" ((Trial "trial" "attempts") -eq "$i")
    }
    $p = Start-Viewer
    $quick = $p.WaitForExit(15000)
    Step "third start hands off to rollback and exits" $quick
    Step "rolled back to 0.1.1" (Wait-Until { (Installed-Version) -eq "0.1.1" } 120)
    Step "0.1.2 recorded as failed" ((Trial "failed" "versions") -match '0\.1\.2')
    # Update.exe restarts the prior version; it must refuse 0.1.2 again.
    Start-Sleep -Seconds 8
    Step "0.1.2 not re-staged after rollback" (-not (Test-Path (Join-Path $root "packages\$packId-0.1.2-full.nupkg")) -or
        ((Installed-Version) -eq "0.1.1"))
    Get-Process MediaViewer -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root*" } |
        ForEach-Object { Close-Viewer $_ 0 }
}
finally {
    Get-Process MediaViewer -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root*" } |
        ForEach-Object { $_.Kill() }
    $upd = Join-Path $root "Update.exe"
    if (Test-Path $upd) { & $upd --silent uninstall | Out-Null }
    Remove-Item Env:MV_UPDATE_FEED_DIR, Env:MV_UPDATE_DEV_PUBKEY, Env:MV_UPDATE_CHECK_DELAY_MS -ErrorAction SilentlyContinue
    $log = Join-Path $env:LOCALAPPDATA "velopack\velopack_$packId.log"
    if (Test-Path $log) { Copy-Item $log (Join-Path $Work "velopack.log") }
    Write-Host "work dir: $Work"
    if (-not $Keep) { Remove-Item -Recurse -Force (Join-Path $Work "install") -ErrorAction SilentlyContinue }
}

$failed = @($results.GetEnumerator() | Where-Object { -not $_.Value }).Count
Write-Host ("{0} passed, {1} failed" -f ($results.Count - $failed), $failed)
exit $(if ($failed) { 1 } else { 0 })
