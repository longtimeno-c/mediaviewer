// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;

namespace MediaViewer.Interop;

/// <summary>
/// The server's menu while `driftwire host` runs (docs/plans/self-hosted.md
/// "Tray"): the base app's own kept entry in the notification area, started by
/// the first window's process and stopped at quit (never waiting on a `host`
/// verb in flight). Phone access itself is an add-on (docs/plans/phone-access-addon.md).
/// </summary>
public static partial class HostTrayNative
{
    [LibraryImport(NativeMethods.Library)]
    private static partial MvStatus mv_host_tray_start();

    public static void Start()
    {
        try { _ = mv_host_tray_start(); }
        catch (Exception ex) when (ex is EntryPointNotFoundException or DllNotFoundException) { }
    }
}
