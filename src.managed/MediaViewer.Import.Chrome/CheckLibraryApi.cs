// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using System.Text;
using MediaViewer.Interop;

namespace MediaViewer.Import.Chrome;

/// <summary>
/// The three calls Check library (docs/plans/check-library.md) appended to
/// <c>mv_import_api</c> after PR 54's two (mediaviewer_import.h). Read here, by
/// the add-on's own chrome, for the reason <see cref="DuplicatesApi"/> gives:
/// the Interop assembly ships with the app.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MvImportCheckCalls
{
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, ulong*, MvStatus> CheckLibrary;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, byte*, uint, uint*, MvStatus> LibraryFind;
    public delegate* unmanaged[Cdecl]<IntPtr, byte*, MvStatus> LibraryRecord;
}

internal sealed unsafe class CheckLibraryApi
{
    /// <summary>MV_ADDON_EVENT_LIBRARY_CHECKED, which the Interop enum does not name.</summary>
    internal const uint EventChecked = 10;

    private readonly MvImportApi* _table;
    private readonly MvImportCheckCalls* _calls;

    internal CheckLibraryApi(IntPtr table)
    {
        _table = (MvImportApi*)table;
        _calls = (MvImportCheckCalls*)((byte*)table + sizeof(MvImportApi) + sizeof(MvImportDuplicatesCalls));
    }

    /// <summary>The Import native library has Check library.</summary>
    internal bool Available =>
        _table->StructSize >= sizeof(MvImportApi) + sizeof(MvImportDuplicatesCalls) + sizeof(MvImportCheckCalls) &&
        _calls->CheckLibrary != null;

    private static byte[] Z(string s) => Encoding.UTF8.GetBytes(s + "\0");

    /// <summary>No-block: the walk and the hashing run on the add-on's thread.</summary>
    internal ulong Check(string requestJson)
    {
        ulong id;
        MvStatus s;
        fixed (byte* p = Z(requestJson)) s = _calls->CheckLibrary(_table->Ctx, p, &id);
        if (s != MvStatus.Ok) throw new MediaViewerException(s, "import.CheckLibrary", 0);
        return id;
    }
}
