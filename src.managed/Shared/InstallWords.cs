// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
namespace MediaViewer.Shared;

/// <summary>Why an add-on install did not finish (#525), in the order a person can act on.</summary>
public enum InstallFailure
{
    /// <summary>The download did not arrive: no connection, the server went away.</summary>
    Offline,
    /// <summary>The bytes that arrived are not the signed ones (size, SHA-256, manifest).</summary>
    Damaged,
    /// <summary>The store refused the unpacked files (a signature or a hash).</summary>
    NotVerified,
    /// <summary>The disk is full.</summary>
    NoSpace,
    /// <summary>Installed, but it did not load or attach.</summary>
    NotStarted,
    /// <summary>Anything else.</summary>
    Failed,
}

/// <summary>The one next step a failure offers.</summary>
public enum InstallNext { Retry, Reinstall }

/// <summary>
/// #525 (docs/design/18 "Installing"): the words of every add-on install on
/// both platforms, Import, Local search's pieces, Places, Driftwire and other
/// slot add-ons alike. One flow: check → download (bytes) → verify → install →
/// attach live, or the restart card (RestartWords). A failure says what
/// happened in plain words and offers one next step; the exception's type goes
/// to the log, never on screen (#509). Also the "updates available" notice.
/// The Mac's copy is InstallWords.swift's <c>InstallWords</c>, and Chrome.Tests
/// checks that every string here is there too. Pure, no UI.
/// </summary>
public static class InstallWords
{
    // The steps, under the bar.
    public const string Connecting = "Connecting…";
    public const string Verifying = "Checking the download…";
    public const string VerifyingFile = "Checking the file…";  // an add-on package opened from disk
    public const string Installing = "Installing…";
    public const string Starting = "Starting…";

    /// <summary>"412 MB of 1.08 GB" (sizes already written by ByteSize).</summary>
    public static string Downloaded(string done, string total) => $"{done} of {total}";

    // What the channel says before anything is installed.
    public const string Unreachable = "Could not reach the download server.";

    public static string CheckingFor(string name) => $"Checking for {name}…";

    public static string NotPublished(string name) =>
        $"{name} is not published for download yet. It will be offered here once a release carries it.";

    public static string NeedsNewerApp(string name) =>
        $"The published {name} needs a newer MediaViewer. Update MediaViewer, then install it.";

    // The buttons.
    public static string Install(string name, string size) => $"Install {name}, {size}";

    public static string UpdateTo(string version, string size) => $"Update to {version}, {size}";

    public const string Retry = "Retry";
    public const string Reinstall = "Remove and install again";
    public const string ReinstallTip = "Removes the installed copy, keeps its data and settings, and downloads it again.";

    // The outcome line.
    public static string Downloading(string name, string? version) =>
        version is null ? $"Downloading {name}…" : $"Downloading {name} {version}…";

    public static string Installed(string name) => $"{name} installed.";

    public static string Updated(string name, string version) => $"{name} updated to {version}.";

    public static string Reinstalled(string name) => $"{name} installed again.";

    public static string Failure(InstallFailure failure, string name) => failure switch
    {
        InstallFailure.Offline => $"{name} could not be downloaded. Check the connection, then retry.",
        InstallFailure.Damaged => $"The download of {name} was damaged, so nothing was installed.",
        InstallFailure.NotVerified => $"{name} did not pass verification, so nothing was installed.",
        InstallFailure.NoSpace => $"There is not enough free space to install {name}.",
        InstallFailure.NotStarted => $"{name} is installed but could not start.",
        _ => $"{name} could not be installed.",
    };

    /// <summary>
    /// Retry, unless the copy on disk is itself unusable (<paramref name="broken"/>:
    /// it did not verify, or it was installed and would not start): then
    /// Remove and install again. A working copy whose update failed is never
    /// removed; it keeps running and Retry downloads the update again.
    /// </summary>
    public static InstallNext Next(InstallFailure failure, bool broken) =>
        broken || failure == InstallFailure.NotStarted ? InstallNext.Reinstall : InstallNext.Retry;

    public static string NextLabel(InstallNext next) => next == InstallNext.Reinstall ? Reinstall : Retry;

    // Updates available (the bar's notice; checked in the background only while
    // "Check for updates automatically" is on, docs/design/13).
    public static string UpdatesBadge(int count) =>
        count == 1 ? "Add-on update available" : $"{count} add-on updates available";

    /// <summary>"Places 1.1.0 is available. Open Settings → Add-ons to update it."</summary>
    public static string UpdatesTip(IReadOnlyList<AddonUpdated> updates) => updates.Count switch
    {
        0 => "",
        1 => $"{updates[0].Name} {updates[0].Version} is available. Open Settings → Add-ons to update it.",
        _ => $"{RestartWords.Join(updates.Select(u => $"{u.Name} {u.Version}").ToList())} are available. Open Settings → Add-ons to update them.",
    };
}
