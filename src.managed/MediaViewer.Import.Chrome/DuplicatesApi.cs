// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using System.Text;
using MediaViewer.Interop;

namespace MediaViewer.Import.Chrome;

/// <summary>
/// The two calls PR 54 appended to <c>mv_import_api</c> after
/// <c>verify_folder</c> (mediaviewer_import.h). Read here, by the add-on's own
/// chrome, rather than added to the host's <see cref="MvImportApi"/>: the
/// Interop assembly ships with the app, so an app that predates PR 54 still
/// runs this chrome, and an app that has it still runs an older Import.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MvImportDuplicatesCalls
{
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, ulong*, MvStatus> FindDuplicates;
    public delegate* unmanaged[Cdecl]<IntPtr, ulong, byte*, MvStatus> TrashDuplicate;
}

internal sealed unsafe class DuplicatesApi
{
    /// <summary>mv_addon_event_kind values the Interop enum does not name.</summary>
    internal const uint EventDone = 8;
    internal const uint EventTrashed = 9;

    private readonly MvImportApi* _table;
    private readonly MvImportDuplicatesCalls* _calls;

    internal DuplicatesApi(IntPtr table)
    {
        _table = (MvImportApi*)table;
        _calls = (MvImportDuplicatesCalls*)((byte*)table + sizeof(MvImportApi));
    }

    /// <summary>The Import native library has the calls (it is at least PR 54).</summary>
    internal bool Available =>
        _table->StructSize >= sizeof(MvImportApi) + sizeof(MvImportDuplicatesCalls) &&
        _calls->FindDuplicates != null && _calls->TrashDuplicate != null;

    private static byte[] Z(string s) => Encoding.UTF8.GetBytes(s + "\0");

    /// <summary>No-block: the walk and the hashing run on the add-on's thread.</summary>
    internal ulong Find(string dir)
    {
        ulong id;
        MvStatus s;
        fixed (byte* p = Z(dir)) s = _calls->FindDuplicates(_table->Ctx, p, &id);
        if (s != MvStatus.Ok) throw new MediaViewerException(s, "import.FindDuplicates", 0);
        return id;
    }

    /// <summary>
    /// No-block: queues the file for the Recycle Bin. The checks (unchanged,
    /// another copy still there) and the move run on the add-on's thread.
    /// </summary>
    internal MvStatus Trash(ulong job, string path)
    {
        fixed (byte* p = Z(path)) return _calls->TrashDuplicate(_table->Ctx, job, p);
    }
}
