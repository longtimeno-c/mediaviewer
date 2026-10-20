// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
namespace MediaViewer.Shared;

/// <summary>An add-on update that takes over at the next start (#510).</summary>
public sealed record AddonUpdated(string Name, string Version);

/// <summary>
/// #510 (docs/design/18 "Updates"): the restart card's words, and Settings →
/// Add-ons' marker. The same on both platforms: the Mac's copy is
/// RestartNoticeView.swift's <c>RestartWords</c>, and Chrome.Tests checks that
/// every string here is there too. Pure, no UI.
/// </summary>
public static class RestartWords
{
    // What Restart now waits for, besides what an add-on names itself
    // (busy_post: Driftwire's "a Driftwire transfer").
    public const string Import = "an import";
    public const string Indexing = "Local search indexing";
    public const string Download = "an add-on download";

    public const string Later = "Later";
    public const string RestartNow = "Restart now";
    public const string RestartAnyway = "Restart anyway";
    public const string Cancel = "Cancel";
    public const string LaterTip = "Keep using this version. Settings → Add-ons offers the restart until MediaViewer next starts.";
    public const string WaitingBody = "MediaViewer restarts when it is done, on this folder and file.";

    /// <summary>"Import", "Import and Places", "Import, Places and Driftwire".</summary>
    public static string Join(IReadOnlyList<string> names) => names.Count switch
    {
        0 => "",
        1 => names[0],
        _ => string.Join(", ", names.Take(names.Count - 1)) + " and " + names[^1],
    };

    /// <summary>"Driftwire was updated to 1.4.2." / "Import and Places were updated."</summary>
    public static string Title(IReadOnlyList<AddonUpdated> updates) => updates.Count switch
    {
        0 => "",
        1 => $"{updates[0].Name} was updated to {updates[0].Version}.",
        _ => $"{Join(updates.Select(u => u.Name).ToList())} were updated.",
    };

    public static string Body(int count) =>
        $"Restart MediaViewer to use {(count == 1 ? "it" : "them")}. It opens again on this folder and file.";

    /// <summary>The command bar button's tooltip.</summary>
    public static string Tooltip(IReadOnlyList<AddonUpdated> updates) =>
        updates.Count == 0 ? "" : $"{Title(updates)} Restart MediaViewer to use {(updates.Count == 1 ? "it" : "them")}.";

    /// <summary>Settings → Add-ons, until the next start: "Restart to finish updating Import 1.3.0."</summary>
    public static string Marker(IReadOnlyList<AddonUpdated> updates) =>
        updates.Count == 0 ? "" : $"Restart to finish updating {Join(updates.Select(u => $"{u.Name} {u.Version}").ToList())}.";

    /// <summary>"Waiting for an import and a Driftwire transfer to finish"</summary>
    public static string WaitingTitle(IReadOnlyList<string> what) => $"Waiting for {Join(what)} to finish";

    public static string AnywayTip(IReadOnlyList<string> what) => $"Restart now. This stops {Join(what)}.";

    public const string CancelTip = "Don't restart now. The update waits for the next start.";
}
