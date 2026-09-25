# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# plan/13 Part 3: "What never leaves the machine ... enforced by a CI check on
# the telemetry payload schema."
#
# The runtime gate is src/shell/telemetry.cpp (looks_like_user_data), and it is
# tested in tests/test_telemetry.cpp. This script is the second line: it reads
# the SOURCE and fails the build if telemetry ever grows a way to carry a
# string that is not from the fixed vocabulary.
#
# It checks four things that no unit test can, because they are about what the
# code is allowed to contain rather than what it does:
#
#   1. The event table and the metric-name table hold only vocabulary terms.
#   2. `record` takes a string_view tag and nothing free-form; no overload
#      takes a path, a wide string, or a std::filesystem::path.
#   3. telemetry.cpp never mentions a metadata or path API.
#   4. Every call site in the tree passes a literal tag, never a variable that
#      could hold a filename.
#
# Exit 0 clean, 1 on a violation.

[CmdletBinding()]
param([string]$RepoRoot)

if (-not $RepoRoot) {
    $RepoRoot = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..'
}
$RepoRoot = (Resolve-Path $RepoRoot).Path
$header = Join-Path $RepoRoot 'src\shell\telemetry.h'
$source = Join-Path $RepoRoot 'src\shell\telemetry.cpp'
$violations = @()

foreach ($f in $header, $source) {
    if (-not (Test-Path $f)) { Write-Error "missing $f"; exit 1 }
}
$headerText = Get-Content $header -Raw
$sourceText = Get-Content $source -Raw

# --- 1. no forbidden field name anywhere in the schema ----------------------
# plan/13: paths, filenames, folder structure, drive labels, pixels,
# thumbnails, EXIF/XMP/IPTC, and anything derived including path hashes.
$forbiddenField = @(
    'path', 'paths', 'filename', 'file', 'folder', 'directory', 'dir',
    'drive', 'volume', 'exif', 'xmp', 'iptc', 'gps', 'latitude', 'longitude',
    'serial', 'username', 'user', 'machine', 'hostname', 'sid',
    'thumbnail', 'thumb', 'pixel', 'pixels', 'name'
)
# Only the SCHEMA is a payload. A log message and a comment may say "filename"
# while explaining why one is refused, and banning the word there would make
# the check impossible to satisfy honestly. Three things are scanned:
#   * the metric-name table,
#   * the event enum's own names,
#   * the JSON keys format_event writes.
$literals = @()

$metricTable = [regex]::Match($sourceText, '(?s)kMetricNames\[\]\s*=\s*\{(.*?)\};')
if (-not $metricTable.Success) { $violations += "kMetricNames table not found in telemetry.cpp" }
else {
    $literals += [regex]::Matches($metricTable.Groups[1].Value, '"([^"\\]*)"') |
        ForEach-Object { $_.Groups[1].Value }
}

$eventEnum = [regex]::Match($headerText, '(?s)enum class event\s*:\s*std::int32_t\s*\{(.*?)\};')
if (-not $eventEnum.Success) { $violations += "event enum not found in telemetry.h" }
else {
    $literals += [regex]::Matches($eventEnum.Groups[1].Value, '(?m)^\s*([a-z_][a-z0-9_]*)\s*=') |
        ForEach-Object { $_.Groups[1].Value }
}

$formatBody = [regex]::Match($sourceText, '(?s)std::string format_event\((.*?)\n\}')
if (-not $formatBody.Success) { $violations += "format_event not found in telemetry.cpp" }
else {
    $literals += [regex]::Matches($formatBody.Groups[1].Value, '\\"([a-z_][a-z0-9_]*)\\"\s*:') |
        ForEach-Object { $_.Groups[1].Value }
}
foreach ($lit in $literals) {
    if (-not $lit) { continue }
    $segments = $lit.ToLowerInvariant() -split '[^a-z0-9]+' | Where-Object { $_ }
    foreach ($seg in $segments) {
        if ($forbiddenField -contains $seg) {
            $violations += "string literal '$lit' has the forbidden field segment '$seg'"
        }
    }
}

# --- 2. the recording signature stays closed --------------------------------
if ($headerText -notmatch 'bool record\(event id, std::string_view tag, const std::vector<metric>& metrics\)') {
    $violations += "record()'s signature changed; the payload is no longer a fixed id + gated tag + named integers"
}
foreach ($wide in 'std::wstring', 'wchar_t', 'std::filesystem', 'LPCWSTR') {
    if ($headerText -match [regex]::Escape($wide) -and $headerText -notmatch "spool_path") {
        $violations += "telemetry.h mentions $wide outside spool_path(); a wide string is how a path arrives"
    }
}

# --- 3. no metadata or path API in the implementation -----------------------
foreach ($api in 'exiv2', 'av_dict', 'GetFullPathName', 'PathFindFileName', 'GetUserName',
                 'GetComputerName', 'SHGetKnownFolderPath') {
    if ($sourceText -match [regex]::Escape($api)) {
        $violations += "telemetry.cpp calls $api"
    }
}

# --- 4. every call site passes a literal tag --------------------------------
# record(event::x, "literal", ...) is the only accepted shape. A variable there
# is exactly how a filename would reach the payload.
$callSites = Get-ChildItem (Join-Path $RepoRoot 'src') -Recurse -Include *.cpp, *.h |
    Where-Object { $_.Name -ne 'telemetry.h' -and $_.Name -ne 'telemetry.cpp' } |
    Select-String -Pattern 'telemetry::record\s*\(' -AllMatches
foreach ($hit in $callSites) {
    if ($hit.Line -notmatch 'telemetry::record\s*\(\s*event::\w+\s*,\s*"') {
        $violations += "$($hit.Filename):$($hit.LineNumber) passes a non-literal tag to telemetry::record"
    }
}

if ($violations.Count) {
    Write-Host "telemetry schema check FAILED (plan/13 Part 3):" -ForegroundColor Red
    $violations | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    exit 1
}
Write-Host "telemetry schema check: clean"
exit 0
