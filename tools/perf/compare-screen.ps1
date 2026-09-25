# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
# Same files in MediaViewer, Windows Photos and Media Player, timed from the screen.
# A grab of the picture is about 17 ms, so a gap is only good to about one refresh.
# PresentMon is not required. Writes docs/perf/compare/screen.json.
param([int]$OpenReps = 2, [int]$PanSeconds = 12, [int]$PlaySeconds = 12)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\compare-lib.ps1"
. "$PSScriptRoot\compare-apps.ps1"
$OutDir = "$PSScriptRoot\..\..\docs\perf\compare"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$Media = "$Repo\tools\testmedia"
$Stills = @("$Media\raw\sony_ilce7rm3.arw", "$Media\raw\nikon_d7500.nef", "$Media\heif\libheif-example.heic")
$Clips = @("$Media\av_transport.mp4", "$Media\hevc_4k_8bit_bt709.mp4")
$Proof = Join-Path $env:TEMP 'mv-compare'
New-Item -ItemType Directory -Force $Proof | Out-Null

function Find-App([string]$File) {
  foreach ($title in @([IO.Path]::GetFileName($File), 'Photos', 'Media Player', 'MediaViewer')) {
    $h = [Win]::Find($title)
    if ($h -ne 0) { return $h }
  }
  return [IntPtr]::Zero
}
function Content-Rect($h) {
  $rc = [Win]::Rect($h)
  if ($rc[2] -lt 80 -or $rc[3] -lt 80) { return $null }
  $w = [Math]::Min(640, [int]($rc[2] * 0.5))
  $hh = [Math]::Min(360, [int]($rc[3] * 0.5))
  return @{ x = $rc[0] + [int](($rc[2] - $w) / 2); y = $rc[1] + [int](($rc[3] - $hh) / 2); w = $w; h = $hh; win = $rc }
}
function Changed($a, $b) {
  if ($null -eq $a -or $null -eq $b -or $a.Length -ne $b.Length) { return $true }
  $n = 0; $d = 0
  for ($i = 0; $i -lt $a.Length; $i += 48) {
    $n++
    if ([Math]::Max([Math]::Abs($a[$i] - $b[$i]), [Math]::Max([Math]::Abs($a[$i+1] - $b[$i+1]), [Math]::Abs($a[$i+2] - $b[$i+2]))) -gt 24) { $d++ }
  }
  return $d -ge 4
}
function Sample-Drag($rect, $cx, $cy, $rx, $ry, [double]$seconds) {
  [Win]::SetCursorPos($cx, $cy) | Out-Null
  [Win]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
  $gaps = New-Object System.Collections.Generic.List[double]
  $prev = $null; $last = -1.0; $changes = 0
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.Elapsed.TotalSeconds -lt $seconds) {
    $t = $sw.Elapsed.TotalSeconds
    [Win]::SetCursorPos($cx + [int]($rx * [Math]::Sin($t * 1.3)), $cy + [int]($ry * [Math]::Sin($t * 1.9))) | Out-Null
    $g = [Win]::Grab($rect.x, $rect.y, $rect.w, $rect.h)
    $now = $sw.Elapsed.TotalMilliseconds
    if (Changed $prev $g) {
      if ($last -ge 0) { $gaps.Add($now - $last) }
      $last = $now
      $changes++
    }
    $prev = $g
  }
  [Win]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
  $sorted = @($gaps | Sort-Object)
  $p50 = $null; $p99 = $null
  if ($sorted.Count -ge 4) {
    $p50 = [Math]::Round($sorted[[int]($sorted.Count * 0.50)], 1)
    $p99 = [Math]::Round($sorted[[Math]::Min($sorted.Count - 1, [int]($sorted.Count * 0.99))], 1)
  }
  return @{ changes = $changes; seconds = [Math]::Round($sw.Elapsed.TotalSeconds, 2)
            p50_ms = $p50; p99_ms = $p99
            fps = [Math]::Round($changes / [Math]::Max(0.001, $sw.Elapsed.TotalSeconds), 1) }
}
function Sample-Motion($rect, [double]$seconds) {
  $gaps = New-Object System.Collections.Generic.List[double]
  $prev = $null; $last = -1.0; $changes = 0; $samples = 0
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.Elapsed.TotalSeconds -lt $seconds) {
    $g = [Win]::Grab($rect.x, $rect.y, $rect.w, $rect.h)
    $samples++
    $now = $sw.Elapsed.TotalMilliseconds
    if (Changed $prev $g) {
      if ($last -ge 0) { $gaps.Add($now - $last) }
      $last = $now
      $changes++
    }
    $prev = $g
  }
  $sorted = @($gaps | Sort-Object)
  $p50 = $null; $p99 = $null
  if ($sorted.Count -ge 4) {
    $p50 = [Math]::Round($sorted[[int]($sorted.Count * 0.50)], 1)
    $p99 = [Math]::Round($sorted[[Math]::Min($sorted.Count - 1, [int]($sorted.Count * 0.99))], 1)
  }
  return @{ changes = $changes; samples = $samples; seconds = [Math]::Round($sw.Elapsed.TotalSeconds, 2)
            p50_ms = $p50; p99_ms = $p99
            fps = [Math]::Round($changes / [Math]::Max(0.001, $sw.Elapsed.TotalSeconds), 1) }
}
function Measure-Open([string]$App, [string]$File) {
  Stop-App $App
  $sw = [Diagnostics.Stopwatch]::StartNew()
  Start-App $App $File
  $samples = @()
  $hwnd = [IntPtr]::Zero
  while ($sw.Elapsed.TotalSeconds -lt 8) {
    if ($hwnd -eq [IntPtr]::Zero) { $hwnd = Find-App $File }
    if ($hwnd -ne [IntPtr]::Zero) {
      $rect = Content-Rect $hwnd
      if ($rect) {
        try { $samples += ,@($sw.Elapsed.TotalMilliseconds, [Win]::Grab($rect.x, $rect.y, $rect.w, $rect.h)) } catch {}
      }
    }
    Start-Sleep -Milliseconds 20
  }
  if ($samples.Count -lt 2) { return $null }
  $final = $samples[-1][1]
  $base = $samples[0][1]
  if (-not (Changed $base $final)) { return [Math]::Round($samples[0][0], 1) }
  foreach ($s in $samples) {
    if ($s[1].Length -eq $final.Length -and [Win]::Settled($base, $final, $s[1]) -ge 0.85) {
      return [Math]::Round($s[0], 1)
    }
  }
  return $null
}

$open = @(); $pan = @(); $play = @()
foreach ($f in $Stills) {
  foreach ($a in 'mv', 'photos') {
    for ($r = 0; $r -lt $OpenReps; $r++) {
      $t = Measure-Open $a $f
      $name = [IO.Path]::GetFileName($f)
      $open += @{ app = $a; file = $name; rep = $r; first_pixel_ms = $t }
      "open  $a $name #$r -> $t ms"
      $h = Find-App $f
      if ($h -ne [IntPtr]::Zero) {
        $rc = [Win]::Rect($h)
        try { [Win]::Save($rc[0], $rc[1], [Math]::Min($rc[2], 1280), [Math]::Min($rc[3], 720), (Join-Path $Proof "open-$a-$name-$r.png")) } catch {}
      }
    }
  }
}
foreach ($f in $Stills) {
  foreach ($a in 'mv', 'photos') {
    Stop-App $a
    Start-App $a $f
    Start-Sleep 5
    $h = Find-App $f
    $name = [IO.Path]::GetFileName($f)
    if ($h -eq [IntPtr]::Zero) { "pan   $a $name -> no window"; $pan += @{ app = $a; file = $name; p50_ms = $null; p99_ms = $null; fps = $null; changes = 0 }; continue }
    [Win]::MoveWindow($h, 160, 80, 2000, 1200, $true) | Out-Null
    Start-Sleep 1
    [Win]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 400
    $rc = [Win]::Rect($h)
    $cx = $rc[0] + [int]($rc[2] / 2); $cy = $rc[1] + [int]($rc[3] / 2)
    [Win]::SetCursorPos($cx, $cy) | Out-Null
    [Win]::Wheel(6, $Apps[$a].Ctrl)
    Start-Sleep 1
    $rect = Content-Rect $h
    $motion = Sample-Drag $rect $cx $cy ([int]($rc[2] * 0.3)) ([int]($rc[3] * 0.25)) $PanSeconds
    $pan += @{ app = $a; file = $name; p50_ms = $motion.p50_ms; p99_ms = $motion.p99_ms; fps = $motion.fps; changes = $motion.changes }
    "pan   $a $name -> p50 $($motion.p50_ms) p99 $($motion.p99_ms) fps $($motion.fps) changes $($motion.changes)"
  }
}
foreach ($f in $Clips) {
  foreach ($a in 'mv', 'wmp') {
    Stop-App $a
    Start-App $a $f
    Start-Sleep 4
    $h = Find-App $f
    $name = [IO.Path]::GetFileName($f)
    if ($h -eq [IntPtr]::Zero) { "play  $a $name -> no window"; $play += @{ app = $a; file = $name; p50_ms = $null; p99_ms = $null; fps = $null; changes = 0 }; continue }
    [Win]::MoveWindow($h, 160, 80, 1600, 900, $true) | Out-Null
    [Win]::SetForegroundWindow($h) | Out-Null
    Start-Sleep 1
    $rect = Content-Rect $h
    $motion = Sample-Motion $rect $PlaySeconds
    $play += @{ app = $a; file = $name; p50_ms = $motion.p50_ms; p99_ms = $motion.p99_ms; fps = $motion.fps; changes = $motion.changes }
    "play  $a $name -> p50 $($motion.p50_ms) p99 $($motion.p99_ms) fps $($motion.fps) changes $($motion.changes)"
    try { [Win]::Save($rect.x, $rect.y, $rect.w, $rect.h, (Join-Path $Proof "play-$a-$name.png")) } catch {}
  }
}
foreach ($a in 'mv', 'photos', 'wmp') { Stop-App $a }
@{ method = 'screen grab, about 17 ms'; machine = 'Ryzen 7 5700X3D / RTX 4070 / 2560x1440'; open = $open; pan = $pan; play = $play } |
  ConvertTo-Json -Depth 6 | Set-Content "$OutDir\screen.json" -Encoding utf8
"done: $OutDir\screen.json"
