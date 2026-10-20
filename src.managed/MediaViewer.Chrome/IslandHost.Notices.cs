// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.Windows.AppNotifications;
using Microsoft.Windows.AppNotifications.Builder;

namespace MediaViewer.Chrome;

/// <summary>
/// An add-on's notices and its keep-alive, on Windows (#452; docs/design/28 "In the
/// background"). The Mac twin is AddonNotices.swift.
/// </summary>
/// <remarks>
/// The core's registry holds what add-ons asked for (<see cref="SlotNative.NoticesJson"/>,
/// change bit 64). A notice is an app notification (AppNotificationManager, registered
/// for this unpackaged build under the process's AUMID, MediaViewer.Viewer) unless its
/// screen is the foreground window: clicking it opens that screen, and its one button
/// (Driftwire's Decline) goes back to the add-on. When notifications cannot be shown
/// (registration refused, or the OS has them off) the screen opens instead, as before
/// #452. The keep-alive list goes to the native window (kMsgBackgroundChanged), which
/// owns the notification-area icon and hides rather than closes while it is not empty.
/// Nothing here registers or shows anything until an add-on posts a notice: with
/// Driftwire's toggles off, AppNotificationManager is never touched.
/// </remarks>
public static partial class IslandHost
{
    private sealed record AddonNotice(string Addon, string Id, string Screen, string Title, string Body,
                                      string Action, string ActionLabel, long Revision);

    private static readonly Dictionary<string, AddonNotice> NoticesShown = new(StringComparer.Ordinal);
    private static long _noticeAsks;
    private static string _noticeKeep = "";
    private static bool _notifyRegistered;
    private static bool _notifyFailed;
    private const string NoticeGroup = "mv-notices";

    // src/shell/main.cpp kMsgBackgroundChanged: the native window re-reads the keep list.
    private const uint MsgBackgroundChanged = 0x8000 + 0x81;

    [DllImport("user32.dll", EntryPoint = "PostMessageW")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool NoticePostMessage(IntPtr hwnd, uint msg, IntPtr wparam, IntPtr lparam);

    [DllImport("user32.dll", EntryPoint = "GetForegroundWindow")]
    private static extern IntPtr NoticeForegroundWindow();

    // UI thread (OnSlotsChanged).
    private static void ReadSlotNotices()
    {
        string json = SlotNative.NoticesJson();
        if (json.Length == 0) return;
        var now = new Dictionary<string, AddonNotice>(StringComparer.Ordinal);
        var busy = new Dictionary<string, string>(StringComparer.Ordinal);
        long asks = 0;
        string keep = "";
        try
        {
            using JsonDocument doc = JsonDocument.Parse(json);
            JsonElement r = doc.RootElement;
            asks = (long)JNum(r, "ask", 0);
            if (r.TryGetProperty("keep", out JsonElement k)) keep = k.GetRawText();
            foreach (JsonElement b in JArr(r, "busy"))
            {
                if (JStr(b, "addon") is { Length: > 0 } addon) busy[addon] = JStr(b, "what");
            }
            foreach (JsonElement n in JArr(r, "notices"))
            {
                var notice = new AddonNotice(JStr(n, "addon"), JStr(n, "id"), JStr(n, "screen"), JStr(n, "title"),
                                             JStr(n, "body"), JStr(n, "action"), JStr(n, "action_label"),
                                             (long)JNum(n, "revision", 0));
                if (notice.Addon.Length > 0 && notice.Id.Length > 0) now[NoticeKey(notice)] = notice;
            }
        }
        catch (JsonException)
        {
            return;
        }
        NoteSlotBusy(busy);  // #510 (IslandHost.RestartNotice.cs)
        // Windows asks nothing up front: registering is the only step, made on
        // the first ask (a toggle turned on) or the first notice.
        if (asks > _noticeAsks)
        {
            _noticeAsks = asks;
            _ = EnsureNotifications();
        }
        if (keep != _noticeKeep)
        {
            _noticeKeep = keep;
            if (_themeWindow != IntPtr.Zero) NoticePostMessage(_themeWindow, MsgBackgroundChanged, IntPtr.Zero, IntPtr.Zero);
        }
        foreach (string gone in NoticesShown.Keys.Where(key => !now.ContainsKey(key)).ToList())
        {
            NoticesShown.Remove(gone);
            RemoveNotification(gone);
        }
        foreach (AddonNotice n in now.Values)
        {
            if (NoticesShown.TryGetValue(NoticeKey(n), out AddonNotice? had) && had == n) continue;
            NoticesShown[NoticeKey(n)] = n;
            PresentNotice(n);
        }
    }

    private static string NoticeKey(AddonNotice n) => n.Addon + "/" + n.Id;

    // A notification tag is at most 64 characters.
    private static string NoticeTag(string key) => key.Length <= 64 ? key : key[..64];

    private static void PresentNotice(AddonNotice n)
    {
        // In plain sight: the card itself is the notice.
        if (ScreenWindows.TryGetValue(new ScreenAt(n.Addon, n.Screen), out var open) &&
            WinRT.Interop.WindowNative.GetWindowHandle(open.Window) == NoticeForegroundWindow())
        {
            return;
        }
        if (!EnsureNotifications() || !ShowNotification(n)) NoticeFallback(n);
    }

    private static bool ShowNotification(AddonNotice n)
    {
        try
        {
            if (AppNotificationManager.Default.Setting != AppNotificationSetting.Enabled) return false;
            var b = new AppNotificationBuilder()
                .AddArgument("addon", n.Addon).AddArgument("id", n.Id).AddArgument("screen", n.Screen)
                .AddText(n.Title).AddText(n.Body);
            if (n.Action.Length > 0)
            {
                b.AddButton(new AppNotificationButton(n.ActionLabel)
                    .AddArgument("addon", n.Addon).AddArgument("id", n.Id).AddArgument("screen", n.Screen)
                    .AddArgument("action", n.Action));
            }
            AppNotification toast = b.BuildNotification();
            toast.Tag = NoticeTag(NoticeKey(n));
            toast.Group = NoticeGroup;
            AppNotificationManager.Default.Show(toast);
            return toast.Id != 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex.Message);
            return false;
        }
    }

    private static void RemoveNotification(string key)
    {
        if (!_notifyRegistered) return;
        try
        {
            _ = AppNotificationManager.Default.RemoveByTagAndGroupAsync(NoticeTag(key), NoticeGroup);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex.Message);
        }
    }

    /// <summary>No notifications: the screen opens, as before #452.</summary>
    private static void NoticeFallback(AddonNotice n) => OpenAddonScreenFront(n.Addon, n.Screen);

    /// <summary>
    /// Registers this unpackaged app for notifications once (the process's AUMID,
    /// MediaViewer.Viewer, set by the native host before the first window). False
    /// when the OS refuses; the caller falls back.
    /// </summary>
    private static bool EnsureNotifications()
    {
        if (_notifyRegistered) return true;
        if (_notifyFailed) return false;
        try
        {
            if (!AppNotificationManager.IsSupported())
            {
                _notifyFailed = true;
                return false;
            }
            AppNotificationManager m = AppNotificationManager.Default;
            // Before Register: a click that arrives while this process runs comes here.
            m.NotificationInvoked += OnNotificationInvoked;
            m.Register();
            _notifyRegistered = true;
        }
        catch (Exception ex)
        {
            _notifyFailed = true;
            System.Diagnostics.Debug.WriteLine(ex.Message);
        }
        return _notifyRegistered;
    }

    // Any thread (the notification platform's): hop to the UI thread.
    private static void OnNotificationInvoked(AppNotificationManager sender, AppNotificationActivatedEventArgs args)
    {
        IDictionary<string, string> a = args.Arguments;
        string Arg(string key) => a.TryGetValue(key, out string? v) ? v : "";
        string addon = Arg("addon"), id = Arg("id"), screen = Arg("screen"), action = Arg("action");
        if (addon.Length == 0) return;
        DispatcherQueueControllerTryEnqueue(() =>
        {
            if (action.Length > 0) SlotNative.NoticeAction(addon, id, action);
            else OpenAddonScreenFront(addon, screen);
        });
    }

    // UI thread: the screen's window, opened or brought forward.
    private static void OpenAddonScreenFront(string addon, string screen)
    {
        if (ScreenWindows.TryGetValue(new ScreenAt(addon, screen), out var open))
        {
            open.Window.Activate();
            return;
        }
        SlotNative.OpenScreen(addon, screen, "", SlotTheme(), 0, 0, (float)SlotScale());
    }

    /// <summary>
    /// Native (#452): the notification-area icon's Open &lt;screen&gt;. In: two
    /// NUL-terminated UTF-8 strings, the add-on's id and the screen's id. Returns 0.
    /// </summary>
    public static int OpenAddonScreen(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < 2) return unchecked((int)0x80070057);
            byte[] bytes = new byte[sizeBytes];
            Marshal.Copy(arg, bytes, 0, sizeBytes);
            var parts = new List<string>(2);
            int start = 0;
            for (int i = 0; i < bytes.Length && parts.Count < 2; ++i)
            {
                if (bytes[i] != 0) continue;
                parts.Add(System.Text.Encoding.UTF8.GetString(bytes, start, i - start));
                start = i + 1;
            }
            if (parts.Count != 2) return unchecked((int)0x80070057);
            OpenAddonScreenFront(parts[0], parts[1]);
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }
}
