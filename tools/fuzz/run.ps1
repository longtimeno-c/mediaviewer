# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runs the libFuzzer harnesses (PR 7, plan/09) for a fixed time each.
#
#   ./tools/fuzz/run.ps1 -BuildDir build-fuzz -Seconds 60              # PR smoke
#   ./tools/fuzz/run.ps1 -BuildDir build-fuzz -Seconds 1200 -Harness png,gif
#
# Each harness gets a writable working corpus under <OutDir>/corpus/<name>,
# seeded read-only from tests/data/seeds (+ tests/data/broken). The seeds
# directory is never written. Crash / leak / timeout / OOM inputs land in
# <OutDir>/artifacts/<name>/. Exit 1 if any harness failed or left an artefact;
# copy a minimised reproducer into tests/data/broken/ when you fix it.

[CmdletBinding()]
param(
    [string]$BuildDir = 'build-fuzz',
    [string]$Config = 'Release',
    [int]$Seconds = 60,
    [string[]]$Harness = @(),
    [string]$OutDir = '',
    [int]$RssLimitMb = 2560,
    [int]$TimeoutSec = 10,
    [int]$Jobs = 1,
    # Seconds added to each harness's wall-clock guard before it is killed.
    [int]$GraceSec = 120
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $repo $BuildDir }
$bin = Join-Path $BuildDir "bin\$Config"
if (-not (Test-Path $bin)) { throw "No $bin. Build the mv_fuzzers target first (see tools/fuzz/CMakeLists.txt)." }
if (-not $OutDir) { $OutDir = Join-Path $BuildDir 'fuzz-out' }

$seeds = Join-Path $repo 'tests\data\seeds'
$broken = Join-Path $repo 'tests\data\broken'

# harness -> (seed families, max input length). Lengths are big enough for the
# seeds and the broken files, small enough that iterations stay fast.
$table = [ordered]@{
    jpeg        = @{ Seeds = @('jpeg');                                 MaxLen = 65536 }
    png         = @{ Seeds = @('png');                                  MaxLen = 65536 }
    bmp         = @{ Seeds = @('bmp');                                  MaxLen = 65536 }
    gif         = @{ Seeds = @('gif');                                  MaxLen = 65536 }
    webp        = @{ Seeds = @('webp');                                 MaxLen = 65536 }
    tiff        = @{ Seeds = @('tiff');                                 MaxLen = 65536 }
    ico         = @{ Seeds = @('ico', 'png', 'bmp');                    MaxLen = 65536 }
    heic        = @{ Seeds = @('heic');                                 MaxLen = 131072 }
    avif        = @{ Seeds = @('avif');                                 MaxLen = 131072 }
    raw         = @{ Seeds = @('raw', 'tiff');                          MaxLen = 262144 }
    raw_preview = @{ Seeds = @('raw', 'tiff', 'jpeg');                  MaxLen = 262144 }
    decode      = @{ Seeds = @('jpeg','png','bmp','gif','webp','tiff','ico','heic','avif','raw'); MaxLen = 131072 }
    animation   = @{ Seeds = @('gif', 'webp', 'png', 'heic', 'avif');   MaxLen = 131072 }
}

if ($Harness.Count -eq 1 -and $Harness[0] -match ',') { $Harness = $Harness[0] -split ',' }
if ($Harness.Count -eq 0) { $Harness = @($table.Keys) }

# vcpkg DLLs and the ASan runtime sit beside the harnesses; make sure a
# symbolizer is reachable so crash stacks have names.
$env:PATH = "$bin;$env:PATH"
if (-not $env:ASAN_OPTIONS) {
    # Leak detection is not supported by ASan on Windows; keep reports whole.
    $env:ASAN_OPTIONS = 'detect_leaks=0:handle_abort=1:allocator_may_return_null=1:windows_hook_rtl_allocators=true'
}

$results = @()
foreach ($name in $Harness) {
    if (-not $table.Contains($name)) { throw "Unknown harness '$name'. Known: $($table.Keys -join ', ')" }
    $exe = Join-Path $bin "fuzz_$name.exe"
    if (-not (Test-Path $exe)) { throw "Missing $exe" }

    $corpus = Join-Path $OutDir "corpus\$name"
    $artifacts = Join-Path $OutDir "artifacts\$name"
    New-Item -ItemType Directory -Force -Path $corpus, $artifacts | Out-Null

    $seedDirs = @()
    foreach ($family in $table[$name].Seeds) {
        $d = Join-Path $seeds $family
        if (Test-Path $d) { $seedDirs += $d }
    }
    if ($name -in @('decode', 'animation', 'png', 'gif', 'tiff', 'ico', 'bmp', 'jpeg', 'webp', 'heic', 'avif', 'raw')) {
        $seedDirs += $broken
    }

    $fuzzArgs = @(
        "-max_total_time=$Seconds",
        "-max_len=$($table[$name].MaxLen)",
        "-rss_limit_mb=$RssLimitMb",
        "-malloc_limit_mb=$RssLimitMb",
        "-timeout=$TimeoutSec",
        "-artifact_prefix=$artifacts\",
        '-print_final_stats=1',
        '-reload=0'
    )
    if ($Jobs -gt 1) { $fuzzArgs += @("-jobs=$Jobs", "-workers=$Jobs") }
    $fuzzArgs += @($corpus) + $seedDirs

    Write-Host "=== fuzz_$name for $Seconds s (max_len $($table[$name].MaxLen)) ==="
    $log = Join-Path $OutDir "fuzz_$name.log"
    $proc = Start-Process -FilePath $exe -ArgumentList $fuzzArgs -NoNewWindow -PassThru `
        -RedirectStandardError $log -RedirectStandardOutput "$log.stdout"
    # Touch the handle now: without it Windows PowerShell never caches the exit
    # code of a -PassThru process and ExitCode reads back $null (every clean
    # run looked like a failure).
    $null = $proc.Handle
    if (-not $proc.WaitForExit(($Seconds + $GraceSec) * 1000)) {
        $proc.Kill()
        $exit = 'killed (wall-clock guard)'
    } else {
        $exit = $proc.ExitCode
    }
    $found = @(Get-ChildItem -Path $artifacts -File -ErrorAction SilentlyContinue)
    $stats = Select-String -Path $log -Pattern 'stat::number_of_executed_units|stat::peak_rss_mb' |
        ForEach-Object { $_.Line.Trim() }
    Get-Content $log -Tail 25 | Write-Host
    $ok = ($exit -eq 0) -and ($found.Count -eq 0)
    $results += [pscustomobject]@{
        Harness = $name; Exit = $exit; Artifacts = $found.Count
        Execs = ($stats | Where-Object { $_ -match 'executed' }) -replace '.*:\s*', ''
        Ok = $ok
    }
}

Write-Host ''
$results | Format-Table -AutoSize | Out-String | Write-Host
if ($results | Where-Object { -not $_.Ok }) {
    Write-Host "Fuzzing found problems. Artefacts: $(Join-Path $OutDir 'artifacts')" -ForegroundColor Red
    exit 1
}
Write-Host 'fuzz: clean'
exit 0
