// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using System.Text;

namespace MediaViewer.Interop;

/// <summary>
/// Another computer's folders (ABI 0.44, docs/plans/desktop-client.md): the paired
/// servers the folder tree pins, a pasted code's preview, pairing on a worker, and
/// Forget. Every call is [no-block]; a server's folders are listed and opened through
/// mv_list_subdirectories and the tree's open, as a disk folder is. Nothing here
/// logs a name, an address or a key (rule 6).
/// </summary>
public static partial class RemoteNative
{
    private unsafe delegate MvStatus JsonFn(byte* utf8, uint cap, out uint outBytes);

    [LibraryImport(NativeMethods.Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static unsafe partial MvStatus mv_remote_servers_json(byte* utf8, uint cap, out uint outBytes);

    [LibraryImport(NativeMethods.Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static partial MvStatus mv_remote_generation(out ulong generation);

    [LibraryImport(NativeMethods.Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static unsafe partial MvStatus mv_remote_preview_json(string code, byte* utf8, uint cap, out uint outBytes);

    [LibraryImport(NativeMethods.Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static partial MvStatus mv_remote_pair_start(string code);

    [LibraryImport(NativeMethods.Library)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static unsafe partial MvStatus mv_remote_pair_state_json(byte* utf8, uint cap, out uint outBytes);

    [LibraryImport(NativeMethods.Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static partial MvStatus mv_remote_forget(string serverKey);

    private static unsafe string Read(JsonFn fn, string fallback)
    {
        try
        {
            uint cap = 8 * 1024;
            for (int attempt = 0; attempt < 4; ++attempt)
            {
                byte[] buf = new byte[cap];
                MvStatus s;
                uint need;
                fixed (byte* p = buf) s = fn(p, cap, out need);
                if (s == MvStatus.Ok) return Encoding.UTF8.GetString(buf, 0, (int)Math.Max(0, need - 1));
                if (s != MvStatus.InvalidArg || need <= cap) return fallback;
                cap = need + 1024;
            }
        }
        catch (Exception ex) when (ex is EntryPointNotFoundException or DllNotFoundException) { }
        return fallback;
    }

    /// <summary>[{"name","key","state"}]; "[]" when none or no core.</summary>
    public static unsafe string ServersJson() => Read(mv_remote_servers_json, "[]");

    public static ulong Generation()
    {
        try { return mv_remote_generation(out ulong g) == MvStatus.Ok ? g : 0; }
        catch (Exception ex) when (ex is EntryPointNotFoundException or DllNotFoundException) { return 0; }
    }

    /// <summary>{"name","fingerprint","paired"} or {"error":"words"}.</summary>
    public static unsafe string PreviewJson(string code) =>
        Read((byte* p, uint cap, out uint need) => mv_remote_preview_json(code, p, cap, out need), "{}");

    /// <summary>False while a pairing is already running.</summary>
    public static bool PairStart(string code)
    {
        try { return mv_remote_pair_start(code) == MvStatus.Ok; }
        catch (Exception ex) when (ex is EntryPointNotFoundException or DllNotFoundException) { return false; }
    }

    /// <summary>{"busy","ok","words"}.</summary>
    public static unsafe string PairStateJson() => Read(mv_remote_pair_state_json, "{}");

    public static void Forget(string serverKey)
    {
        try { _ = mv_remote_forget(serverKey); }
        catch (Exception ex) when (ex is EntryPointNotFoundException or DllNotFoundException) { }
    }
}
