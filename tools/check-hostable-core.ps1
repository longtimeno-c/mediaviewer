# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# D9 / docs/design/15-platforms.md: from PR 4, new native code above gfx/ does not
# include platform headers the other host cannot replace. Two mirrored rules:
#
# Windows. Direct #include of d3d11.h, dxgi.h, windows.h, atlbase.h, the
# WASAPI/COM headers (audioclient.h, mmdeviceapi.h, audiopolicy.h, mmreg.h,
# avrt.h, endpointvolume.h, functiondiscoverykeys_devpkey.h, combaseapi.h,
# objbase.h, wrl/*), d3d12.h, or an FFmpeg hwcontext header that drags d3d11.h
# in (libavutil/hwcontext_d3d11va.h, hwcontext_dxva2.h). Allowed only in
# *_win.cpp.
#
# Cocoa / Metal. #include or #import of Foundation, AppKit / Cocoa / UIKit /
# SwiftUI, Metal / MetalKit / QuartzCore, CoreVideo / CoreMedia / CoreImage /
# VideoToolbox / IOSurface, AVFoundation / AVKit, or Core Audio / AudioToolbox.
# Allowed only in the Mac port files: *_mac.* and *_metal.*, plus *_posix.*,
# the POSIX ports that guard their Apple-only lines with __APPLE__.
#
# Both apply in:
#   core/, codec/, canvas/, image/, meta/, player/, edit/, addon/, addons/,
#   infer/, and io/*.h
# and to .h, .hpp, .cpp, .mm and .m files.
#
# Platform I/O stays in io/*_win.cpp / io/*_mac.cpp (and the existing
# io/file.cpp). gfx/ is the D3D11 / Metal backend and is allowed. shell/ holds
# both hosts. nle/mac is the Mac-only Final Cut Pro agent (docs/design/23).
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

$bannedModules = @('core', 'codec', 'canvas', 'image', 'meta', 'player', 'edit', 'addon', 'addons', 'infer')

$rules = @(
    [pscustomobject]@{
        Name    = 'Windows'
        Pattern = '^\s*#\s*(include|import)\s*[<"](d3d11[^"]*|dxgi[^"]*|windows\.h|atlbase\.h|winuser\.h|audioclient\.h|audiopolicy\.h|mmdeviceapi\.h|mmreg\.h|mmsystem\.h|avrt\.h|endpointvolume\.h|functiondiscoverykeys[^"]*|combaseapi\.h|objbase\.h|wrl/[^"]*|d3d12[^"]*|libavutil/hwcontext_d3d11va\.h|libavutil/hwcontext_dxva2\.h)'
        Exempt  = '_win\.cpp$'
    },
    [pscustomobject]@{
        Name    = 'Cocoa/Metal'
        Pattern = '^\s*#\s*(include|import)\s*[<"](Foundation|AppKit|Cocoa|UIKit|SwiftUI|Metal|MetalKit|MetalPerformanceShaders|QuartzCore|CoreVideo|CoreMedia|CoreImage|VideoToolbox|IOSurface|AVFoundation|AVKit|AudioToolbox|CoreAudio)/'
        Exempt  = '_(mac|metal|posix)\.[A-Za-z]+$'
    }
)

$violations = @()

function Test-File([System.IO.FileInfo]$file) {
    $found = @()
    $lineNumber = 0
    foreach ($line in (Get-Content -LiteralPath $file.FullName)) {
        $lineNumber++
        foreach ($rule in $rules) {
            if ($file.Name -match $rule.Exempt) { continue }
            if ($line -match $rule.Pattern) {
                $found += [pscustomobject]@{
                    File = $file.FullName.Substring($SourceRoot.Length + 1)
                    Line = $lineNumber
                    Rule = $rule.Name
                    Text = $line.Trim()
                }
            }
        }
    }
    return $found
}

foreach ($module in $bannedModules) {
    $moduleDir = Join-Path $SourceRoot $module
    if (-not (Test-Path $moduleDir)) { continue }
    $files = Get-ChildItem -Path $moduleDir -Recurse -Include *.h, *.hpp, *.cpp, *.mm, *.m -File
    foreach ($file in $files) {
        $violations += @(Test-File $file)
    }
}

$ioDir = Join-Path $SourceRoot 'io'
if (Test-Path $ioDir) {
    $headers = Get-ChildItem -Path $ioDir -Recurse -Include *.h, *.hpp -File
    foreach ($file in $headers) {
        $violations += @(Test-File $file)
    }
}

if ($violations.Count -eq 0) {
    Write-Host "hostable core: clean (no Windows or Cocoa/Metal headers above gfx/ outside the port files)"
    exit 0
}

Write-Host ''
Write-Host 'HOSTABLE-CORE VIOLATIONS: HWND / D3D11 / wchar paths stay in shell/ and *_win.cpp; Cocoa / Metal stay in shell/, gfx/ and *_mac.*.' -ForegroundColor Red
Write-Host 'docs/design/15-platforms.md (D9).'
Write-Host ''
foreach ($v in $violations) {
    Write-Host ("  {0}:{1} [{2}]" -f $v.File, $v.Line, $v.Rule) -ForegroundColor Red
    Write-Host ("    {0}" -f $v.Text)
}
Write-Host ''
exit 1
