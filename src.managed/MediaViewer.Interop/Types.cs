// SPDX-License-Identifier: GPL-2.0-or-later
using System.Runtime.InteropServices;

namespace MediaViewer.Interop;

/// <summary>Mirrors <c>mv_status</c>. The values are part of the ABI.</summary>
public enum MvStatus
{
    Ok = 0,
    InvalidArg = 1,
    OutOfMemory = 2,
    Io = 3,
    UnsupportedFormat = 4,
    Corrupt = 5,
    Cancelled = 6,
    DeviceLost = 7,
    Internal = 8,
}

/// <summary>Mirrors <c>mv_completion_kind</c>.</summary>
public enum MvCompletionKind : uint
{
    None = 0,
    Echo = 1,
    ImageOpened = 2,
}

/// <summary>Mirrors <c>mv_session_config</c>.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct MvSessionConfig
{
    /// <summary>0 selects max(1, cores - 2).</summary>
    public uint WorkerCount;
    public uint EnableEtw;
}

/// <summary>
/// Mirrors <c>mv_completion</c> field for field, including the explicit padding.
/// </summary>
/// <remarks>
/// The size is asserted at startup against the native <c>sizeof</c>. A silent
/// layout change here reads as corrupted job ids rather than as an error, which
/// is precisely the class of bug plan/14-abi.md exists to prevent.
/// </remarks>
[StructLayout(LayoutKind.Sequential)]
public struct MvCompletion
{
    public MvCompletionKind Kind;
    public MvStatus Status;
    public ulong JobId;
    public ulong CorrelationId;
    public uint Generation;
    public uint Reserved;
    public long Payload;
}

/// <summary>Mirrors <c>mv_job_stats</c>.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct MvJobStats
{
    public ulong Submitted;
    public ulong Completed;
    public ulong Cancelled;
    public ulong QueueDepth;
    public uint WorkerCount;
    public uint Generation;
}

/// <summary>Mirrors <c>mv_image_info</c>.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct MvImageInfo
{
    public uint Width;
    public uint Height;
    public uint Format;
    public uint IccTagged;
    public uint TransferIntent;
    public uint Reserved;
}

/// <summary>
/// Thrown when a core call fails. Carries the correlation id, which is what ties
/// this exception to the native minidump that produced it
/// (plan/13-updates-and-telemetry.md).
/// </summary>
public sealed class MediaViewerException : Exception
{
    public MediaViewerException(MvStatus status, string message, ulong correlationId)
        : base($"{status}: {message} [correlation {correlationId}]")
    {
        Status = status;
        CorrelationId = correlationId;
    }

    public MvStatus Status { get; }
    public ulong CorrelationId { get; }
}
