# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
# Issue #486: launch MediaViewer repeatedly with --boot-report and summarise the boot
# timeline (src/core/boot_timeline.h) per stage. With two -Exe values the runs alternate
# (base, new, base, new), as docs/DEVELOPMENT "Performance suite" asks of a perf claim.
#
#   powershell -File tools\perf\boot.ps1 -Exe C:\b\base\bin\mediaviewer_lab.exe,C:\b\new\bin\mediaviewer_lab.exe `
#              -Runs 10 -Open tools\testmedia\heif\libheif-example.heic -Out boot.json
#
# The first launch of each build is a warm-up and is discarded (the published runs are warm
# cache). Writes every run and the per-stage medians to -Out.
param(
  [Parameter(Mandatory = $true)][string[]]$Exe,
  [int]$Runs = 8,
  [string]$Open = "",
  [string]$Out = "boot.json",
  [int]$PauseMs = 1500
)
$ErrorActionPreference = 'Stop'
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("mv-boot-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $tmp | Out-Null

function Invoke-Boot([string]$exe, [string]$report) {
  $argv = @('--boot-report', "`"$report`"")
  if ($Open) { $argv += "`"$((Resolve-Path $Open).Path)`"" }
  $p = Start-Process -FilePath $exe -ArgumentList $argv -PassThru
  if (-not $p.WaitForExit(60000)) { Stop-Process -Id $p.Id -Force; throw "no report after 60 s: $exe" }
  if (-not (Test-Path $report)) { throw "exit $($p.ExitCode) without a report: $exe" }
  Get-Content $report -Raw | ConvertFrom-Json
}

$results = @{}
foreach ($e in $Exe) { $results[$e] = New-Object System.Collections.ArrayList }
foreach ($e in $Exe) { [void](Invoke-Boot $e (Join-Path $tmp 'warm.json')); Remove-Item (Join-Path $tmp 'warm.json'); Start-Sleep -Milliseconds $PauseMs }
for ($r = 0; $r -lt $Runs; $r++) {
  for ($i = 0; $i -lt $Exe.Count; $i++) {
    $report = Join-Path $tmp "run-$r-$i.json"
    $j = Invoke-Boot $Exe[$i] $report
    [void]$results[$Exe[$i]].Add($j.stages_ms)
    $ready = [math]::Max($j.stages_ms.interactive, [math]::Max($j.stages_ms.first_frame, $j.stages_ms.full_resolution))
    "run $r build $i  interactive=$([math]::Round($j.stages_ms.interactive)) ms  ready=$([math]::Round($ready)) ms"
    Start-Sleep -Milliseconds $PauseMs
  }
}

function Median([double[]]$v) {
  $s = $v | Sort-Object; $n = $s.Count
  if ($n -eq 0) { return -1 }
  if ($n % 2) { return $s[($n - 1) / 2] } else { return ($s[$n / 2 - 1] + $s[$n / 2]) / 2 }
}

$summary = [ordered]@{ open = [IO.Path]::GetFileName($Open); runs = $Runs; builds = @() }
foreach ($e in $Exe) {
  $stages = [ordered]@{}
  foreach ($name in $results[$e][0].PSObject.Properties.Name) {
    $vals = @($results[$e] | ForEach-Object { [double]$_.$name } | Where-Object { $_ -ge 0 })
    $stages[$name] = [ordered]@{
      median = [math]::Round((Median $vals), 1)
      min = if ($vals.Count) { [math]::Round(($vals | Measure-Object -Minimum).Minimum, 1) } else { -1 }
      max = if ($vals.Count) { [math]::Round(($vals | Measure-Object -Maximum).Maximum, 1) } else { -1 }
    }
  }
  $summary.builds += [ordered]@{ exe = $e; median_ms = $stages; runs = $results[$e] }
}
$summary | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 $Out
Remove-Item -Recurse -Force $tmp
""
"stage                 " + (($Exe | ForEach-Object { "build $([array]::IndexOf($Exe, $_))".PadLeft(16) }) -join '')
foreach ($name in $results[$Exe[0]][0].PSObject.Properties.Name) {
  $line = $name.PadRight(22)
  foreach ($b in $summary.builds) { $m = $b.median_ms[$name]; $line += ("{0,7} [{1}-{2}]" -f $m.median, [math]::Round($m.min), [math]::Round($m.max)).PadLeft(16) }
  $line
}
