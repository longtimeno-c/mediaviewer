// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace MediaViewer.Interop;

/// <summary>
/// The managed face of the core. Owns the SafeHandle, enforces the string
/// copying rule, and drains the completion queue.
/// </summary>
/// <remarks>
/// This type never blocks. Every call it makes is annotated <c>[no-block]</c> in
/// the header, which is the rule that follows from docs/design/02: anything the UI
/// thread calls must return without I/O, decode, or lock contention. Opening an
/// image is a request that returns a job id, never a call that decodes first.
/// </remarks>
public sealed partial class MediaViewerSession : IDisposable
{
    public MvVideoInfo VideoInfo { get { ThrowIfFailed(NativeMethods.mv_video_get_info(_handle, out var value)); return value; } }
    public long VideoPosition { get { ThrowIfFailed(NativeMethods.mv_video_position(_handle, out var value)); return value; } }
    public uint VideoState { get { ThrowIfFailed(NativeMethods.mv_video_state(_handle, out var value)); return value; } }
    public void VideoPlay() => ThrowIfFailed(NativeMethods.mv_video_play(_handle));
    public void VideoPause() => ThrowIfFailed(NativeMethods.mv_video_pause(_handle));
    public void VideoHold(bool hold, bool resume) => ThrowIfFailed(NativeMethods.mv_video_set_hold(_handle, hold ? 1 : 0, resume ? 1 : 0));
    public void VideoSeek(long ns, bool exact) => ThrowIfFailed(NativeMethods.mv_video_seek(_handle, ns, exact ? 1 : 0));
    public void VideoStep(int frames) => ThrowIfFailed(NativeMethods.mv_video_step(_handle, frames));
    public void VideoRate(double rate) => ThrowIfFailed(NativeMethods.mv_video_set_rate(_handle, rate));
    public void VideoVolume(float volume) => ThrowIfFailed(NativeMethods.mv_video_set_volume(_handle, volume));
    public void VideoMuted(bool muted) => ThrowIfFailed(NativeMethods.mv_video_set_muted(_handle, muted ? 1 : 0));
    public void VideoTrack(uint index) => ThrowIfFailed(NativeMethods.mv_video_select_audio_track(_handle, index));
    public void VideoLoop(long a, long b) => ThrowIfFailed(NativeMethods.mv_video_set_loop(_handle, a, b));

    private readonly MvSessionHandle _handle;
    internal MvSessionHandle Handle => _handle;
    private MvCompletion[] _drainBuffer = new MvCompletion[256];
    // Issue #226: the listing's buffers, kept and reused across relists.
    private MvFolderEntry[] _entries = new MvFolderEntry[256];
    private byte[] _entryStrings = new byte[64 * 1024];

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
    /// <summary>
    /// Borrows a session the native host already owns. Retains so Dispose is a
    /// matching release; the host keeps its own reference.
    /// </summary>
    public static MediaViewerSession Borrow(IntPtr native)
    {
        if (native == IntPtr.Zero) throw new ArgumentNullException(nameof(native));
        ThrowIfFailed(NativeMethods.mv_session_retain(native));
        return new MediaViewerSession(new MvSessionHandle(native));
    }

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
    /// Submits an image open and returns its job id immediately. The answer
    /// arrives as <see cref="MvCompletionKind.ImageOpened"/>. Pixels never
    /// cross the ABI.
    /// </summary>
    public ulong OpenImage(string utf8Path)
    {
        ArgumentNullException.ThrowIfNull(utf8Path);
        ThrowIfFailed(NativeMethods.mv_session_bump_generation(_handle, out _));
        ThrowIfFailed(NativeMethods.mv_image_open(_handle, utf8Path, out ulong jobId));
        return jobId;
    }

    public uint FolderCount
    {
        get
        {
            ThrowIfFailed(NativeMethods.mv_folder_count(_handle, out uint count));
            return count;
        }
    }

    public MvFolderItem FolderItemAt(uint index)
    {
        ThrowIfFailed(NativeMethods.mv_folder_item_at(_handle, index, out MvFolderItem item));
        return item;
    }

    public string FolderItemName(uint index) => ReadFolderString(NativeMethods.mv_folder_item_name, index);
    public string FolderItemPath(uint index) => ReadFolderString(NativeMethods.mv_folder_item_path, index);
    public string FolderItemThumbPath(uint index) =>
        ReadFolderString(NativeMethods.mv_folder_item_thumb_path, index);
    /// <summary>The other half of a paired stop (RAW, or a Live Photo's MOV); empty if unpaired.</summary>
    public string FolderItemPairPath(uint index) =>
        ReadFolderString(NativeMethods.mv_folder_item_pair_path, index);

    public ulong FolderSelect(uint index)
    {
        ThrowIfFailed(NativeMethods.mv_folder_select(_handle, index, out ulong jobId));
        return jobId;
    }

    public void FolderThumbsVisible(uint first, uint count) =>
        ThrowIfFailed(NativeMethods.mv_folder_thumbs_visible(_handle, first, count));

    /// <summary>
    /// Issue #429 (ABI 0.31): thumbnails of pages <paramref name="first"/> onward of the
    /// selected stop, for the page sidebar; each lands as
    /// <see cref="MvCompletionKind.PageThumbReady"/> (payload = page, generation = the
    /// returned serial). Returns the serial naming the stop, or 0 on a core without it.
    /// </summary>
    public uint FolderPageThumbs(uint first, uint count)
    {
        try
        {
            return NativeMethods.mv_folder_page_thumbs(_handle, first, count, out uint serial) == MvStatus.Ok
                ? serial
                : 0;
        }
        catch (EntryPointNotFoundException)
        {
            return 0;
        }
    }

    /// <summary>The JPEG path of a landed page thumbnail; empty until it lands.</summary>
    public string FolderPageThumbPath(uint page) =>
        ReadFolderString(NativeMethods.mv_folder_page_thumb_path, page);

    public string FolderDirectory => ReadUtf8(NativeMethods.mv_folder_directory);

    /// <summary>
    /// #291 (ABI 0.28): the folder's stops whose tags hold every word of
    /// <paramref name="query"/>, as JSON {"done","total","reading","hits":[[index,"why"],...]}.
    /// Empty on a core older than 0.20. Never reads a file.
    /// </summary>
    public string FolderTagSearch(string query)
    {
        try
        {
            NativeMethods.mv_folder_tag_search(_handle, query, IntPtr.Zero, 0, out uint need);
            // The pass runs on a worker: the answer can grow between the size
            // call and the copy, and a cut copy is JSON that does not parse
            // (the search would then miss its hits and stop polling). Ask
            // again with the bigger size; slack covers most growth at once.
            for (int attempt = 0; attempt < 4 && need > 1; ++attempt)
            {
                uint cap = need + 4096;
                IntPtr buf = Marshal.AllocHGlobal((int)cap);
                try
                {
                    ThrowIfFailed(NativeMethods.mv_folder_tag_search(_handle, query, buf, cap, out need));
                    if (need <= cap) return Marshal.PtrToStringUTF8(buf) ?? string.Empty;
                }
                finally
                {
                    Marshal.FreeHGlobal(buf);
                }
            }
            return string.Empty;
        }
        catch (EntryPointNotFoundException)
        {
            return string.Empty;
        }
    }

    /// <summary>#291: moves when the tag pass learns more (0 on an older core).</summary>
    public ulong FolderTagGeneration
    {
        get
        {
            try
            {
                return NativeMethods.mv_folder_tag_generation(_handle, out ulong g) == MvStatus.Ok ? g : 0;
            }
            catch (EntryPointNotFoundException)
            {
                return 0;
            }
        }
    }

    public uint FolderSubfolderCount
    {
        get
        {
            ThrowIfFailed(NativeMethods.mv_folder_subfolder_count(_handle, out uint count));
            return count;
        }
    }

    public string FolderSubfolderName(uint index) =>
        ReadFolderString(NativeMethods.mv_folder_subfolder_name, index);
    public string FolderSubfolderPath(uint index) =>
        ReadFolderString(NativeMethods.mv_folder_subfolder_path, index);

    public MvFolderSummary FolderSummaryAt(uint index)
    {
        ThrowIfFailed(NativeMethods.mv_folder_summary_at(_handle, index, out MvFolderSummary summary));
        return summary;
    }

    public string FolderSummaryCoverThumb(uint index) =>
        ReadFolderString(NativeMethods.mv_folder_summary_cover_thumb_path, index);

    public void FolderRequestSummary(uint index) =>
        ThrowIfFailed(NativeMethods.mv_folder_request_summary(_handle, index));

    /// <summary>
    /// ABI 0.14: shows <paramref name="paths"/> as the listing, in that order
    /// (search results). A clip with a moment (>= 0) opens paused on it. The
    /// answer arrives as <see cref="MvCompletionKind.FolderReady"/>, as for a folder.
    /// </summary>
    public unsafe ulong OpenList(string title, IReadOnlyList<string> paths, IReadOnlyList<long>? momentsMs,
                                 uint selectIndex)
    {
        ArgumentNullException.ThrowIfNull(title);
        ArgumentNullException.ThrowIfNull(paths);
        if (momentsMs is not null && momentsMs.Count != paths.Count)
            throw new ArgumentException("one moment per path", nameof(momentsMs));
        // One block: every path NUL-terminated, pinned for the call only (the
        // core copies before it returns).
        var offsets = new int[paths.Count];
        var blob = new List<byte>(paths.Count * 96);
        for (int i = 0; i < paths.Count; ++i)
        {
            offsets[i] = blob.Count;
            blob.AddRange(System.Text.Encoding.UTF8.GetBytes(paths[i]));
            blob.Add(0);
        }
        byte[] bytes = blob.ToArray();
        byte[] titleBytes = System.Text.Encoding.UTF8.GetBytes(title + "\0");
        long[]? moments = momentsMs?.ToArray();
        var pointers = new IntPtr[paths.Count];
        ulong jobId;
        fixed (byte* b = bytes)
        fixed (byte* t = titleBytes)
        fixed (IntPtr* p = pointers)
        fixed (long* m = moments)
        {
            for (int i = 0; i < offsets.Length; ++i) pointers[i] = (IntPtr)(b + offsets[i]);
            ThrowIfFailed(NativeMethods.mv_folder_open_list(_handle, t, (byte**)p, moments is null ? null : m,
                                                            (uint)paths.Count, selectIndex, out jobId));
        }
        return jobId;
    }

    /// <summary>The open result list's title, or "" while a directory is open.</summary>
    public string ListTitle => ReadUtf8(NativeMethods.mv_folder_list_title);

    /// <summary>The moment an item opens on, -1 for none (always -1 in a folder).</summary>
    public long ItemMoment(uint index)
    {
        ThrowIfFailed(NativeMethods.mv_folder_item_moment(_handle, index, out long ms));
        return ms;
    }

    /// <summary>
    /// ABI 0.20 (issue #226): the whole listing in one call under one core lock,
    /// instead of about eight calls per stop. Fills buffers this session keeps,
    /// so a relist allocates nothing here once they have grown to the folder.
    /// Both spans are valid until the next call; read a stop's strings with
    /// <see cref="EntryString"/>.
    /// </summary>
    public unsafe int ReadFolderEntries(out ReadOnlySpan<MvFolderEntry> entries,
                                        out ReadOnlySpan<byte> strings)
    {
        // The listing can change between a refused call and the retry; a few
        // rounds always settle (a watcher relist is not a busy loop).
        for (int attempt = 0; ; ++attempt)
        {
            MvStatus status;
            uint count, bytes;
            try
            {
                fixed (MvFolderEntry* e = _entries)
                fixed (byte* b = _entryStrings)
                {
                    status = NativeMethods.mv_folder_entries(_handle, e, (uint)_entries.Length, b,
                                                             (uint)_entryStrings.Length, out count,
                                                             out bytes);
                }
            }
            catch (EntryPointNotFoundException)
            {
                // A core older than ABI 0.20: the same snapshot, a stop at a time.
                int n = ReadFolderEntriesPerItem();
                entries = _entries.AsSpan(0, n);
                strings = _entryStrings;
                return n;
            }
            if (status == MvStatus.Ok)
            {
                entries = _entries.AsSpan(0, (int)count);
                strings = _entryStrings.AsSpan(0, (int)bytes);
                return (int)count;
            }
            bool grow = count > _entries.Length || bytes > _entryStrings.Length;
            if (status != MvStatus.InvalidArg || !grow || attempt >= 8) ThrowIfFailed(status);
            // Headroom, so a folder that gains a file does not reallocate again.
            if (count > _entries.Length) _entries = new MvFolderEntry[count + count / 4 + 16];
            if (bytes > _entryStrings.Length) _entryStrings = new byte[bytes + bytes / 4 + 4096];
        }
    }

    /// <summary>The NUL-terminated string at <paramref name="offset"/> of a listing's string block.</summary>
    public static string EntryString(ReadOnlySpan<byte> strings, uint offset)
    {
        ReadOnlySpan<byte> s = strings[(int)offset..];
        int end = s.IndexOf((byte)0);
        return end <= 0 ? string.Empty : System.Text.Encoding.UTF8.GetString(s[..end]);
    }

    private int ReadFolderEntriesPerItem()
    {
        uint count = FolderCount;
        if (_entries.Length < count) _entries = new MvFolderEntry[count];
        int at = 0;
        uint Put(string value)
        {
            int need = System.Text.Encoding.UTF8.GetMaxByteCount(value.Length) + 1;
            if (at + need > _entryStrings.Length)
                Array.Resize(ref _entryStrings, Math.Max(_entryStrings.Length * 2, at + need));
            int offset = at;
            at += System.Text.Encoding.UTF8.GetBytes(value, 0, value.Length, _entryStrings, at);
            _entryStrings[at++] = 0;
            return (uint)offset;
        }
        bool list;
        try { list = ListTitle.Length > 0; }
        catch (EntryPointNotFoundException) { list = false; }
        for (uint i = 0; i < count; ++i)
        {
            MvFolderItem item = FolderItemAt(i);
            _entries[i] = new MvFolderEntry
            {
                Item = item,
                MomentMs = list ? ItemMoment(i) : -1,
                NameOffset = Put(FolderItemName(i)),
                PathOffset = Put(FolderItemPath(i)),
                ThumbOffset = Put(FolderItemThumbPath(i)),
                PairOffset = Put(item.PairKind == MvPairKind.None ? "" : FolderItemPairPath(i)),
            };
        }
        return (int)count;
    }

    /// <summary>
    /// The immediate subfolders of <paramref name="dir"/> for the folder tree (PR 9): hidden,
    /// system and dot directories skipped, sorted by name. One directory read, so call it off
    /// the UI thread. Returns an empty list when the directory cannot be read.
    /// </summary>
    public static IReadOnlyList<(string Name, string Path)> ListSubdirectories(string dir)
    {
        var result = new List<(string, string)>();
        MvStatus first = NativeMethods.mv_list_subdirectories(dir, IntPtr.Zero, 0, out uint need);
        // cap == 0 reports the size and INVALID_ARG; an I/O failure reports neither.
        if (need <= 1 || first == MvStatus.Io) return result;
        IntPtr buf = Marshal.AllocHGlobal((int)need);
        try
        {
            if (NativeMethods.mv_list_subdirectories(dir, buf, need, out _) != MvStatus.Ok) return result;
            string text = Marshal.PtrToStringUTF8(buf) ?? string.Empty;
            foreach (string line in text.Split('\n', StringSplitOptions.RemoveEmptyEntries))
            {
                int tab = line.IndexOf('\t');
                if (tab > 0) result.Add((line[..tab], line[(tab + 1)..]));
            }
        }
        finally
        {
            Marshal.FreeHGlobal(buf);
        }
        return result;
    }

    private delegate MvStatus FolderStringFn(MvSessionHandle session, uint index, IntPtr utf8,
                                             uint cap, out uint outBytes);
    private delegate MvStatus SessionStringFn(MvSessionHandle session, IntPtr utf8, uint cap,
                                              out uint outBytes);

    private string ReadFolderString(FolderStringFn fn, uint index)
    {
        uint need = 0;
        fn(_handle, index, IntPtr.Zero, 0, out need);
        if (need <= 1) return string.Empty;
        IntPtr buf = Marshal.AllocHGlobal((int)need);
        try
        {
            ThrowIfFailed(fn(_handle, index, buf, need, out _));
            return Marshal.PtrToStringUTF8(buf) ?? string.Empty;
        }
        finally
        {
            Marshal.FreeHGlobal(buf);
        }
    }

    private string ReadUtf8(SessionStringFn fn)
    {
        uint need = 0;
        fn(_handle, IntPtr.Zero, 0, out need);
        if (need <= 1) return string.Empty;
        IntPtr buf = Marshal.AllocHGlobal((int)need);
        try
        {
            ThrowIfFailed(fn(_handle, buf, need, out _));
            return Marshal.PtrToStringUTF8(buf) ?? string.Empty;
        }
        finally
        {
            Marshal.FreeHGlobal(buf);
        }
    }

    public MvImageInfo ImageInfo
    {
        get
        {
            ThrowIfFailed(NativeMethods.mv_session_image_info(_handle, out MvImageInfo info));
            return info;
        }
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
    public unsafe ReadOnlySpan<MvCompletion> Drain()
    {
        // Issue #226: straight into the kept buffer, so an empty queue costs one
        // call and no allocation.
        int total = 0;
        while (true)
        {
            if (total == _drainBuffer.Length) Array.Resize(ref _drainBuffer, _drainBuffer.Length * 2);
            uint capacity = (uint)(_drainBuffer.Length - total);
            uint count;
            fixed (MvCompletion* p = &_drainBuffer[total])
            {
                count = NativeMethods.mv_completion_drain(_handle, p, capacity);
            }
            total += (int)count;
            if (count < capacity) break;
        }

        return _drainBuffer.AsSpan(0, total);
    }

    public void Dispose() => _handle.Dispose();

    private static void ThrowIfFailed(MvStatus status)
    {
        if (status == MvStatus.Ok) return;

        // Copy immediately. The core's buffer is valid only until the next call
        // on this thread (docs/design/14 ownership table), so storing the pointer or
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
