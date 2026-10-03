// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using Microsoft.UI.Xaml;

namespace MediaViewer.Shared;

/// <summary>
/// Gives an add-on's own window the app mark (title bar, taskbar, Alt+Tab),
/// as the main window has it: icon resource 1 of the running exe
/// (src/shell/mediaviewer.rc, MV_IDI_APP), loaded at the window's DPI for the
/// big and the small size, like main.cpp. The add-on chrome runs inside
/// MediaViewer.exe, so the exe's module is the host's. A WinUI Window has no
/// icon of its own; without this it shows the generic one. Linked into each
/// add-on chrome assembly.
/// </summary>
internal static class AppIcon
{
    private const int IdiApp = 1;  // MV_IDI_APP, src/shell/app_icon.h
    private const uint ImageTypeIcon = 1;
    private const uint WmSetIcon = 0x0080;
    private const int IconSmall = 0, IconBig = 1;
    private const int SmCxIcon = 11, SmCyIcon = 12, SmCxSmIcon = 49, SmCySmIcon = 50;

    // Icons live as long as the process (a window that closes may reopen);
    // one pair per DPI the windows were opened at.
    private static readonly Dictionary<uint, (IntPtr Big, IntPtr Small)> s_byDpi = new();

    public static void Apply(Window window)
    {
        try
        {
            IntPtr hwnd = WinRT.Interop.WindowNative.GetWindowHandle(window);
            uint dpi = GetDpiForWindow(hwnd);
            if (dpi == 0) dpi = 96;
            if (!s_byDpi.TryGetValue(dpi, out (IntPtr Big, IntPtr Small) icons))
            {
                IntPtr exe = GetModuleHandleW(IntPtr.Zero);
                icons = (Load(exe, SmCxIcon, SmCyIcon, dpi), Load(exe, SmCxSmIcon, SmCySmIcon, dpi));
                s_byDpi[dpi] = icons;
            }
            if (icons.Big != IntPtr.Zero) SendMessageW(hwnd, WmSetIcon, IconBig, icons.Big);
            if (icons.Small != IntPtr.Zero) SendMessageW(hwnd, WmSetIcon, IconSmall, icons.Small);
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException)
        {
            // Cosmetic: the window keeps the generic icon.
        }
    }

    private static IntPtr Load(IntPtr module, int cxMetric, int cyMetric, uint dpi) =>
        LoadImageW(module, IdiApp, ImageTypeIcon,
                   GetSystemMetricsForDpi(cxMetric, dpi), GetSystemMetricsForDpi(cyMetric, dpi), 0);

    [DllImport("kernel32.dll")]
    private static extern IntPtr GetModuleHandleW(IntPtr name);

    [DllImport("user32.dll")]
    private static extern IntPtr LoadImageW(IntPtr instance, nint name, uint type, int cx, int cy, uint flags);

    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(IntPtr hwnd);

    [DllImport("user32.dll")]
    private static extern int GetSystemMetricsForDpi(int index, uint dpi);

    [DllImport("user32.dll")]
    private static extern IntPtr SendMessageW(IntPtr hwnd, uint msg, nint wParam, IntPtr lParam);
}
