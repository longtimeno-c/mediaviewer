// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace MediaViewer.Interop;

/// <summary>Mirrors <c>mv_ai_backend</c> (mediaviewer_ai.h).</summary>
public enum MvAiBackend : uint
{
    Cpu = 0,
    Cuda = 1,
    OpenVino = 2,
    CoreMl = 3,
}

/// <summary>Mirrors <c>mv_ai_compute</c>: Settings → Local search → Compute.</summary>
public enum MvAiCompute : uint
{
    Auto = 0,
    CpuOnly = 1,
    Cuda = 2,
    OpenVino = 3,
    CoreMl = 4,
}

/// <summary>Mirrors <c>mv_ai_quality</c>.</summary>
public enum MvAiQuality : uint
{
    Auto = 0,
    Fast = 1,
    High = 2,
}

/// <summary>Mirrors <c>mv_ai_state</c>.</summary>
public enum MvAiState : uint
{
    Idle = 0,
    Indexing = 1,
    Paused = 2,
    Yielding = 3,
    Loading = 4,
    Error = 5,
}

/// <summary>Mirrors <c>mv_ai_yield</c>.</summary>
public enum MvAiYield : uint
{
    None = 0,
    Viewer = 1,
    Battery = 2,
    Frames = 3,
}

/// <summary>Mirrors <c>mv_ai_scope</c>.</summary>
public enum MvAiScope : uint
{
    Folder = 0,
    Tree = 1,
    All = 2,
}

/// <summary>Mirrors <c>MV_AI_KIND_*</c>.</summary>
[Flags]
public enum MvAiKinds : uint
{
    Photos = 1,
    Videos = 2,
    All = 3,

    // MV_AI_FIND_* (audio, 2026-09-27), OR'ed in: what to look for. None = all.
    FindPictures = 0x10,
    FindSounds = 0x20,
    FindSpeech = 0x40,
}

/// <summary>Mirrors <c>MV_AI_MEDIA_*</c>: what a folder's videos are indexed for.</summary>
public enum MvAiMedia : uint
{
    Default = 0,   // per folder: follow the setting
    Pictures = 1,
    Sound = 2,     // sounds and speech; needs the ai-audio piece
    Both = 3,
}

/// <summary>Mirrors <c>MV_AI_MATCH_*</c>: why a result matched.</summary>
[Flags]
public enum MvAiMatch : uint
{
    None = 0,
    Picture = 1,
    Sound = 2,
    Speech = 4,
}

/// <summary>Mirrors <c>mv_ai_status</c> field for field (1248 bytes).</summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct MvAiStatus
{
    public const uint FlagIndexFull = 1;    // "Index is full — raise the cap or remove a folder"
    public const uint FlagFacesOn = 2;      // the people opt-in is on
    public const uint FlagFacesReady = 4;   // ai-faces is installed and loaded
    public const uint FlagNoModels = 8;     // the pack is installed without its model files
    public const uint FlagAudioReady = 16;  // ai-audio is installed and loaded

    public uint StructSize;
    public MvAiState State;
    public MvAiYield YieldReason;
    public MvAiBackend Backend;
    /// <summary>0 none, 1 not in this build, 2 runtime missing, 3 failed, 4 mismatch vs CPU, 5 slower than CPU.</summary>
    public uint ProviderFault;
    public MvAiQuality Quality;
    public ulong AssetsTotal;
    public ulong AssetsDone;
    public ulong AssetsFailed;
    public ulong FramesIndexed;
    public double AssetsPerSecond;
    public double FramesPerSecond;
    public double EtaLowSeconds;
    public double EtaHighSeconds;
    public ulong IndexBytes;
    public ulong MigrateTotal;
    public ulong MigrateDone;
    public ulong FacesTotal;
    public uint People;
    /// <summary>MV_AI_STATUS_* bits: <see cref="FlagIndexFull"/> and friends.</summary>
    public uint Flags;
    public fixed byte ActiveRoot[1024];
    public fixed byte Model[64];
    // Audio (2026-09-27), appended: clips to index for sound / speech, and done.
    public ulong SoundTotal;
    public ulong SoundDone;
    public ulong SpeechTotal;
    public ulong SpeechDone;

    /// <summary>Display only (rule 6: never logged).</summary>
    public readonly string ActiveRootText
    {
        get { fixed (byte* p = ActiveRoot) return Marshal.PtrToStringUTF8((IntPtr)p) ?? ""; }
    }

    public readonly string ModelText
    {
        get { fixed (byte* p = Model) return Marshal.PtrToStringUTF8((IntPtr)p) ?? ""; }
    }
}

/// <summary>Mirrors <c>mv_ai_result</c>: a photo, or a clip's best moment.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct MvAiResult
{
    public ulong AssetId;
    /// <summary>-1 for a photo.</summary>
    public long PtsMs;
    public float Score;
    public MvAiKinds Kind;
    public uint MoreInClip;
    /// <summary>Why it matched at <see cref="PtsMs"/> (was reserved before audio).</summary>
    public MvAiMatch Match;
}

/// <summary>
/// Mirrors <c>mv_ai_api</c> (mediaviewer_ai.h, interface "mv.ai.1"). Field
/// order is the ABI; <see cref="AiApi"/> wraps it with the buffer rules.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct MvAiApi
{
    public uint StructSize;
    public uint Reserved;
    public IntPtr Ctx;

    // state and settings (PR 20, 23)
    public delegate* unmanaged[Cdecl]<IntPtr, MvAiStatus*, MvStatus> Status;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, uint, uint*, MvStatus> SettingsJson;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, byte*, MvStatus> SetSetting;
    public delegate* unmanaged[Cdecl]<IntPtr, uint, MvStatus> Pause;

    // remembered roots (PR 21, 23)
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, uint, uint*, MvStatus> RootsJson;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, uint, ulong*, MvStatus> IndexFolder;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, uint, MvStatus> RootSetEnabled;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, MvStatus> RootRescan;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, MvStatus> RootRemove;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, uint*, MvStatus> FolderCoverage;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, MvStatus> NoteFolderOpened;
    public delegate* unmanaged[Cdecl]<IntPtr, MvStatus> ClearIndex;

    // search (PR 22, 23)
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, byte*, uint, uint, ulong*, MvStatus> SearchText;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, long, byte*, uint, uint, ulong*, MvStatus> SearchSimilar;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, uint*, MvStatus> ResultCount;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, uint, MvAiResult*, MvStatus> ResultAt;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, uint, byte*, uint, MvStatus> ResultPath;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, uint, byte*, uint, MvStatus> ResultThumb;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, byte*, long*, float*, uint, uint*, MvStatus> ClipMatches;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, MvStatus> SearchRelease;

    // people (PR 24)
    public delegate* unmanaged[Cdecl]<IntPtr, uint, MvStatus> FacesEnable;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, uint, uint*, MvStatus> PeopleJson;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, byte*, uint, uint*, MvStatus> PersonFacesJson;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, byte*, MvStatus> PersonRename;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, ulong, MvStatus> PersonMerge;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, MvStatus> FaceReject;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong*, uint, ulong*, MvStatus> FaceSplit;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, byte*, uint, ulong*, MvStatus> SearchPerson;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, long, ulong*, MvStatus> SearchThisPerson;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, byte*, uint, MvStatus> FaceThumb;

    // audio (2026-09-27)
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, uint, MvStatus> RootSetMedia;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, uint, byte*, uint, MvStatus> ResultSnippet;
}

/// <summary>
/// The AI pack's table with the buffer rules applied: UTF-8 in, strings out,
/// <see cref="MediaViewerException"/> on a failed status. Methods marked
/// "worker" read the index and must not run on the UI thread. Nothing here
/// logs a path, a query or a name (rule 6).
/// </summary>
public sealed unsafe class AiApi
{
    public const string InterfaceId = "mv.ai.1";

    private readonly MvAiApi* _api;

    public AiApi(IntPtr table)
    {
        if (table == IntPtr.Zero) throw new ArgumentNullException(nameof(table));
        // The POD layouts, pinned against mediaviewer_ai.h: a drift here reads
        // as garbage progress rather than as an error.
        if (sizeof(MvAiStatus) != 1248 || sizeof(MvAiResult) != 32)
            throw new InvalidOperationException("mv_ai_status / mv_ai_result layout drifted");
        _api = (MvAiApi*)table;
        if (_api->StructSize < (uint)sizeof(MvAiApi))
            throw new InvalidOperationException("AI add-on table is older than this chrome");
    }

    private IntPtr Ctx => _api->Ctx;

    private static byte[] Z(string? s) => s is null ? Array.Empty<byte>() : Encoding.UTF8.GetBytes(s + "\0");

    private static void Check(MvStatus s, [CallerMemberName] string what = "")
    {
        // The operation's name only: never its arguments (rule 6).
        if (s != MvStatus.Ok) throw new MediaViewerException(s, $"ai.{what}", 0);
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
        throw new MediaViewerException(MvStatus.Internal, $"ai.{what}", 0);
    }

    private static string ReadPath(Func<IntPtr, uint, MvStatus> call, [CallerMemberName] string what = "")
    {
        byte[] buf = new byte[32 * 1024];
        fixed (byte* p = buf) Check(call((IntPtr)p, (uint)buf.Length), what);
        int end = Array.IndexOf(buf, (byte)0);
        return Encoding.UTF8.GetString(buf, 0, end < 0 ? buf.Length : end);
    }

    private static byte* Opt(byte[] bytes, byte* pinned) => bytes.Length == 0 ? null : pinned;

    // ---- state and settings (no-block) ----
    public MvAiStatus Status()
    {
        MvAiStatus s = default;
        s.StructSize = (uint)sizeof(MvAiStatus);
        Check(_api->Status(Ctx, &s));
        return s;
    }

    public string SettingsJson() => ReadJson((b, c, n) => _api->SettingsJson(Ctx, b, c, n));

    public void SetSetting(string key, string valueJson)
    {
        fixed (byte* k = Z(key))
        fixed (byte* v = Z(valueJson))
        {
            Check(_api->SetSetting(Ctx, k, v));
        }
    }

    public void Pause(bool paused) => Check(_api->Pause(Ctx, paused ? 1u : 0u));

    // ---- roots ----
    /// <summary>Worker: reads the index.</summary>
    public string RootsJson() => ReadJson((b, c, n) => _api->RootsJson(Ctx, b, c, n));

    public ulong IndexFolder(string dir, bool recursive)
    {
        ulong id;
        fixed (byte* p = Z(dir)) Check(_api->IndexFolder(Ctx, p, recursive ? 1u : 0u, &id));
        return id;
    }

    public void RootSetEnabled(ulong root, bool enabled) => Check(_api->RootSetEnabled(Ctx, root, enabled ? 1u : 0u));
    public void RootRescan(ulong root) => Check(_api->RootRescan(Ctx, root));
    public void RootRemove(ulong root) => Check(_api->RootRemove(Ctx, root));

    /// <summary>0 not covered, 1 covered and indexing, 2 covered and complete.</summary>
    public uint FolderCoverage(string dir)
    {
        uint state;
        fixed (byte* p = Z(dir)) Check(_api->FolderCoverage(Ctx, p, &state));
        return state;
    }

    public void NoteFolderOpened(string dir) { fixed (byte* p = Z(dir)) Check(_api->NoteFolderOpened(Ctx, p)); }
    public void ClearIndex() => Check(_api->ClearIndex(Ctx));

    // ---- search ----
    public ulong SearchText(string query, string? scopeDir, MvAiScope scope, MvAiKinds kinds)
    {
        ulong id;
        byte[] dir = Z(scopeDir);
        fixed (byte* q = Z(query))
        fixed (byte* d = dir)
        {
            Check(_api->SearchText(Ctx, q, Opt(dir, d), (uint)scope, (uint)kinds, &id));
        }
        return id;
    }

    public ulong SearchSimilar(string path, long ptsMs, string? scopeDir, MvAiScope scope, MvAiKinds kinds)
    {
        ulong id;
        byte[] dir = Z(scopeDir);
        fixed (byte* p = Z(path))
        fixed (byte* d = dir)
        {
            Check(_api->SearchSimilar(Ctx, p, ptsMs, Opt(dir, d), (uint)scope, (uint)kinds, &id));
        }
        return id;
    }

    public uint ResultCount(ulong search)
    {
        uint n;
        Check(_api->ResultCount(Ctx, search, &n));
        return n;
    }

    public MvAiResult ResultAt(ulong search, uint index)
    {
        MvAiResult r;
        Check(_api->ResultAt(Ctx, search, index, &r));
        return r;
    }

    public string ResultPath(ulong search, uint index) =>
        ReadPath((b, c) => _api->ResultPath(Ctx, search, index, (byte*)b, c));

    /// <summary>Worker: may make the moment's JPEG on a cache miss.</summary>
    public string ResultThumb(ulong search, uint index) =>
        ReadPath((b, c) => _api->ResultThumb(Ctx, search, index, (byte*)b, c));

    /// <summary>Every matching moment of the clip at <paramref name="path"/>, in time order.</summary>
    public long[] ClipMatches(ulong search, string path)
    {
        byte[] p = Z(path);
        uint count = 0;
        fixed (byte* pp = p) Check(_api->ClipMatches(Ctx, search, pp, null, null, 0, &count));
        if (count == 0) return Array.Empty<long>();
        long[] ms = new long[count];
        float[] scores = new float[count];
        fixed (byte* pp = p)
        fixed (long* m = ms)
        fixed (float* s = scores)
        {
            Check(_api->ClipMatches(Ctx, search, pp, m, s, count, &count));
        }
        return count == ms.Length ? ms : ms.AsSpan(0, (int)Math.Min(count, (uint)ms.Length)).ToArray();
    }

    public void SearchRelease(ulong search) => Check(_api->SearchRelease(Ctx, search));

    // ---- people (PR 24) ----
    public void FacesEnable(bool enable) => Check(_api->FacesEnable(Ctx, enable ? 1u : 0u));
    /// <summary>Worker.</summary>
    public string PeopleJson() => ReadJson((b, c, n) => _api->PeopleJson(Ctx, b, c, n));
    /// <summary>Worker.</summary>
    public string PersonFacesJson(ulong person) => ReadJson((b, c, n) => _api->PersonFacesJson(Ctx, person, b, c, n));
    public void PersonRename(ulong person, string name) { fixed (byte* p = Z(name)) Check(_api->PersonRename(Ctx, person, p)); }
    public void PersonMerge(ulong into, ulong from) => Check(_api->PersonMerge(Ctx, into, from));
    public void FaceReject(ulong face) => Check(_api->FaceReject(Ctx, face));

    public ulong FaceSplit(IReadOnlyList<ulong> faces)
    {
        ulong[] ids = faces.ToArray();
        ulong person;
        fixed (ulong* p = ids) Check(_api->FaceSplit(Ctx, p, (uint)ids.Length, &person));
        return person;
    }

    public ulong SearchPerson(ulong person, string? scopeDir, MvAiScope scope)
    {
        ulong id;
        byte[] dir = Z(scopeDir);
        fixed (byte* d = dir) Check(_api->SearchPerson(Ctx, person, Opt(dir, d), (uint)scope, &id));
        return id;
    }

    // ---- audio (2026-09-27) ----
    /// <summary>What a folder's videos are indexed for; Default follows "video_index".</summary>
    public void RootSetMedia(ulong root, MvAiMedia media) => Check(_api->RootSetMedia(Ctx, root, (uint)media));

    /// <summary>Worker: the words that matched, for a speech result; "" otherwise.</summary>
    public string ResultSnippet(ulong search, uint index) =>
        ReadPath((b, c) => _api->ResultSnippet(Ctx, search, index, (byte*)b, c));

    /// <summary>Worker: the JPEG a face was found in (made on a miss); crop it in the view with the face's box.</summary>
    public string FaceThumb(ulong face) =>
        ReadPath((b, c) => _api->FaceThumb(Ctx, face, (byte*)b, c));

    /// <summary>After a piece (ai-faces, ai-cuda) is installed or removed while Core is loaded.</summary>
    public void Reload() => SetSetting("reload", "1");

    /// <summary>"Index anyway": ignore the battery pause until the machine is next on AC
    /// (or the app restarts). Never saved; the threshold setting is unchanged.</summary>
    public void IndexAnyway() => SetSetting("battery_override", "1");

    public ulong SearchThisPerson(string path, long ptsMs)
    {
        ulong id;
        fixed (byte* p = Z(path)) Check(_api->SearchThisPerson(Ctx, p, ptsMs, &id));
        return id;
    }
}
