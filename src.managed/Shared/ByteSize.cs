// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;

namespace MediaViewer.Shared;

/// <summary>
/// The one way the Windows chrome and its add-on windows write a size (issue
/// #227): decimal units, as download sizes and disks are sold, to three
/// significant figures, as Explorer shows them ("850 bytes", "8.42 MB",
/// "42.1 MB", "412 MB", "1.08 GB").
/// </summary>
internal static class ByteSize
{
    private static readonly string[] Units = { "KB", "MB", "GB", "TB" };

    public static string Text(long bytes)
    {
        if (bytes < 1000) return $"{Math.Max(0, bytes)} bytes";
        double v = bytes / 1000.0;
        int u = 0;
        // Round first, so 999,999 bytes reads "1.00 MB", not "1000 KB".
        while (u < Units.Length - 1 && Math.Round(v) >= 1000)
        {
            v /= 1000;
            ++u;
        }
        string format = Math.Round(v, 2) < 10 ? "0.00" : Math.Round(v, 1) < 100 ? "0.0" : "0";
        return v.ToString(format, CultureInfo.CurrentCulture) + " " + Units[u];
    }
}
