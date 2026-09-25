// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace MediaViewer.Interop;

/// <summary>Mirrors <c>mv_clip_op</c> (mediaviewer_clip.h, ABI 0.10).</summary>
public enum MvClipOp : uint
{
    TrimKeyframe = 1,
    TrimReencode = 2,
    Rotate = 3,
    Split = 4,
    RemoveMiddle = 5,
    Remux = 6,
    Frame = 7,
    Audio = 8,
    Animation = 9,
}

/// <summary>Mirrors <c>mv_clip_job_state</c>.</summary>
public enum MvClipJobState : uint
{
    Queued = 1,
    Running = 2,
    Done = 3,
    Failed = 4,
    Cancelled = 5,
}

/// <summary>Mirrors <c>mv_clip_progress</c> (368 bytes).</summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct MvClipProgress
{
    public ulong JobId;
    public MvClipJobState State;
    public MvClipOp Op;
    public double Fraction;
    public long ElapsedMs;
    public long EtaMs;
    public MvStatus Error;
    public uint OutputCount;
    public fixed byte Title[64];
    public fixed byte SourceName[256];

    public string TitleText
    {
        get { fixed (byte* p = Title) return Marshal.PtrToStringUTF8((IntPtr)p) ?? string.Empty; }
    }

    public string SourceNameText
    {
        get { fixed (byte* p = SourceName) return Marshal.PtrToStringUTF8((IntPtr)p) ?? string.Empty; }
    }
}

internal static partial class NativeMethods
{
    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial MvStatus mv_clip_cancel(MvSessionHandle session, ulong jobId);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial MvStatus mv_clip_retry(MvSessionHandle session, ulong jobId, out ulong newJobId);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial MvStatus mv_clip_jobs(MvSessionHandle session, [Out] ulong[]? ids, uint cap,
                                                  out uint count);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial MvStatus mv_clip_job_progress(MvSessionHandle session, ulong jobId,
                                                          out MvClipProgress progress);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial MvStatus mv_clip_job_output(MvSessionHandle session, ulong jobId, uint index,
                                                        IntPtr utf8, uint cap, out uint outBytes);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial MvStatus mv_clip_clear_finished(MvSessionHandle session);
}

/// <summary>
/// PR 13 / 14: the clip job queue, for the Jobs pane. Native submits (it owns
/// trim mode and the clip tools); the pane lists, cancels, retries and reveals.
/// Every call is [no-block] (mediaviewer_clip.h), so the pane polls it on its
/// UI timer.
/// </summary>
public sealed partial class MediaViewerSession
{
    public ulong[] ClipJobs()
    {
        ThrowIfFailed(NativeMethods.mv_clip_jobs(_handle, null, 0, out uint count));
        if (count == 0) return Array.Empty<ulong>();
        var ids = new ulong[count];
        ThrowIfFailed(NativeMethods.mv_clip_jobs(_handle, ids, count, out uint again));
        return again < count ? ids[..(int)again] : ids;
    }

    /// <summary>False when the job is gone (cleared); never throws for that.</summary>
    public bool TryClipProgress(ulong jobId, out MvClipProgress progress) =>
        NativeMethods.mv_clip_job_progress(_handle, jobId, out progress) == MvStatus.Ok;

    public string ClipJobOutput(ulong jobId, uint index)
    {
        uint need = 0;
        NativeMethods.mv_clip_job_output(_handle, jobId, index, IntPtr.Zero, 0, out need);
        if (need <= 1) return string.Empty;
        IntPtr buf = Marshal.AllocHGlobal((int)need);
        try
        {
            if (NativeMethods.mv_clip_job_output(_handle, jobId, index, buf, need, out _) != MvStatus.Ok)
                return string.Empty;
            return Marshal.PtrToStringUTF8(buf) ?? string.Empty;
        }
        finally
        {
            Marshal.FreeHGlobal(buf);
        }
    }

    /// <summary>False if the job had already finished.</summary>
    public bool ClipCancel(ulong jobId) => NativeMethods.mv_clip_cancel(_handle, jobId) == MvStatus.Ok;

    /// <summary>0 if the job cannot be retried.</summary>
    public ulong ClipRetry(ulong jobId) =>
        NativeMethods.mv_clip_retry(_handle, jobId, out ulong id) == MvStatus.Ok ? id : 0;

    public void ClipClearFinished() => ThrowIfFailed(NativeMethods.mv_clip_clear_finished(_handle));
}
