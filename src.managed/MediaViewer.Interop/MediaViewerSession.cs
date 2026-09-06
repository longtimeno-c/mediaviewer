// SPDX-License-Identifier: GPL-2.0-or-later
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace MediaViewer.Interop;

/// <summary>
/// The managed face of the core. Owns the SafeHandle, enforces the string
/// copying rule, and drains the completion queue.
/// </summary>
/// <remarks>
/// This type never blocks. Every call it makes is annotated <c>[no-block]</c> in
/// the header, which is the rule that follows from plan/02: anything the UI
/// thread calls must return without I/O, decode, or lock contention. Opening an
/// image is a request that returns a job id, never a call that decodes first.
/// </remarks>
public sealed class MediaViewerSession : IDisposable
{
    private readonly MvSessionHandle _handle;
    private MvCompletion[] _drainBuffer = new MvCompletion[256];

    private MediaViewerSession(MvSessionHandle handle)
    {
        _handle = handle;
        CompletionSignal = new AutoResetEventStandIn(
            NativeMethods.mv_completion_wait_handle(handle));
    }

    /// <summary>The ABI version the loaded core DLL reports.</summary>
    public static Version AbiVersion
    {
        get
        {
            uint packed = NativeMethods.mv_abi_version();
            return new Version((int)(packed >> 16), (int)(packed & 0xffff));
        }
    }

    /// <summary>
    /// A wait handle signalled while at least one completion is pending. The
    /// core owns the underlying OS handle, so this wrapper does NOT own it —
    /// closing it here would tear down the session's event out from under the
    /// core.
    /// </summary>
    public WaitHandle CompletionSignal { get; }

    /// <summary>
    /// Creates a session and asserts the ABI version.
    /// </summary>
    /// <remarks>
    /// The version check is here rather than left to the caller because a
    /// mismatched core DLL must fail loudly. The alternative is a struct layout
    /// change nobody notices until a field reads garbage.
    /// </remarks>
    public static MediaViewerSession Create(uint workerCount = 0, bool enableEtw = true)
    {
        Version abi = AbiVersion;
        if (abi.Major != ExpectedAbiMajor)
        {
            throw new MediaViewerException(
                MvStatus.Internal,
                $"core ABI {abi} is not compatible with this shell (expected major {ExpectedAbiMajor})",
                correlationId: 0);
        }

        var config = new MvSessionConfig
        {
            WorkerCount = workerCount,
            EnableEtw = enableEtw ? 1u : 0u,
        };

        MvStatus status = NativeMethods.mv_session_create(in config, out IntPtr raw);
        ThrowIfFailed(status);
        return new MediaViewerSession(new MvSessionHandle(raw));
    }

    /// <summary>The ABI major version this assembly was written against.</summary>
    public const int ExpectedAbiMajor = 0;

    /// <summary>
    /// Submits an echo and returns its job id immediately. The answer arrives as
    /// a completion.
    /// </summary>
    /// <remarks>
    /// This is PR 1's round-trip proof, and it is deliberately shaped like the
    /// real calls that follow it. If it were synchronous it would teach the
    /// wrong pattern on day one.
    /// </remarks>
    public ulong Echo(string text)
    {
        ArgumentNullException.ThrowIfNull(text);
        ThrowIfFailed(NativeMethods.mv_session_echo(_handle, text, out ulong jobId));
        return jobId;
    }

    /// <summary>
    /// Bumps the view generation. Everything queued or running at the old
    /// generation is abandoned at its next check.
    /// </summary>
    public uint BumpGeneration()
    {
        ThrowIfFailed(NativeMethods.mv_session_bump_generation(_handle, out uint generation));
        return generation;
    }

    public uint CurrentGeneration
    {
        get
        {
            ThrowIfFailed(NativeMethods.mv_session_current_generation(_handle, out uint gen));
            return gen;
        }
    }

    public MvJobStats JobStats
    {
        get
        {
            ThrowIfFailed(NativeMethods.mv_session_job_stats(_handle, out MvJobStats stats));
            return stats;
        }
    }

    /// <summary>
    /// Drains every pending completion. Returns an empty span when there is
    /// nothing to do.
    /// </summary>
    /// <remarks>
    /// The core never calls a managed callback and never touches the dispatcher;
    /// this is the other half of that arrangement. Draining also means
    /// completions batch naturally — a folder scan finishing 400 thumbnails is
    /// one call here, not 400 marshalling hops.
    /// <para>
    /// The returned span is valid until the next drain on this instance.
    /// </para>
    /// </remarks>
    public ReadOnlySpan<MvCompletion> Drain()
    {
        uint total = 0;
        while (true)
        {
            uint capacity = (uint)_drainBuffer.Length - total;
            if (capacity == 0)
            {
                Array.Resize(ref _drainBuffer, _drainBuffer.Length * 2);
                continue;
            }

            var chunk = new MvCompletion[capacity];
            uint count = NativeMethods.mv_completion_drain(_handle, chunk, capacity);
            if (count == 0) break;

            Array.Copy(chunk, 0, _drainBuffer, total, count);
            total += count;
            if (count < capacity) break;
        }

        return _drainBuffer.AsSpan(0, (int)total);
    }

    public void Dispose() => _handle.Dispose();

    private static void ThrowIfFailed(MvStatus status)
    {
        if (status == MvStatus.Ok) return;

        // Copy immediately. The core's buffer is valid only until the next call
        // on this thread (plan/14 ownership table), so storing the pointer or
        // deferring the read is a use-after-free waiting for a busy moment.
        string message = Marshal.PtrToStringUTF8(NativeMethods.mv_last_error_message())
                         ?? string.Empty;
        ulong correlationId = NativeMethods.mv_last_error_correlation_id();
        throw new MediaViewerException(status, message, correlationId);
    }

    /// <summary>
    /// A <see cref="WaitHandle"/> over a native event the core owns.
    /// </summary>
    /// <remarks>
    /// <c>ownsHandle: false</c> is the whole point: the session's event belongs
    /// to the core and is closed by <c>mv_session_release</c>. Letting the
    /// runtime close it would close a handle the core still uses, and the
    /// symptom would be a completion signal that stops arriving on a machine
    /// under GC pressure.
    /// </remarks>
    private sealed class AutoResetEventStandIn : WaitHandle
    {
        public AutoResetEventStandIn(IntPtr nativeHandle)
        {
            SafeWaitHandle = new SafeWaitHandle(nativeHandle, ownsHandle: false);
        }
    }
}
