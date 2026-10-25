# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The driftwire command line's Windows zip (docs/design/28 "Installing"), made
# the same way by CI (unsigned) and by a release (signed between the steps):
#
#   driftwire-windows.ps1 -Action Stage [-BuildDir build] [-Config Release] [-Version V] [-OutDir D]
#     D/driftwire-V-windows-x64/   driftwire.exe; every DLL it loads that Windows does not have,
#                                  found by reading the import tables (ours from the build, the
#                                  Visual C++ runtime from Visual Studio's redistributable
#                                  folder, never System32); LICENSE, NOTICE and COPYRIGHT.txt
#     D/sign/                      copies of ours (driftwire.exe and the DLLs built with it) for
#                                  the signing step; Microsoft's runtime keeps its own signature
#   driftwire-windows.ps1 -Action Pack [-Version V] [-OutDir D]
#     puts D/sign/* (signed or not) into the folder, checks that driftwire.exe runs from there
#     and says V, and writes D/driftwire-V-windows-x64.zip (that one folder inside)
#   driftwire-windows.ps1 -Action Imports -Path FILE
#     the DLLs a PE file names in its import and delay-load tables, one per line
#
# x64-windows is a dynamic triplet (tools/vcpkg/triplets/x64-windows.cmake), so
# libsodium and BLAKE3 are DLLs here, shared with the app and its add-ons; FTXUI's
# port is static only. The C runtime is the dynamic one (/MD) the whole build uses,
# so the zip carries the Visual C++ runtime DLLs the program imports, app-local: a
# per-user install without admin cannot run vc_redist, and a clean Windows does not
# have them. The Universal CRT (ucrtbase, api-ms-win-crt-*) is part of Windows 10.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('Stage', 'Pack', 'Imports')][string]$Action,
    [string]$BuildDir = 'build',
    [string]$Config = 'Release',
    # Defaults to project(VERSION) in CMakeLists.txt.
    [string]$Version,
    # Defaults to <repo>/dist/driftwire-cli.
    [string]$OutDir,
    # -Action Imports: the PE file to read.
    [string]$Path,
    # The Visual C++ runtime's DLLs (default: Microsoft.VC14*.CRT of the newest
    # redistributable of the newest Visual Studio with the C++ tools, or of
    # $env:VCToolsRedistDir in a developer shell).
    [string]$RedistDir,
    # Windows' own DLLs (default: $env:SystemRoot\System32).
    [string]$SystemDir,
    # Tests on a machine that cannot run the program: skip the run in Pack.
    [switch]$NoRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

function Fail([string]$message) {
    throw "driftwire-windows: $message"
}

# ---- reading a PE file's imports ------------------------------------------------

function Get-PeImports([string]$File) {
    $b = [IO.File]::ReadAllBytes($File)
    $name = Split-Path -Leaf $File
    if ($b.Length -lt 64 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { Fail "$name is not a PE file" }
    $pe = [BitConverter]::ToInt32($b, 0x3C)
    if ($pe -lt 64 -or $pe + 24 -gt $b.Length -or [BitConverter]::ToUInt32($b, $pe) -ne 0x00004550) {
        Fail "$name is not a PE file"
    }
    $machine = [BitConverter]::ToUInt16($b, $pe + 4)
    $count = [BitConverter]::ToUInt16($b, $pe + 6)
    $optSize = [BitConverter]::ToUInt16($b, $pe + 20)
    $opt = $pe + 24
    $magic = [BitConverter]::ToUInt16($b, $opt)
    # The data directories follow the PE32+ (0x20b) or PE32 (0x10b) optional header.
    if ($magic -eq 0x20b) { $dirs = $opt + 112; $imageBase = [BitConverter]::ToUInt64($b, $opt + 24) }
    elseif ($magic -eq 0x10b) { $dirs = $opt + 96; $imageBase = [uint64][BitConverter]::ToUInt32($b, $opt + 28) }
    else { Fail "$name has an unknown optional header" }
    $dirCount = [BitConverter]::ToUInt32($b, $dirs - 4)
    $sectionTable = $opt + $optSize
    $sections = @(for ($i = 0; $i -lt $count; $i++) {
        $s = $sectionTable + 40 * $i
        [pscustomobject]@{
            VA = [BitConverter]::ToUInt32($b, $s + 12)
            Size = [Math]::Max([BitConverter]::ToUInt32($b, $s + 8), [BitConverter]::ToUInt32($b, $s + 16))
            Raw = [BitConverter]::ToUInt32($b, $s + 20)
        }
    })
    $toOffset = {
        param([uint64]$rva)
        foreach ($s in $sections) {
            if ($rva -ge $s.VA -and $rva -lt [uint64]$s.VA + $s.Size) { return [int]($rva - $s.VA + $s.Raw) }
        }
        Fail "$name points outside its sections"
    }
    $readName = {
        param([uint64]$rva)
        $at = & $toOffset $rva
        $end = $at
        while ($end -lt $b.Length -and $b[$end] -ne 0) { $end++ }
        [Text.Encoding]::ASCII.GetString($b, $at, $end - $at)
    }
    $names = New-Object System.Collections.Generic.List[string]
    # 1: the import table, IMAGE_IMPORT_DESCRIPTOR (20 bytes, Name at 12).
    # 13: the delay-load table, IMAGE_DELAYLOAD_DESCRIPTOR (32 bytes, DllNameRVA at 4;
    #     a virtual address, not an RVA, when bit 0 of Attributes is clear).
    foreach ($table in @(@{ Index = 1; Size = 20; NameAt = 12 }, @{ Index = 13; Size = 32; NameAt = 4 })) {
        if ($dirCount -le $table.Index) { continue }
        $rva = [BitConverter]::ToUInt32($b, $dirs + 8 * $table.Index)
        if ($rva -eq 0) { continue }
        $at = & $toOffset $rva
        while ($true) {
            $nameRva = [uint64][BitConverter]::ToUInt32($b, $at + $table.NameAt)
            if ($nameRva -eq 0) { break }
            if ($table.Index -eq 13 -and ([BitConverter]::ToUInt32($b, $at) -band 1) -eq 0) { $nameRva -= $imageBase }
            $names.Add((& $readName $nameRva))
            $at += $table.Size
        }
    }
    [pscustomobject]@{ Machine = $machine; Imports = $names.ToArray() }
}

# ---- helpers --------------------------------------------------------------------

function Get-ProjectVersion {
    $m = [regex]::Match((Get-Content (Join-Path $repo 'CMakeLists.txt') -Raw), '(?ms)^project\(mediaviewer\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
    if (-not $m.Success) { Fail 'could not read project(VERSION) from CMakeLists.txt; pass -Version' }
    $m.Groups[1].Value
}

# The files of a folder by lower-case name: Windows matches DLL names without case.
function Get-Index([string]$Dir) {
    $index = @{}
    if ($Dir -and (Test-Path -LiteralPath $Dir -PathType Container)) {
        foreach ($f in Get-ChildItem -LiteralPath $Dir -File) { $index[$f.Name.ToLowerInvariant()] = $f.FullName }
    }
    $index
}

function Find-RedistDir {
    $root = $env:VCToolsRedistDir
    if (-not $root) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere)) { Fail 'vswhere.exe not found: pass -RedistDir' }
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if (-not $vs) { Fail 'no Visual Studio with the C++ tools: pass -RedistDir' }
        $msvc = Join-Path $vs 'VC\Redist\MSVC'
        $root = Get-ChildItem -LiteralPath $msvc -Directory |
            Where-Object { $_.Name -match '^\d+(\.\d+)+$' } |
            Sort-Object { [version]$_.Name } | Select-Object -Last 1 -ExpandProperty FullName
        if (-not $root) { Fail "no versioned redistributable under $msvc" }
    }
    $crt = Get-ChildItem -LiteralPath (Join-Path $root 'x64') -Directory -Filter 'Microsoft.VC14*.CRT' |
        Sort-Object Name | Select-Object -Last 1 -ExpandProperty FullName
    if (-not $crt) { Fail "no x64\Microsoft.VC14*.CRT under $root" }
    $crt
}

# ---- Stage ----------------------------------------------------------------------

function Invoke-Stage {
    $bin = Join-Path $BuildDir "bin\$Config"
    if (-not [IO.Path]::IsPathRooted($bin)) { $bin = Join-Path $repo $bin }
    $exe = Join-Path $bin 'driftwire.exe'
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { Fail "no driftwire.exe in ${bin}: build the mv_driftwire_cli target ($Config) first" }
    if (-not $RedistDir) { $RedistDir = Find-RedistDir }
    if (-not $SystemDir) { $SystemDir = Join-Path $env:SystemRoot 'System32' }
    $ourIndex = Get-Index $bin
    $crtIndex = Get-Index $RedistDir
    $sysIndex = Get-Index $SystemDir
    if ($sysIndex.Count -eq 0) { Fail "no Windows DLLs in $SystemDir" }

    # The closure of what driftwire.exe loads: ours and the runtime are carried,
    # Windows' own (System32, and the API sets it maps) are not; anything else fails.
    $ours = [ordered]@{ 'driftwire.exe' = $exe }
    $crt = [ordered]@{}
    $seen = @{ 'driftwire.exe' = $true }
    $queue = New-Object System.Collections.Generic.Queue[string]
    $queue.Enqueue($exe)
    while ($queue.Count -gt 0) {
        $file = $queue.Dequeue()
        $pe = Get-PeImports $file
        if ($pe.Machine -ne 0x8664) { Fail ("{0} is not x64 (machine 0x{1:x4})" -f (Split-Path -Leaf $file), $pe.Machine) }
        foreach ($import in $pe.Imports) {
            $key = $import.ToLowerInvariant()
            if ($seen.ContainsKey($key)) { continue }
            $seen[$key] = $true
            if ($key -like 'api-ms-win-*' -or $key -like 'ext-ms-*') { continue }
            if ($crtIndex.ContainsKey($key)) { $crt[$key] = $crtIndex[$key]; $queue.Enqueue($crtIndex[$key]); continue }
            if ($ourIndex.ContainsKey($key)) { $ours[$key] = $ourIndex[$key]; $queue.Enqueue($ourIndex[$key]); continue }
            if ($sysIndex.ContainsKey($key)) { continue }
            Fail ("driftwire.exe needs {0} (through {1}), which is not built here, not the Visual C++ runtime and not part of Windows" -f $import, (Split-Path -Leaf $file))
        }
    }

    $top = Join-Path $OutDir "driftwire-$Version-windows-x64"
    $sign = Join-Path $OutDir 'sign'
    foreach ($dir in $top, $sign) {
        if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
        New-Item -ItemType Directory -Force $dir | Out-Null
    }
    foreach ($entry in $ours.GetEnumerator()) {
        Copy-Item -LiteralPath $entry.Value -Destination (Join-Path $top (Split-Path -Leaf $entry.Value))
        Copy-Item -LiteralPath $entry.Value -Destination (Join-Path $sign (Split-Path -Leaf $entry.Value))
    }
    foreach ($entry in $crt.GetEnumerator()) {
        Copy-Item -LiteralPath $entry.Value -Destination (Join-Path $top (Split-Path -Leaf $entry.Value))
    }
    Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination (Join-Path $top 'LICENSE')
    Copy-Item -LiteralPath (Join-Path $repo 'NOTICE') -Destination (Join-Path $top 'NOTICE')
    Copy-Item -LiteralPath (Join-Path $repo 'packaging\linux\copyright') -Destination (Join-Path $top 'COPYRIGHT.txt')
    Write-Host ("driftwire-windows: ours {0}; the Visual C++ runtime {1} (from {2})" -f
        (($ours.Values | ForEach-Object { Split-Path -Leaf $_ }) -join ', '),
        $(if ($crt.Count) { ($crt.Values | ForEach-Object { Split-Path -Leaf $_ }) -join ', ' } else { 'none' }),
        $RedistDir)
    Write-Host "driftwire-windows: staged $top; sign what is in $sign, then -Action Pack"
}

# ---- Pack -----------------------------------------------------------------------

function Invoke-Pack {
    $top = Join-Path $OutDir "driftwire-$Version-windows-x64"
    $sign = Join-Path $OutDir 'sign'
    if (-not (Test-Path -LiteralPath (Join-Path $top 'driftwire.exe'))) { Fail "nothing staged in ${top}: -Action Stage first" }
    $signed = @(Get-ChildItem -LiteralPath $sign -File)
    if (-not ($signed | Where-Object Name -eq 'driftwire.exe')) { Fail "no driftwire.exe in $sign" }
    foreach ($f in $signed) {
        if (-not (Test-Path -LiteralPath (Join-Path $top $f.Name))) { Fail "$($f.Name) in $sign was not staged" }
        Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $top $f.Name) -Force
    }
    if (-not $NoRun) {
        # From the folder itself: the DLLs beside it are the ones it loads.
        $said = (& (Join-Path $top 'driftwire.exe') --version | Out-String).Trim()
        if ($LASTEXITCODE -ne 0) { Fail "driftwire.exe --version failed ($LASTEXITCODE)" }
        if ($said -ne "driftwire $Version") { Fail "driftwire.exe says '$said', not 'driftwire $Version'" }
        Write-Host "driftwire-windows: $said"
    }
    $zip = Join-Path $OutDir "driftwire-$Version-windows-x64.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($top, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Host "driftwire-windows: $zip sha256 $hash"
}

if ($Action -eq 'Imports') {
    if (-not $Path) { Fail '-Action Imports needs -Path' }
    (Get-PeImports $Path).Imports
    return
}
if (-not $Version) { $Version = Get-ProjectVersion }
if ($Version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') { Fail "'$Version' is not an x.y.z version" }
if (-not $OutDir) { $OutDir = Join-Path $repo 'dist\driftwire-cli' }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
if ($Action -eq 'Stage') { Invoke-Stage } else { Invoke-Pack }
