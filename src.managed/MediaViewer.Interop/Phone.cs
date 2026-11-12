// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using System.Text;

namespace MediaViewer.Interop;

/// <summary>
/// Settings → Phone access, part of the app (ABI 0.40, docs/plans/phone-in-the-app.md):
/// one JSON request for the feature's own thread, and the card's state. Every
/// call is [no-block]; nothing here logs a path, an address or a key (rule 6).
/// </summary>
public static partial class PhoneNative
{
    private const uint Secondary = 1;  // MV_PHONE_SECONDARY

    [LibraryImport(NativeMethods.Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static partial MvStatus mv_phone_request(string requestJson);

    [LibraryImport(NativeMethods.Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static unsafe partial MvStatus mv_phone_state_json(byte* utf8, uint cap, out uint outBytes);

    [LibraryImport(NativeMethods.Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static partial MvStatus mv_phone_resume(uint flags);

    /// <summary>At launch: this window's process runs Phone access (the first window), or not.</summary>
    public static void Resume(bool primary)
    {
        try { _ = mv_phone_resume(primary ? 0u : Secondary); }
        catch (Exception ex) when (ex is EntryPointNotFoundException or DllNotFoundException) { }
    }

    /// <summary>{"do": …}; false when it does not parse, or in a later window's process.</summary>
    public static bool Request(string requestJson)
    {
        try { return mv_phone_request(requestJson) == MvStatus.Ok; }
        catch (Exception ex) when (ex is EntryPointNotFoundException or DllNotFoundException) { return false; }
    }

    /// <summary>The card's state, one JSON object; "{}" when the core has none.</summary>
    public static unsafe string StateJson()
    {
        try
        {
            uint cap = 16 * 1024;
            for (int attempt = 0; attempt < 4; ++attempt)
            {
                byte[] buf = new byte[cap];
                MvStatus s;
                uint need;
                fixed (byte* p = buf) s = mv_phone_state_json(p, cap, out need);
                if (s == MvStatus.Ok) return Encoding.UTF8.GetString(buf, 0, (int)Math.Max(0, need - 1));
                if (s != MvStatus.InvalidArg || need <= cap) return "{}";
                cap = need + 1024;
            }
        }
        catch (Exception ex) when (ex is EntryPointNotFoundException or DllNotFoundException) { }
        return "{}";
    }
}
