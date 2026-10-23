// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace MediaViewer.Updater;

/// <summary>
/// #532: the other MediaViewer processes of this install. Update.exe's apply
/// kills every process running from the install folder but the one it waits
/// for (Velopack's force_stop_package), so a second window's process, or the
/// one Driftwire keeps in the background, would lose a transfer under way.
/// No apply is launched while one runs; "Update ready — restart" asks them to
/// quit first (<see cref="AskToQuit"/>), and each quits once nothing in it is
/// busy (src/shell/main.cpp, kQuitForUpdateMessage).
/// </summary>
public static class OtherInstances
{
    /// <summary>The registered window message the native host listens for. wParam: the asking pid.</summary>
    public const string QuitForUpdateMessage = "MediaViewer.QuitForUpdate";

    /// <summary>Pids of the other processes running this executable. Pids only (rule 6).</summary>
    public static List<int> Running()
    {
        var pids = new List<int>();
        string? exe = Environment.ProcessPath;
        if (exe is null) return pids;
        int self = Environment.ProcessId;
        foreach (Process p in Process.GetProcessesByName(Path.GetFileNameWithoutExtension(exe)))
        {
            using (p)
            {
                if (p.Id == self) continue;
                try
                {
                    string? path = p.MainModule?.FileName;
                    if (path is not null && string.Equals(path, exe, StringComparison.OrdinalIgnoreCase)) pids.Add(p.Id);
                }
                catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException)
                {
                    // Exited meanwhile, or not ours to read (another user): not this install's.
                }
            }
        }
        return pids;
    }

    /// <summary>Asks every other MediaViewer to quit once it is idle. Top-level windows, hidden ones too.</summary>
    public static void AskToQuit()
    {
        uint msg = RegisterWindowMessageW(QuitForUpdateMessage);
        if (msg != 0) _ = PostMessageW(HwndBroadcast, msg, (nuint)Environment.ProcessId, 0);
    }

    private static readonly IntPtr HwndBroadcast = new(0xFFFF);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint RegisterWindowMessageW(string name);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool PostMessageW(IntPtr hwnd, uint msg, nuint wParam, nint lParam);
}
