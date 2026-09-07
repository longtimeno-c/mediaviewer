# SPDX-License-Identifier: GPL-2.0-or-later
#
# D9 / plan/15-platforms.md: from PR 4, new native code above gfx/ does not
# include Windows-only headers a Metal host cannot replace.
#
# Direct #include of d3d11.h, dxgi.h, windows.h, atlbase.h, the WASAPI/COM
# headers (audioclient.h, mmdeviceapi.h, audiopolicy.h, mmreg.h, avrt.h,
# endpointvolume.h, functiondiscoverykeys_devpkey.h, combaseapi.h, objbase.h,
# wrl/*), or d3d12.h is forbidden in:
#   core/, codec/, canvas/, image/, meta/, player/, edit/, and io/*.h
#
# Windows I/O stays in io/*_win.cpp (and the existing io/file.cpp). gfx/ is
# the D3D11 backend and is allowed. shell/ is the Windows host.
#
# Exit 0 clean, 1 on a violation.

[CmdletBinding()]
param(
    [string]$SourceRoot
)

$ErrorActionPreference = 'Stop'

if (-not $SourceRoot) {
    $SourceRoot = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\src'
}

$SourceRoot = (Resolve-Path $SourceRoot).Path

$bannedModules = @('core', 'codec', 'canvas', 'image', 'meta', 'player', 'edit')
$pattern = '^\s*#\s*include\s*[<"](d3d11[^"]*|dxgi[^"]*|windows\.h|atlbase\.h|winuser\.h|audioclient\.h|audiopolicy\.h|mmdeviceapi\.h|mmreg\.h|mmsystem\.h|avrt\.h|endpointvolume\.h|functiondiscoverykeys[^"]*|combaseapi\.h|objbase\.h|wrl/[^"]*|d3d12[^"]*)'

$violations = @()

foreach ($module in $bannedModules) {
    $moduleDir = Join-Path $SourceRoot $module
    if (-not (Test-Path $moduleDir)) { continue }
    $files = Get-ChildItem -Path $moduleDir -Recurse -Include *.h, *.hpp, *.cpp -File
    foreach ($file in $files) {
        if ($file.Name -like '*_win.cpp') { continue }
        $lineNumber = 0
        foreach ($line in (Get-Content -LiteralPath $file.FullName)) {
            $lineNumber++
            if ($line -match $pattern) {
                $violations += [pscustomobject]@{
                    File = $file.FullName.Substring($SourceRoot.Length + 1)
                    Line = $lineNumber
                    Text = $line.Trim()
                }
            }
        }
    }
}

$ioDir = Join-Path $SourceRoot 'io'
if (Test-Path $ioDir) {
    $headers = Get-ChildItem -Path $ioDir -Recurse -Include *.h, *.hpp -File
    foreach ($file in $headers) {
        $lineNumber = 0
        foreach ($line in (Get-Content -LiteralPath $file.FullName)) {
            $lineNumber++
            if ($line -match $pattern) {
                $violations += [pscustomobject]@{
                    File = $file.FullName.Substring($SourceRoot.Length + 1)
                    Line = $lineNumber
                    Text = $line.Trim()
                }
            }
        }
    }
}

if ($violations.Count -eq 0) {
    Write-Host "hostable core: clean (no windows.h / d3d11.h above gfx/)"
    exit 0
}

Write-Host ''
Write-Host 'HOSTABLE-CORE VIOLATIONS — HWND / D3D11 / wchar paths stay in shell/ and *_win.cpp.' -ForegroundColor Red
Write-Host 'plan/15-platforms.md (D9).'
Write-Host ''
foreach ($v in $violations) {
    Write-Host ("  {0}:{1}" -f $v.File, $v.Line) -ForegroundColor Red
    Write-Host ("    {0}" -f $v.Text)
}
Write-Host ''
exit 1
