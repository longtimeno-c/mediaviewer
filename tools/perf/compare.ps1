# Drives MediaViewer, Windows Photos and Media Player through identical scripted work while an elevated
# PresentMon records every present; writes runs.json next to the PresentMon CSV. Analyse with compare-analyse.py.
# Needs Intel PresentMon (winget install Intel.PresentMon). PresentMon asks for administrator once (UAC).
param([string]$OutDir = "$PSScriptRoot\..\..\docs\perf\compare", [int]$PanSeconds = 20, [int]$PlaySeconds = 30, [int]$OpenReps = 3, [int]$PanReps = 2)
. "$PSScriptRoot\compare-lib.ps1"
. "$PSScriptRoot\compare-apps.ps1"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$Media = "$Repo\tools\testmedia"
$Stills = @("$Media\raw\sony_ilce7rm3.arw", "$Media\raw\nikon_d7500.nef", "$Media\heif\libheif-example.heic")
$Clips  = @("$Media\av_transport.mp4", "$Media\hevc_4k_8bit_bt709.mp4")
$pm = "C:\Program Files\Intel\PresentMon\PresentMonConsoleApplication\PresentMon-2.6.0-x64.exe"
$csv = "$OutDir\presentmon.csv"; Remove-Item $csv -ErrorAction SilentlyContinue
$budget = 60 + $Stills.Count * 2 * $OpenReps * 20 + $Stills.Count * 2 * $PanReps * 45 + $Clips.Count * 2 * ($PlaySeconds + 25)
& $pm --restart_as_admin --process_name mediaviewer_lab.exe --process_name Photos.exe --process_name Microsoft.Media.Player.exe `
      --qpc_time --no_console_stats --timed $budget --terminate_after_timed --output_file $csv | Out-Null
for ($i = 0; $i -lt 60 -and -not (Test-Path $csv); $i++) { Start-Sleep 1 }
if (-not (Test-Path $csv)) { throw "PresentMon did not start (was the UAC prompt declined?)" }
Start-Sleep 3

$runs = New-Object System.Collections.ArrayList
$qpc = { [Diagnostics.Stopwatch]::GetTimestamp() }
function Place($app, $h) { if ($app -eq 'mv') { [Win]::MoveWindow($h, 260, 260, 1920, 1023, $true) | Out-Null } }

foreach ($f in $Stills) { foreach ($a in 'mv', 'photos') {
  for ($r = 0; $r -lt $OpenReps; $r++) {
    Stop-App $a; Start-Sleep 2
    $base = [Win]::Grab(0, 0, 2560, 1440); $samples = New-Object System.Collections.ArrayList
    $sw = [Diagnostics.Stopwatch]::StartNew(); Start-App $a $f
    while ($sw.Elapsed.TotalSeconds -lt 9) { [void]$samples.Add(@($sw.Elapsed.TotalMilliseconds, [Win]::Grab(0, 0, 2560, 1440))) }
    $final = $samples[$samples.Count - 1][1]; $t = $null
    foreach ($s in $samples) { if ([Win]::Settled($base, $final, $s[1]) -ge 0.9) { $t = $s[0]; break } }
    [void]$runs.Add([ordered]@{ kind = 'open'; app = $a; file = [IO.Path]::GetFileName($f); rep = $r; first_pixel_ms = $t; samples = $samples.Count })
    "open  $a $([IO.Path]::GetFileName($f)) #$r -> $t ms"
  } } }

foreach ($f in $Stills) { foreach ($a in 'mv', 'photos') {
  for ($r = 0; $r -lt $PanReps; $r++) {
    Stop-App $a; Start-App $a $f; Start-Sleep 9
    $h = [Win]::Find((App-Title $a $f)); if ($h -eq 0) { "no window: $a"; continue }
    Place $a $h; Start-Sleep 1; [Win]::SetForegroundWindow($h) | Out-Null; Start-Sleep 1
    $rc = [Win]::Rect($h); $cx = $rc[0] + [int]($rc[2] / 2); $cy = $rc[1] + [int]($rc[3] / 2)
    [Win]::SetCursorPos($cx, $cy) | Out-Null; Start-Sleep -Milliseconds 300
    [Win]::Wheel(6, $Apps[$a].Ctrl); Start-Sleep 2
    $t0 = & $qpc; [Win]::Drag($cx, $cy, [int]($rc[2] * 0.3), [int]($rc[3] * 0.25), $PanSeconds); $t1 = & $qpc
    [void]$runs.Add([ordered]@{ kind = 'pan'; app = $a; file = [IO.Path]::GetFileName($f); rep = $r; qpc_start = $t0; qpc_end = $t1 })
    "pan   $a $([IO.Path]::GetFileName($f)) #$r"
  } } }

foreach ($f in $Clips) { foreach ($a in 'mv', 'wmp') {
  Stop-App $a; Start-App $a $f; Start-Sleep 8
  $h = [Win]::Find((App-Title $a $f)); if ($h -ne 0) { Place $a $h; [Win]::SetForegroundWindow($h) | Out-Null }
  $t0 = & $qpc; Start-Sleep $PlaySeconds; $t1 = & $qpc
  [void]$runs.Add([ordered]@{ kind = 'play'; app = $a; file = [IO.Path]::GetFileName($f); rep = 0; qpc_start = $t0; qpc_end = $t1 })
  "play  $a $([IO.Path]::GetFileName($f))"
} }
foreach ($a in 'mv', 'photos', 'wmp') { Stop-App $a }
[ordered]@{ qpc_freq = [Diagnostics.Stopwatch]::Frequency; runs = $runs } | ConvertTo-Json -Depth 5 | Set-Content "$OutDir\runs.json" -Encoding utf8
"done: $OutDir\runs.json"
