// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
namespace MediaViewer.Shared;

/// <summary>
/// #510 (docs/design/18 "Settings → Add-ons"): the words of Settings → Add-ons'
/// list, one row per add-on with its state in a few words, and of the page each
/// row opens. The same on both platforms: the Mac's copy is AddonListView.swift's
/// <c>AddonListWords</c>, and Chrome.Tests checks that every string here is
/// there too. Pure, no UI.
/// </summary>
public static class AddonListWords
{
    public const string Heading = "Add-ons";
    public const string Back = "All add-ons";
    public const string FromOthers = "From other makers";
    public const string Checking = "Checking…";
    public const string NotInstalled = "Not installed";
    public const string Installing = "Installing…";
    public const string NeedsUpdate = "Needs an update";
    public const string CouldNotStart = "Couldn't start";
    public const string Restart = "Restart to finish updating";

    public static string Installed(string version) => version.Length == 0 ? "Installed" : $"Installed · {version}";

    public static string Update(string version) => $"Update available · {version}";

    public static string Others(int count) => count switch
    {
        0 => "None installed",
        1 => "1 installed",
        _ => $"{count} installed",
    };

    /// <summary>
    /// A row's state. <paramref name="known"/>: what is installed has been read;
    /// <paramref name="state"/>: the store's "ok", "needs_update" or "invalid";
    /// <paramref name="update"/>: a newer published version, or null;
    /// <paramref name="restart"/>: an update waits on a restart; <paramref name="busy"/>:
    /// a download or install of it runs.
    /// </summary>
    public static string Line(bool known, bool installed, string version, string state, string? update,
                              bool restart, bool busy)
    {
        if (!known) return Checking;
        if (busy) return Installing;
        if (restart) return Restart;
        if (!installed) return NotInstalled;
        if (state == "needs_update") return NeedsUpdate;
        if (state != "ok") return CouldNotStart;
        if (update is not null) return Update(update);
        return Installed(version);
    }
}
