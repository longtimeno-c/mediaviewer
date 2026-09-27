// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using System.Text;

namespace MediaViewer.Updater;

/// <summary>
/// The start-attempt record shared with the native host (src/shell/update_guard.h).
/// File: <c>&lt;install root&gt;\updater\trial.ini</c>. The managed updater writes
/// <c>[trial]</c> just before it hands a staged package to Update.exe; the native
/// host counts starts of that version before anything that can crash, clears the
/// section after the chrome has attached and a few seconds have passed, and
/// rolls back after two failed starts. Both sides use the Win32 profile API so
/// they agree on encoding. Versions and one package filename only (rule 6).
/// </summary>
public static class TrialState
{
    public const string Section = "trial";
    public const string FailedSection = "failed";

    public static string UpdaterDir(string installRoot) => Path.Combine(installRoot, "updater");
    public static string FilePath(string installRoot) => Path.Combine(UpdaterDir(installRoot), "trial.ini");
    public static string RollbackDir(string installRoot) => Path.Combine(UpdaterDir(installRoot), "rollback");

    /// <summary>Arms the guard for <paramref name="target"/>. Called on the updater worker.</summary>
    public static void Arm(string installRoot, ReleaseVersion target, ReleaseVersion prior, string? priorPackageFile)
    {
        Directory.CreateDirectory(UpdaterDir(installRoot));
        string file = FilePath(installRoot);
        Write(file, Section, "version", target.ToString());
        Write(file, Section, "prior_version", prior.ToString());
        Write(file, Section, "prior_package", priorPackageFile ?? "");
        Write(file, Section, "attempts", "0");
    }

    /// <summary>Versions this machine rolled back ("failed to start twice").</summary>
    public static List<ReleaseVersion> FailedVersions(string installRoot)
    {
        var list = new List<ReleaseVersion>();
        string file = FilePath(installRoot);
        if (!File.Exists(file)) return list;
        foreach (string v in Read(file, FailedSection, "versions").Split(',', StringSplitOptions.RemoveEmptyEntries))
        {
            if (ReleaseVersion.TryParse(v.Trim(), out ReleaseVersion rv)) list.Add(rv);
        }
        return list;
    }

    /// <summary>A rollback the user has not been told about yet, or null. Marks it told.</summary>
    public static string? TakeUnreportedRollback(string installRoot)
    {
        string file = FilePath(installRoot);
        if (!File.Exists(file)) return null;
        string from = Read(file, FailedSection, "unreported");
        if (from.Length == 0) return null;
        Write(file, FailedSection, "unreported", null);
        return from;
    }

    internal static string Read(string file, string section, string key)
    {
        var sb = new StringBuilder(1024);
        _ = GetPrivateProfileStringW(section, key, "", sb, sb.Capacity, file);
        return sb.ToString();
    }

    internal static void Write(string file, string section, string key, string? value)
    {
        if (!WritePrivateProfileStringW(section, key, value, file))
            throw new IOException("trial.ini write failed", Marshal.GetHRForLastWin32Error());
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetPrivateProfileStringW(string section, string key, string def,
        StringBuilder value, int size, string file);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool WritePrivateProfileStringW(string section, string key, string? value, string file);
}
