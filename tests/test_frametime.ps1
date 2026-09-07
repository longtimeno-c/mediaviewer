# SPDX-License-Identifier: GPL-2.0-or-later
param([string]$Harness, [string]$Fixture, [string]$OutputRoot)
$ErrorActionPreference = 'Stop'
$testDir = Join-Path $OutputRoot ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testDir -Force | Out-Null
Copy-Item -LiteralPath $Harness -Destination (Join-Path $testDir 'frametime.exe')
$exe = Join-Path $testDir 'frametime.exe'
$baseline = Join-Path $testDir 'baseline.json'
$previousMode = $env:MV_FRAMETIME_FIXTURE_MODE
try {
    $env:MV_FRAMETIME_FIXTURE_MODE = 'pass'
    & $exe --lab $Fixture --baseline $baseline --update-baseline
    if ($LASTEXITCODE -ne 0) { throw 'Valid pair of reports did not pass' }
    $saved = Get-Content -Raw -LiteralPath $baseline
    $report = Get-Content -Raw -LiteralPath (Join-Path $testDir 'frametime-report.json')
    if ($saved -cne $report) { throw 'Baseline differs from the gated report' }

    foreach ($mode in @('drop', 'short', 'unknown-refresh', 'fast', 'gap', 'busy-idle',
                        'idle-present', 'child-error', 'child-fail', 'malformed', 'regression')) {
        $env:MV_FRAMETIME_FIXTURE_MODE = $mode
        & $exe --lab $Fixture --baseline $baseline --update-baseline
        if ($LASTEXITCODE -eq 0) { throw "Failing $mode run passed" }
        if ((Get-Content -Raw -LiteralPath $baseline) -cne $saved) {
            throw "Failing $mode run changed the baseline"
        }
    }
    Write-Host 'frametime: success, report identity, failure gates, child exits and baseline protection passed'
} finally {
    $env:MV_FRAMETIME_FIXTURE_MODE = $previousMode
}
