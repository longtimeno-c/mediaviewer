// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace MediaViewer.Interop;

/// <summary>Mirrors <c>mv_location_input</c> (mediaviewer.h, ABI 0.20). 152 bytes.</summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct MvLocationInput
{
    public uint StructSize;
    public uint Kind;  // MetaNative.Location*
    public double Lat, Lon;
    public fixed byte Text[128];
}

/// <summary>
/// #352 (owner, 2026-10-12): the Metadata pane's Location editor reads what was
/// typed through the core, so both hosts accept exactly the same forms
/// (meta::parse_location). Pure: no session, no file; safe on the UI thread.
/// </summary>
public static unsafe partial class MetaNative
{
    private const string Library = NativeMethods.Library;

    public const uint LocationPoint = 0;
    public const uint LocationBlank = 1;
    public const uint LocationInvalid = 2;

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_meta_parse_location(byte* utf8, MvLocationInput* output);

    /// <summary>
    /// What <paramref name="text"/> says. Kind is LocationPoint (Lat / Lon set,
    /// Message the formatted point), LocationBlank (remove the location) or
    /// LocationInvalid (Message says why). A core that cannot be called reads
    /// as invalid, never as a point.
    /// </summary>
    public static (uint Kind, double Lat, double Lon, string Message) ParseLocation(string text)
    {
        byte[] z = Encoding.UTF8.GetBytes(text + "\0");
        var input = new MvLocationInput { StructSize = (uint)sizeof(MvLocationInput) };
        MvStatus status;
        try
        {
            fixed (byte* p = z)
            {
                status = mv_meta_parse_location(p, &input);
            }
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException)
        {
            return (LocationInvalid, 0, 0, "Could not read the position");  // an older core
        }
        if (status != MvStatus.Ok) return (LocationInvalid, 0, 0, "Could not read the position");
        string message = Marshal.PtrToStringUTF8((IntPtr)input.Text) ?? "";
        return (input.Kind, input.Lat, input.Lon, message);
    }
}
