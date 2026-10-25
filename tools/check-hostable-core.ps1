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
# Linux (D9 amended 2026-10-23, docs/plans/linux.md). #include of EGL / GL /
# GLES / KHR / epoxy, the Wayland and xkbcommon headers, X11 / xcb, the DRM,
# GBM, VA-API and VDPAU headers, PipeWire / SPA / PulseAudio / ALSA, udev,
# systemd, D-Bus, GLib / GIO / GTK / GDK, Qt, BlueZ, Linux's own <linux/...>
# and the Linux-only <sys/...> interfaces (inotify, epoll, eventfd, signalfd,
# timerfd, sendfile), or an FFmpeg hwcontext header for VA-API / VDPAU / DRM.
# Allowed only in the Linux port files: *_linux.*, *_posix.*, and the OpenGL
# backend's *_gl.*, *_egl.*, *_wayland.*. Qt's own include style, <QString>,
# <QWindow> (and the QPA headers, <qpa/...>), is a second rule matched case-
# sensitively, so it never catches <queue>; <QtGui/QWindow> is in the first.
#
# The three apply in:
#   core/, codec/, canvas/, image/, meta/, player/, edit/, addon/, addons/,
#   geo/, infer/, and io/*.h
# and to .h, .hpp, .cpp, .mm and .m files.
#
# Vulkan (D9: no Vulkan on any OS). <vulkan/...>, volk, VMA or FFmpeg's Vulkan
# hwcontext, anywhere under src/ (shell/ and gfx/ too), with no exempt file.
#
# Platform I/O stays in io/*_win.cpp / io/*_mac.cpp / io/*_linux.cpp (and the
# existing io/file.cpp). gfx/ is the D3D11 / Metal / OpenGL backend and is
# allowed. shell/ holds the hosts. nle/mac is the Mac-only Final Cut Pro agent
# (docs/design/23).
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

$bannedModules = @('core', 'codec', 'canvas', 'image', 'meta', 'player', 'edit', 'addon', 'addons', 'infer', 'geo')

$rules = @(
    [pscustomobject]@{
        Name          = 'Windows'
        CaseSensitive = $false
        Pattern       = '^\s*#\s*(include|import)\s*[<"](d3d11[^"]*|dxgi[^"]*|windows\.h|atlbase\.h|winuser\.h|audioclient\.h|audiopolicy\.h|mmdeviceapi\.h|mmreg\.h|mmsystem\.h|avrt\.h|endpointvolume\.h|functiondiscoverykeys[^"]*|combaseapi\.h|objbase\.h|wrl/[^"]*|d3d12[^"]*|libavutil/hwcontext_d3d11va\.h|libavutil/hwcontext_dxva2\.h)'
        Exempt        = '_win\.cpp$'
    },
    [pscustomobject]@{
        Name          = 'Cocoa/Metal'
        CaseSensitive = $false
        Pattern       = '^\s*#\s*(include|import)\s*[<"](Foundation|AppKit|Cocoa|UIKit|SwiftUI|Metal|MetalKit|MetalPerformanceShaders|QuartzCore|CoreVideo|CoreMedia|CoreImage|VideoToolbox|IOSurface|AVFoundation|AVKit|AudioToolbox|CoreAudio)/'
        Exempt        = '_(mac|metal|posix)\.[A-Za-z]+$'
    },
    [pscustomobject]@{
        Name          = 'Linux (GL/EGL/Wayland/Qt)'
        CaseSensitive = $false
        Pattern       = '^\s*#\s*(include|import)\s*[<"](EGL/|GL/|GLES[0-9]*/|KHR/|epoxy/|wayland-[^">]*\.h|xkbcommon/|X11/|xcb/|gbm\.h|xf86drm|drm/|libdrm/|va/|vdpau/|pipewire/|spa/|pulse/|alsa/|asoundlib\.h|libudev\.h|systemd/|dbus/|gio/|glib\.h|glib-object\.h|gtk/|gdk/|Qt[A-Za-z0-9]*/|bluetooth/|sys/inotify\.h|sys/epoll\.h|sys/eventfd\.h|sys/signalfd\.h|sys/timerfd\.h|sys/sendfile\.h|linux/|libavutil/hwcontext_(vaapi|vdpau|drm)\.h)'
        Exempt        = '_(linux|posix|gl|egl|wayland)\.[A-Za-z]+$'
    },
    [pscustomobject]@{
        Name          = 'Linux (Qt class headers)'
        CaseSensitive = $true
        Pattern       = '^\s*#\s*(include|import)\s*[<"](Q[A-Z][A-Za-z0-9]*[>"/]|qpa/)'
        Exempt        = '_(linux|posix|gl|egl|wayland)\.[A-Za-z]+$'
    }
)

# No exempt file: D9 has no Vulkan on any OS, so this one is checked across
# all of src/, the hosts and gfx/ included.
$vulkanRule = [pscustomobject]@{
    Name          = 'Vulkan (D9)'
    CaseSensitive = $false
    Pattern       = '^\s*#\s*(include|import)\s*[<"](vulkan/|volk\.h|vk_mem_alloc\.h|libavutil/hwcontext_vulkan\.h)'
    Exempt        = '^$'
}

$violations = @()

function Test-File([System.IO.FileInfo]$file, [object[]]$ruleSet = $rules) {
    $found = @()
    $lineNumber = 0
    foreach ($line in (Get-Content -LiteralPath $file.FullName)) {
        $lineNumber++
        foreach ($rule in $ruleSet) {
            if ($file.Name -match $rule.Exempt) { continue }
            # -match ignores case; the Qt class-header rule must not.
            $hit = if ($rule.CaseSensitive) { $line -cmatch $rule.Pattern } else { $line -match $rule.Pattern }
            if ($hit) {
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

$allFiles = Get-ChildItem -Path $SourceRoot -Recurse -Include *.h, *.hpp, *.cpp, *.mm, *.m, *.c -File
foreach ($file in $allFiles) {
    $violations += @(Test-File $file @($vulkanRule))
}

if ($violations.Count -eq 0) {
    Write-Host "hostable core: clean (no Windows, Cocoa/Metal or Linux headers above gfx/ outside the port files; no Vulkan anywhere)"
    exit 0
}

Write-Host ''
Write-Host 'HOSTABLE-CORE VIOLATIONS: HWND / D3D11 / wchar paths stay in shell/ and *_win.cpp; Cocoa / Metal stay in shell/, gfx/ and *_mac.*; Wayland / EGL / GL / Qt stay in shell/, gfx/ and *_linux.* / *_gl.* / *_egl.* / *_wayland.*; Vulkan nowhere.' -ForegroundColor Red
Write-Host 'docs/design/15-platforms.md (D9).'
Write-Host ''
foreach ($v in $violations) {
    Write-Host ("  {0}:{1} [{2}]" -f $v.File, $v.Line, $v.Rule) -ForegroundColor Red
    Write-Host ("    {0}" -f $v.Text)
}
Write-Host ''
exit 1
