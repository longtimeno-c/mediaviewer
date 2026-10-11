// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace MediaViewer.Interop;

/// <summary>Mirrors <c>mv_slot_theme</c>: the chrome's tokens as 0xRRGGBBAA.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct MvSlotTheme
{
    public uint Canvas;
    public uint Surface;
    public uint Title;
    public uint Body;
    public uint Disabled;
    public uint Hairline;
    public uint Accent;
    public uint Dark;
}

/// <summary>What changed in the registry (the listener's bits).</summary>
[Flags]
public enum SlotChange : uint
{
    None = 0,
    Contributions = 1,
    Info = 2,
    Screens = 4,
    Search = 8,
    Badges = 16,
    Status = 32,
}

/// <summary>
/// Screens and slots (docs/design/25 PRs 58-59; ABI 0.20, mediaviewer_addon.h
/// <c>mv_slots_*</c>): what MediaViewer's own add-ons add to the chrome, read
/// from the core's registry. Every call copies a table or posts to an add-on's
/// worker; none waits on an add-on, so all are safe on the UI thread.
/// </summary>
public static unsafe partial class SlotNative
{
    private const string Library = NativeMethods.Library;

    public const string Interface = "mv.slots.1";
    public const uint ScopeFiles = 1;
    public const uint ScopeLocal = 2;

    public const uint EventClick = 1;
    public const uint EventChange = 2;
    public const uint EventSubmit = 3;
    public const uint EventSelect = 4;
    public const uint EventActivate = 5;

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void ChangedCallback(IntPtr user, uint bits);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_slots_set_listener(IntPtr fn, IntPtr user);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_slots_contributions_json(byte* output, uint cap, uint* needed);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_slots_info_json(byte* output, uint cap, uint* needed);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_slots_screens_json(byte* output, uint cap, uint* needed);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_slots_search_json(uint scope, byte* output, uint cap, uint* needed);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_slots_badges_json(byte* output, uint cap, uint* needed);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_slots_status_json(byte* output, uint cap, uint* needed);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_slots_request_info(byte* path, byte* locationText, MvSlotTheme* theme,
        float widthPt, float scale, ulong* generation);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_slots_clear_info();

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_slots_search(uint scope, byte* query, byte* folder, ulong* generation);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_slots_request_badges(byte* folder, ulong* generation);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_slots_open_screen(byte* addon, byte* screen, byte* path, MvSlotTheme* theme,
        float widthPt, float heightPt, float scale);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_slots_close_screen(byte* addon, byte* screen);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_slots_event(byte* addon, byte* screen, byte* node, uint kind, byte* value);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_slots_invoke(byte* addon, byte* item, byte* paths);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_slots_surface(byte* addon, byte* screen, byte* surface, byte* output,
        ulong cap, uint* width, uint* height, ulong* revision, ulong* needed);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_slots_local_search_provider(uint on);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial MvStatus mv_slots_thumbnail_path(byte* path, byte* output, uint cap, uint* needed);

    /// <summary>Worker thread: the JPEG-512 of a still, by path; null for none (a clip, unreadable).</summary>
    public static string? ThumbnailPath(string path)
    {
        string got = Json((o, c, n) =>
        {
            fixed (byte* p = Z(path)) return mv_slots_thumbnail_path(p, o, c, n);
        });
        return got.Length == 0 ? null : got;
    }

    // Kept alive for the process: native holds the pointer.
    private static ChangedCallback? _callback;

    private static byte[] Z(string s)
    {
        byte[] b = new byte[Encoding.UTF8.GetByteCount(s) + 1];
        Encoding.UTF8.GetBytes(s, 0, s.Length, b, 0);
        return b;
    }

    private delegate MvStatus JsonCall(byte* output, uint cap, uint* needed);

    // "" on an older core (no 0.20 entry points) or on any failure: the slots
    // then simply stay empty.
    private static string Json(JsonCall call)
    {
        try
        {
            uint cap = 16 * 1024;
            for (int attempt = 0; attempt < 4; ++attempt)
            {
                byte[] buf = new byte[cap];
                uint needed = 0;
                MvStatus s;
                fixed (byte* p = buf) s = call(p, cap, &needed);
                if (s == MvStatus.Ok) return Encoding.UTF8.GetString(buf, 0, (int)Math.Max(0, needed - 1));
                if (s != MvStatus.InvalidArg || needed <= cap) return "";
                cap = needed + 256;
            }
        }
        catch (EntryPointNotFoundException)
        {
        }
        return "";
    }

    /// <summary>Registers the one listener (any thread; the chrome marshals). False on an older core.</summary>
    public static bool SetListener(ChangedCallback callback)
    {
        try
        {
            _callback = callback;
            return mv_slots_set_listener(Marshal.GetFunctionPointerForDelegate(callback), IntPtr.Zero) == MvStatus.Ok;
        }
        catch (EntryPointNotFoundException)
        {
            return false;
        }
    }

    public static string ContributionsJson() => Json(mv_slots_contributions_json);
    public static string InfoJson() => Json(mv_slots_info_json);
    public static string ScreensJson() => Json(mv_slots_screens_json);
    public static string BadgesJson() => Json(mv_slots_badges_json);
    public static string StatusJson() => Json(mv_slots_status_json);
    public static string SearchJson(uint scope) => Json((o, c, n) => mv_slots_search_json(scope, o, c, n));

    private static T Try<T>(Func<T> call, T fallback)
    {
        try { return call(); }
        catch (EntryPointNotFoundException) { return fallback; }
    }

    public static ulong RequestInfo(string path, string locationText, MvSlotTheme theme, float widthPt, float scale) =>
        Try(() =>
        {
            ulong g = 0;
            fixed (byte* p = Z(path))
            fixed (byte* l = Z(locationText))
            {
                _ = mv_slots_request_info(p, l, &theme, widthPt, scale, &g);
            }
            return g;
        }, 0UL);

    public static void ClearInfo() => Try(() => mv_slots_clear_info(), MvStatus.Ok);

    public static ulong Search(uint scope, string query, string folder) =>
        Try(() =>
        {
            ulong g = 0;
            fixed (byte* q = Z(query))
            fixed (byte* f = Z(folder))
            {
                _ = mv_slots_search(scope, q, f, &g);
            }
            return g;
        }, 0UL);

    public static ulong RequestBadges(string folder) =>
        Try(() =>
        {
            ulong g = 0;
            fixed (byte* f = Z(folder)) _ = mv_slots_request_badges(f, &g);
            return g;
        }, 0UL);

    public static bool OpenScreen(string addon, string screen, string path, MvSlotTheme theme, float w, float h, float scale) =>
        Try(() =>
        {
            fixed (byte* a = Z(addon))
            fixed (byte* s = Z(screen))
            fixed (byte* p = Z(path))
            {
                return mv_slots_open_screen(a, s, p, &theme, w, h, scale) == MvStatus.Ok;
            }
        }, false);

    public static bool CloseScreen(string addon, string screen) =>
        Try(() =>
        {
            fixed (byte* a = Z(addon))
            fixed (byte* s = Z(screen))
            {
                return mv_slots_close_screen(a, s) == MvStatus.Ok;
            }
        }, false);

    /// <summary>A control changed; the core checks it against the tree first.</summary>
    public static bool Event(string addon, string screen, string node, uint kind, string valueJson) =>
        Try(() =>
        {
            fixed (byte* a = Z(addon))
            fixed (byte* s = Z(screen))
            fixed (byte* n = Z(node))
            fixed (byte* v = Z(valueJson))
            {
                return mv_slots_event(a, s, n, kind, v) == MvStatus.Ok;
            }
        }, false);

    public static bool Invoke(string addon, string item, string pathsJson) =>
        Try(() =>
        {
            fixed (byte* a = Z(addon))
            fixed (byte* i = Z(item))
            fixed (byte* p = Z(pathsJson))
            {
                return mv_slots_invoke(a, i, p) == MvStatus.Ok;
            }
        }, false);

    /// <summary>A surface's premultiplied BGRA, or null.</summary>
    public static (byte[] Pixels, int Width, int Height, ulong Revision)? Surface(string addon, string screen, string surface) =>
        Try<(byte[], int, int, ulong)?>(() =>
        {
            byte[] a = Z(addon), s = Z(screen), f = Z(surface);
            uint w = 0, h = 0;
            ulong rev = 0, need = 0;
            fixed (byte* pa = a)
            fixed (byte* ps = s)
            fixed (byte* pf = f)
            {
                MvStatus st = mv_slots_surface(pa, ps, pf, null, 0, &w, &h, &rev, &need);
                if (st != MvStatus.InvalidArg || need == 0 || need > int.MaxValue) return null;
                byte[] px = new byte[need];
                fixed (byte* pp = px)
                {
                    if (mv_slots_surface(pa, ps, pf, pp, need, &w, &h, &rev, &need) != MvStatus.Ok) return null;
                }
                return (px, (int)w, (int)h, rev);
            }
        }, null);

    /// <summary>docs/design/25 §7: the AI pack attached (true) or went: Local search is a provider.</summary>
    public static void LocalSearchProvider(bool on) => Try(() => mv_slots_local_search_provider(on ? 1u : 0u), MvStatus.Ok);
}
