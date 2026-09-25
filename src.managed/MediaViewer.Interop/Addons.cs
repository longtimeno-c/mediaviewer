// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace MediaViewer.Interop;

/// <summary>Mirrors <c>mv_addon_event_kind</c> (mediaviewer_addon.h).</summary>
public enum MvAddonEvent : uint
{
    None = 0,
    ScanDone = 1,
    PlanReady = 2,
    JobProgress = 3,
    JobDone = 4,
    VolumeArrived = 5,
    VolumeRemoved = 6,
    VerifyDone = 7,
}

/// <summary>Mirrors <c>mv_import_job_state</c>.</summary>
public enum MvImportJobState : uint
{
    None = 0,
    Queued = 1,
    Running = 2,
    Paused = 3,
    Done = 4,
    Failed = 5,
    Cancelled = 6,
    Interrupted = 7,
}

/// <summary>Mirrors <c>mv_import_arrival_action</c>.</summary>
public enum MvImportArrival : long
{
    Nothing = 0,
    Open = 1,
    Auto = 2,
}

/// <summary>An add-on completion as it arrives on the session queue.</summary>
public readonly record struct AddonCompletion(MvAddonEvent Event, MvStatus Status, ulong Id, long Payload)
{
    /// <summary>MV_COMPLETION_ADDON: the event kind rides in <c>generation</c>.</summary>
    public const uint CompletionKind = 100;

    public static bool TryFrom(in MvCompletion c, out AddonCompletion e)
    {
        e = default;
        if ((uint)c.Kind != CompletionKind) return false;
        e = new AddonCompletion((MvAddonEvent)c.Generation, c.Status, c.JobId, c.Payload);
        return true;
    }
}

/// <summary>Mirrors <c>mv_import_progress</c> field for field.</summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct MvImportProgress
{
    public ulong JobId;
    public MvImportJobState State;
    public uint DestinationCount;
    public uint UnitsTotal;
    public uint UnitsDone;
    public uint UnitsSkipped;
    public uint UnitsFailed;
    public ulong BytesTotal;
    public ulong BytesRead;
    public fixed ulong BytesVerified[2];
    public double BytesPerSecond;
    public long EtaSeconds;
    public long ElapsedMs;
    public fixed byte CurrentName[256];

    public readonly string CurrentNameText
    {
        get
        {
            fixed (byte* p = CurrentName)
            {
                return Marshal.PtrToStringUTF8((IntPtr)p) ?? "";
            }
        }
    }
}

/// <summary>
/// Mirrors <c>mv_import_api</c> (mediaviewer_import.h): the Import add-on's
/// function table, obtained with <see cref="AddonNative.Load"/>. Field order
/// is the ABI; <see cref="ImportApi"/> wraps it with the buffer rules.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct MvImportApi
{
    public uint StructSize;
    public uint Reserved;
    public IntPtr Ctx;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, uint, uint*, MvStatus> SourcesJson;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, MvStatus> AddFolderSource;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, MvStatus> RemoveFolderSource;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, byte*, uint, MvStatus> ArrivalRoot;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, ulong*, MvStatus> Scan;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, ulong*, MvStatus> ScanFiles;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, byte*, byte*, ulong*, MvStatus> Plan;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, byte*, uint, uint*, MvStatus> PlanJson;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, int, byte*, uint, MvStatus> Select;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, uint, byte*, uint, MvStatus> Thumbnail;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, ulong*, MvStatus> Start;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, ulong*, MvStatus> ImportNow;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, uint, MvStatus> Pause;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, MvStatus> Cancel;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, uint, MvStatus> SetPriority;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, MvImportProgress*, MvStatus> Progress;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, byte*, uint, uint*, MvStatus> SummaryJson;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, ulong*, MvStatus> RetryFailed;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, uint, uint*, MvStatus> UnfinishedJson;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, MvStatus> Resume;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, byte*, uint, MvStatus> ReportPath;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, MvStatus> Eject;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, uint, uint*, MvStatus> PresetsJson;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, MvStatus> SavePreset;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, MvStatus> DeletePreset;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, byte*, uint, MvStatus> BindCard;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, byte*, uint, uint*, MvStatus> PreviewNamesJson;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, uint, uint*, MvStatus> HistoryJson;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, ulong*, MvStatus> VerifyFolder;
}

/// <summary>
/// The Import add-on's table with the buffer rules applied: UTF-8 in, strings
/// out, <see cref="MediaViewerException"/> on a failed status. Methods marked
/// "worker" read files or import.db and must not run on the UI thread.
/// </summary>
public sealed unsafe class ImportApi
{
    private readonly MvImportApi* _api;

    public ImportApi(IntPtr table)
    {
        if (table == IntPtr.Zero) throw new ArgumentNullException(nameof(table));
        _api = (MvImportApi*)table;
        if (_api->StructSize < (uint)sizeof(MvImportApi))
            throw new InvalidOperationException("Import add-on table is older than this chrome");
    }

    private IntPtr Ctx => _api->Ctx;

    private static byte[] Z(string? s) => s is null ? Array.Empty<byte>() : Encoding.UTF8.GetBytes(s + "\0");

    private static void Check(MvStatus s, [CallerMemberName] string what = "")
    {
        if (s != MvStatus.Ok) throw new MediaViewerException(s, $"import.{what}", 0);
    }

    private delegate MvStatus JsonCall(byte* buf, uint cap, uint* needed);

    private static string ReadJson(JsonCall call, [CallerMemberName] string what = "")
    {
        uint cap = 64 * 1024;
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            byte[] buf = new byte[cap];
            uint needed = 0;
            MvStatus s;
            fixed (byte* p = buf) s = call(p, cap, &needed);
            if (s == MvStatus.Ok) return Encoding.UTF8.GetString(buf, 0, (int)Math.Max(0, needed - 1));
            if (s != MvStatus.InvalidArg || needed <= cap) Check(s, what);
            cap = needed + 1024;
        }
        throw new MediaViewerException(MvStatus.Internal, $"import.{what}", 0);
    }

    private static string ReadPath(Func<IntPtr, uint, MvStatus> call, [CallerMemberName] string what = "")
    {
        byte[] buf = new byte[32 * 1024];
        fixed (byte* p = buf) Check(call((IntPtr)p, (uint)buf.Length), what);
        return Encoding.UTF8.GetString(buf, 0, Array.IndexOf(buf, (byte)0));
    }

    // ---- sources (worker) ----
    public string SourcesJson() => ReadJson((b, c, n) => _api->SourcesJson(Ctx, b, c, n));
    public void AddFolderSource(string dir) { fixed (byte* p = Z(dir)) Check(_api->AddFolderSource(Ctx, p)); }
    public void RemoveFolderSource(string dir) { fixed (byte* p = Z(dir)) Check(_api->RemoveFolderSource(Ctx, p)); }
    public string ArrivalRoot(ulong seq) => ReadPath((b, c) => _api->ArrivalRoot(Ctx, seq, (byte*)b, c));

    // ---- scan and plan (no-block) ----
    public ulong Scan(string root) { ulong id; fixed (byte* p = Z(root)) Check(_api->Scan(Ctx, p, &id)); return id; }
    public ulong ScanFiles(string pathsJson) { ulong id; fixed (byte* p = Z(pathsJson)) Check(_api->ScanFiles(Ctx, p, &id)); return id; }

    public ulong Plan(ulong scan, string? presetJson, string? markedJson)
    {
        ulong id;
        byte[] preset = Z(presetJson);
        byte[] marked = Z(markedJson);
        fixed (byte* pp = preset)
        fixed (byte* pm = marked)
        {
            Check(_api->Plan(Ctx, scan, presetJson is null ? null : pp, markedJson is null ? null : pm, &id));
        }
        return id;
    }

    public string PlanJson(ulong plan) => ReadJson((b, c, n) => _api->PlanJson(Ctx, plan, b, c, n));

    public void Select(ulong plan, int unit, string? day, bool selected)
    {
        byte[] d = Z(day);
        fixed (byte* p = d) Check(_api->Select(Ctx, plan, unit, day is null ? null : p, selected ? 1u : 0u));
    }

    /// <summary>Worker: may decode the file to make the thumbnail.</summary>
    public string Thumbnail(ulong plan, uint unit) => ReadPath((b, c) => _api->Thumbnail(Ctx, plan, unit, (byte*)b, c));

    // ---- jobs (no-block unless noted) ----
    public ulong Start(ulong plan) { ulong id; Check(_api->Start(Ctx, plan, &id)); return id; }
    public ulong ImportNow(string pathsJson) { ulong id; fixed (byte* p = Z(pathsJson)) Check(_api->ImportNow(Ctx, p, &id)); return id; }
    public void Pause(ulong job, bool paused) => Check(_api->Pause(Ctx, job, paused ? 1u : 0u));
    public void Cancel(ulong job) => Check(_api->Cancel(Ctx, job));
    public void SetPriority(ulong job, bool fast) => Check(_api->SetPriority(Ctx, job, fast ? 1u : 0u));

    public MvImportProgress Progress(ulong job)
    {
        MvImportProgress p;
        Check(_api->Progress(Ctx, job, &p));
        return p;
    }

    public string SummaryJson(ulong job) => ReadJson((b, c, n) => _api->SummaryJson(Ctx, job, b, c, n));
    public ulong RetryFailed(ulong job) { ulong id; Check(_api->RetryFailed(Ctx, job, &id)); return id; }
    public string UnfinishedJson() => ReadJson((b, c, n) => _api->UnfinishedJson(Ctx, b, c, n));
    public void Resume(ulong job) => Check(_api->Resume(Ctx, job));
    public string ReportPath(ulong job) => ReadPath((b, c) => _api->ReportPath(Ctx, job, (byte*)b, c));
    /// <summary>Worker: unmount and eject can take seconds.</summary>
    public void Eject(string root) { fixed (byte* p = Z(root)) Check(_api->Eject(Ctx, p)); }

    // ---- presets (worker) ----
    public string PresetsJson() => ReadJson((b, c, n) => _api->PresetsJson(Ctx, b, c, n));
    public void SavePreset(string json) { fixed (byte* p = Z(json)) Check(_api->SavePreset(Ctx, p)); }
    public void DeletePreset(string name) { fixed (byte* p = Z(name)) Check(_api->DeletePreset(Ctx, p)); }

    public void BindCard(string volumeId, string preset, bool autoImport)
    {
        fixed (byte* v = Z(volumeId))
        fixed (byte* p = Z(preset))
        {
            Check(_api->BindCard(Ctx, v, p, autoImport ? 1u : 0u));
        }
    }

    public string PreviewNamesJson(string presetJson)
    {
        byte[] preset = Z(presetJson);
        return ReadJson((b, c, n) => { fixed (byte* p = preset) return _api->PreviewNamesJson(Ctx, p, b, c, n); });
    }

    // ---- library tools ----
    public string HistoryJson() => ReadJson((b, c, n) => _api->HistoryJson(Ctx, b, c, n));
    public ulong VerifyFolder(string dir) { ulong id; fixed (byte* p = Z(dir)) Check(_api->VerifyFolder(Ctx, p, &id)); return id; }
}

/// <summary>
/// The core's add-on management (mediaviewer_addon.h, host side). The core
/// never downloads: the chrome fetches, the core verifies and installs.
/// Everything here but <see cref="SetPresentBusy"/> reads or hashes files:
/// worker threads only.
/// </summary>
public static unsafe partial class AddonNative
{
    private const string Library = NativeMethods.Library;

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_addon_installed_json(byte* output, uint cap, uint* needed);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_addon_check_manifest(byte* manifest, uint manifestLen, byte* sig,
        uint sigLen, byte* output, uint cap, uint* needed);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_addon_sha256_file(byte* path, byte* output65);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_addon_make_staging(byte* output, uint cap);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_addon_install(byte* staged);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_addon_remove(byte* id, uint keepData);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_addon_load(IntPtr session, byte* id, byte* iface, IntPtr* outIface,
        byte* chrome, uint chromeCap);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_addon_unload(byte* id);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial void mv_present_set_busy(uint busy);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_volume_watch(IntPtr session, uint enable);

    private static byte[] Z(string s) => Encoding.UTF8.GetBytes(s + "\0");

    private static void Check(MvStatus s, string what)
    {
        if (s == MvStatus.Ok) return;
        string message = Marshal.PtrToStringUTF8(NativeMethods.mv_last_error_message()) ?? what;
        throw new MediaViewerException(s, message, NativeMethods.mv_last_error_correlation_id());
    }

    private static string Json(Func<IntPtr, uint, IntPtr, MvStatus> call, string what)
    {
        uint cap = 16 * 1024;
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            byte[] buf = new byte[cap];
            uint needed = 0;
            MvStatus s;
            fixed (byte* p = buf) s = call((IntPtr)p, cap, (IntPtr)(&needed));
            if (s == MvStatus.Ok) return Encoding.UTF8.GetString(buf, 0, (int)Math.Max(0, needed - 1));
            if (s != MvStatus.InvalidArg || needed <= cap) Check(s, what);
            cap = needed + 256;
        }
        throw new MediaViewerException(MvStatus.Internal, what, 0);
    }

    public static string InstalledJson() =>
        Json((b, c, n) => mv_addon_installed_json((byte*)b, c, (uint*)n), "mv_addon_installed_json");

    public static string CheckManifest(byte[] manifest, byte[]? signature)
    {
        byte[] sig = signature ?? Array.Empty<byte>();
        return Json((b, c, n) =>
        {
            fixed (byte* m = manifest)
            fixed (byte* s = sig)
            {
                return mv_addon_check_manifest(m, (uint)manifest.Length, sig.Length == 0 ? null : s,
                    (uint)sig.Length, (byte*)b, c, (uint*)n);
            }
        }, "mv_addon_check_manifest");
    }

    public static string Sha256File(string path)
    {
        byte* hex = stackalloc byte[65];
        fixed (byte* p = Z(path)) Check(mv_addon_sha256_file(p, hex), "mv_addon_sha256_file");
        return Marshal.PtrToStringUTF8((IntPtr)hex) ?? "";
    }

    public static string MakeStaging()
    {
        byte[] buf = new byte[32 * 1024];
        fixed (byte* p = buf) Check(mv_addon_make_staging(p, (uint)buf.Length), "mv_addon_make_staging");
        return Encoding.UTF8.GetString(buf, 0, Array.IndexOf(buf, (byte)0));
    }

    public static void Install(string stagedDir)
    {
        fixed (byte* p = Z(stagedDir)) Check(mv_addon_install(p), "mv_addon_install");
    }

    public static void Remove(string id, bool keepData)
    {
        fixed (byte* p = Z(id)) Check(mv_addon_remove(p, keepData ? 1u : 0u), "mv_addon_remove");
    }

    /// <summary>Loads (or returns the loaded) add-on; the interface table and the chrome path.</summary>
    public static (IntPtr Interface, string ChromePath) Load(MediaViewerSession session, string id, string interfaceId)
    {
        byte[] chrome = new byte[32 * 1024];
        IntPtr iface = IntPtr.Zero;
        bool added = false;
        session.Handle.DangerousAddRef(ref added);
        try
        {
            fixed (byte* i = Z(id))
            fixed (byte* f = Z(interfaceId))
            fixed (byte* c = chrome)
            {
                Check(mv_addon_load(session.Handle.DangerousGetHandle(), i, f, &iface, c, (uint)chrome.Length),
                    "mv_addon_load");
            }
        }
        finally
        {
            if (added) session.Handle.DangerousRelease();
        }
        return (iface, Encoding.UTF8.GetString(chrome, 0, Array.IndexOf(chrome, (byte)0)));
    }

    public static void Unload(string id)
    {
        fixed (byte* p = Z(id)) Check(mv_addon_unload(p), "mv_addon_unload");
    }

    public static void SetPresentBusy(bool busy) => mv_present_set_busy(busy ? 1u : 0u);

    /// <summary>The base app's card watch for the one-time Import hint.</summary>
    public static void WatchVolumes(MediaViewerSession? session)
    {
        if (session is null)
        {
            Check(mv_volume_watch(IntPtr.Zero, 0), "mv_volume_watch");
            return;
        }
        bool added = false;
        session.Handle.DangerousAddRef(ref added);
        try { Check(mv_volume_watch(session.Handle.DangerousGetHandle(), 1), "mv_volume_watch"); }
        finally { if (added) session.Handle.DangerousRelease(); }
    }
}

/// <summary>
/// What the base chrome gives an add-on's chrome (plan/18: "given the
/// chrome's IAddonHost"). Lives in this assembly so the add-on's own
/// AssemblyLoadContext shares the one type.
/// </summary>
public interface IAddonHost
{
    /// <summary>Queue work on the chrome's UI thread.</summary>
    void Post(Action action);
    /// <summary>Open a file in the main viewer (culling before copying).</summary>
    void OpenInViewer(string path);
    /// <summary>The viewer's marked files, for selection "marked".</summary>
    IReadOnlyList<string> MarkedPaths();
    /// <summary>A progress line for the command bar while a job runs; null hides it.</summary>
    void SetStatus(string? text);
    /// <summary>A system notification when a job finishes (plan/18).</summary>
    void Notify(string title, string body);
}

/// <summary>An add-on chrome's entry point: one public type implementing it.</summary>
public interface IAddonChrome
{
    void Attach(IAddonHost host, IntPtr interfaceTable);
    /// <summary>Open the window, optionally on a source.</summary>
    void Open(string? sourceRoot);
    /// <summary>Ctrl+Shift+F7: import these now with the last preset.</summary>
    void ImportNow(string pathsJson);
    /// <summary>An add-on completion from the session queue.</summary>
    void OnEvent(AddonCompletion e);
    void Shutdown();
}
