// SPDX-License-Identifier: GPL-2.0-or-later
using System.Runtime.InteropServices;

namespace MediaViewer.Interop;

/// <summary>
/// The raw P/Invoke surface. Nothing outside this file calls these directly —
/// everything else goes through <see cref="MediaViewerSession"/>, which owns the
/// SafeHandle and the copying rules.
/// </summary>
/// <remarks>
/// plan/14-abi.md. Every entry point here is <c>__cdecl</c> and returns
/// <see cref="MvStatus"/>; a bool or a -1 anywhere in this file is a bug.
/// </remarks>
internal static partial class NativeMethods
{
    internal const string Library = "mediaviewer_core";

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial uint mv_abi_version();

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial IntPtr mv_status_name(MvStatus status);

    // Returns a pointer the core owns, valid only until the next call on this
    // thread. Callers copy immediately with Marshal.PtrToStringUTF8 and never
    // store the pointer — hence IntPtr here rather than a marshalled string,
    // which would hide the lifetime rule the header is explicit about.
    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial IntPtr mv_last_error_message();

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial ulong mv_last_error_correlation_id();

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_session_create(in MvSessionConfig config,
                                                       out IntPtr session);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_session_retain(IntPtr session);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_session_release(IntPtr session);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_session_bump_generation(MvSessionHandle session,
                                                                out uint generation);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_session_current_generation(MvSessionHandle session,
                                                                   out uint generation);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial IntPtr mv_completion_wait_handle(MvSessionHandle session);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial uint mv_completion_drain(MvSessionHandle session,
                                                     [Out] MvCompletion[] completions,
                                                     uint capacity);

    // The string is UTF-8, owned by the caller, and copied by the core before it
    // returns. StringMarshalling.Utf8 pins a temporary for exactly the duration
    // of the call, which is the contract.
    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_session_echo(MvSessionHandle session, string utf8Text,
                                                     out ulong jobId);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_session_job_stats(MvSessionHandle session,
                                                          out MvJobStats stats);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_image_open(MvSessionHandle session, string utf8Path,
                                                   out ulong jobId);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_session_image_info(MvSessionHandle session,
                                                           out MvImageInfo info);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_folder_open(MvSessionHandle session, string utf8Dir,
                                                    string? utf8SelectPath, out ulong jobId);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_folder_count(MvSessionHandle session, out uint count);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_folder_item_at(MvSessionHandle session, uint index,
                                                       out MvFolderItem item);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_folder_item_name(MvSessionHandle session, uint index,
                                                         IntPtr utf8, uint cap, out uint outBytes);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_folder_item_path(MvSessionHandle session, uint index,
                                                         IntPtr utf8, uint cap, out uint outBytes);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_folder_item_thumb_path(MvSessionHandle session, uint index,
                                                               IntPtr utf8, uint cap,
                                                               out uint outBytes);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_folder_select(MvSessionHandle session, uint index,
                                                      out ulong jobId);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_folder_thumbs_visible(MvSessionHandle session, uint first,
                                                              uint count);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_folder_close(MvSessionHandle session);
    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_video_play(MvSessionHandle session);
    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_video_pause(MvSessionHandle session);
    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_video_seek(MvSessionHandle session, long position, int exact);
    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_video_step(MvSessionHandle session, int frames);
    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_video_set_rate(MvSessionHandle session, double rate);
    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_video_set_volume(MvSessionHandle session, float volume);
    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_video_set_muted(MvSessionHandle session, int muted);
    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_video_select_audio_track(MvSessionHandle session, uint index);
    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_video_set_loop(MvSessionHandle session, long a, long b);
    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_video_position(MvSessionHandle session, out long position);
    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_video_state(MvSessionHandle session, out uint state);
    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial MvStatus mv_video_get_info(MvSessionHandle session, out MvVideoInfo info);
}
