// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;

namespace MediaViewer.Chrome;

/// <summary>
/// An add-on's settings page (contribution API 8, place "settings"): its view
/// tree drawn on its page in Settings → Add-ons, under the add-on's card, with
/// the screens' renderer (its cards are siblings of that card, never inside
/// it). Opened while the page shows and Settings is up, closed when either
/// goes; never a window. Driftwire's is the window's settings. Phone access's
/// is drawn in Settings → Phone access instead, while no add-on page shows
/// (IslandHost.PhoneAccess.cs): one settings screen is open at a time. The
/// Mac's twin is AddonSettingsScreen in AddonListView.swift.
/// </summary>
public static partial class IslandHost
{
    // Each loaded add-on's settings screen, from the contributions (addon → id).
    private static readonly Dictionary<string, string> SlotSettingsScreens = new(StringComparer.Ordinal);
    // The one open, and the revision drawn.
    private static ScreenAt? _settingsScreenAt;
    private static long _settingsScreenRevision = -1;
    private static ContentControl? _addonPageSettings;
    // Contribution API 15: what is typed in a secure field, by screen and node,
    // until the screen closes (IslandHost.Screens.cs); never in a tree.
    private static readonly Dictionary<string, string> ScreenSecrets = new(StringComparer.Ordinal);

    // Where the open settings screen is drawn: its add-on page, or Phone access's section.
    private static ContentControl? SettingsScreenHost =>
        _settingsScreenAt?.Addon == PhoneAccessId ? _phoneAccessScreen : _addonPageSettings;

    // From NoteSlotWindowScreens: the contributions' settings screens.
    private static void NoteSlotSettingsScreens(JsonElement root)
    {
        SlotSettingsScreens.Clear();
        foreach (JsonElement s in JArr(root, "screens"))
        {
            if (JStr(s, "place") != "settings") continue;
            string addon = JStr(s, "addon");
            if (addon.Length > 0 && !SlotSettingsScreens.ContainsKey(addon)) SlotSettingsScreens[addon] = JStr(s, "id");
        }
    }

    /// <summary>Under the page's card: the settings screen's tree. Built once.</summary>
    private static ContentControl BuildAddonPageSettings() => _addonPageSettings = new ContentControl
    {
        HorizontalContentAlignment = HorizontalAlignment.Stretch,
        IsTabStop = false,
        Margin = Pad(0, 4, 0, 0),
    };

    /// <summary>
    /// The page that shows and the screen that is open agree: opens the open
    /// slot add-on page's settings screen, closes any other. Called when a page
    /// opens or closes, Settings hides, and the contributions change (an add-on
    /// loaded or reloaded: <paramref name="reopen"/> asks again, a no-op in the
    /// core when it is still open).
    /// </summary>
    private static void SyncAddonSettingsScreen(bool reopen = false)
    {
        ScreenAt? want = null;
        if (_settingsVisible && _slotPageId is { } id)
        {
            if (id != PhoneAccessId && SlotSettingsScreens.TryGetValue(id, out string? screen)) want = new ScreenAt(id, screen);
        }
        else if (_settingsVisible && SlotSettingsScreens.TryGetValue(PhoneAccessId, out string? phone))
        {
            // Settings' list shows, and with it Phone access's section.
            want = new ScreenAt(PhoneAccessId, phone);
        }
        if (_settingsScreenAt == want && !reopen) return;
        if (_settingsScreenAt is { } was && was != want)
        {
            SlotNative.CloseScreen(was.Addon, was.Screen);
            string prefix = $"{was.Addon}/{was.Screen}/";
            foreach (string k in ScreenSecrets.Keys.Where(k => k.StartsWith(prefix, StringComparison.Ordinal)).ToList())
            {
                ScreenSecrets.Remove(k);
            }
        }
        if (_settingsScreenAt != want)
        {
            if (SettingsScreenHost is { } host) host.Content = null;
            _settingsScreenAt = want;
            _settingsScreenRevision = -1;
            if (SettingsScreenHost is { } next) next.Content = null;
        }
        if (want is { } at) SlotNative.OpenScreen(at.Addon, at.Screen, "", SlotTheme(), 0, 0, (float)SlotScale());
    }

    // SyncScreenWindows: the core's list. The open settings screen is drawn when
    // its tree's revision moves; gone from the list (its add-on stopped), its
    // tree goes until the add-on is back.
    private static void SyncSettingsScreen(JsonElement list)
    {
        if (_settingsScreenAt is not { } at || SettingsScreenHost is not { } host) return;
        foreach (JsonElement s in list.EnumerateArray())
        {
            if (JStr(s, "addon") == at.Addon && JStr(s, "id") == at.Screen)
            {
                DrawSettingsScreen(s, at);
                return;
            }
        }
        host.Content = null;
        _settingsScreenRevision = -1;
    }

    private static void DrawSettingsScreen(JsonElement s, ScreenAt at)
    {
        if (SettingsScreenHost is not { } host) return;
        long revision = (long)JNum(s, "revision", 0);
        if (revision == _settingsScreenRevision || ScreenPressed.Contains(at)) return;
        if (!s.TryGetProperty("tree", out JsonElement tree) || tree.ValueKind != JsonValueKind.Object) return;
        ScreenPlace? kept = NoteScreenPlace(host.Content as UIElement);
        FrameworkElement content = BuildSettingsScreenContent(tree, at);
        if (kept is { } k) RestoreScreenPlace(content, k);
        host.Content = content;
        _settingsScreenRevision = revision;
    }

    private static FrameworkElement BuildSettingsScreenContent(JsonElement tree, ScreenAt at)
    {
        // The window's face, as the add-on's own window draws the same tree.
        var ids = new Dictionary<string, FrameworkElement>(StringComparer.Ordinal);
        _screenFace = ScreenWindowFace();
        _screenIds = ids;
        FrameworkElement root;
        try
        {
            root = (FrameworkElement)BuildScreenNode(tree, at);
        }
        finally
        {
            _screenFace = null;
            _screenIds = null;
        }
        ScreenIdsOf.AddOrUpdate(root, ids);
        // #511: no rebuild between a press and its release.
        root.AddHandler(UIElement.PointerPressedEvent, new PointerEventHandler((_, _) => ScreenPressed.Add(at)), true);
        var released = new PointerEventHandler((_, _) =>
        {
            if (ScreenPressed.Remove(at)) DispatcherQueueControllerTryEnqueue(ReadSlotScreens);
        });
        root.AddHandler(UIElement.PointerReleasedEvent, released, true);
        root.AddHandler(UIElement.PointerCanceledEvent, released, true);
        root.AddHandler(UIElement.PointerCaptureLostEvent, released, true);
        return root;
    }
}
