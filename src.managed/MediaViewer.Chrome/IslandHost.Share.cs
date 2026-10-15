// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using System.Text.Json;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Windows.ApplicationModel.DataTransfer;
using Windows.Storage;

namespace MediaViewer.Chrome;

/// <summary>
/// Issue #444 (docs/design/16 "Share"): one share command, four ways in.
/// Ctrl+Shift+S, the gallery's share icon, the command bar's share button
/// (beside Home) and the right-click Share submenu all post the command
/// table's <c>share</c> id; native picks the files (the range, else the marks,
/// else the current file, or the right-clicked file when it is not one of
/// them), walks the pair halves on its pool and hands them back through
/// <see cref="ShareMenu"/>. The menu is Sharepoint first (or its one-click
/// install), then Windows Share (Mail, Phone Link, Nearby Share, apps). The
/// sheet is a WinRT object bound to the native window through
/// <c>IDataTransferManagerInterop</c>, the contract SMTC uses
/// (IslandHost.Video.cs). Nothing is read or sent until a target is picked.
/// The Mac twin is ShareMenu.swift.
/// </summary>
public static partial class IslandHost
{
    // shell::share_target and shell::share_from (src/shell/share_menu.h).
    private const int ShareTargetMenu = 0, ShareTargetSharepoint = 1, ShareTargetSystem = 2;
    private const int ShareFromKey = 0, ShareFromBar = 1, ShareFromGallery = 2, ShareFromContext = 3;

    private static DataTransferManager? _share;
    private static IntPtr _shareHwnd;
    private static string[] _sharePaths = Array.Empty<string>();

    // Where a request from the chrome asked (the icon, the bar button, the
    // right-clicked tile and point), until native answers.
    private static FrameworkElement? _shareAnchor;
    private static Windows.Foundation.Point? _shareAt;
    private static Button? _barShareButton;

    /// <summary>
    /// The island's command carries one float: (clicked + 1) * 16 + from * 4 + target,
    /// as shell::pack_share_request (tests/test_share_menu.cpp pins the layout).
    /// </summary>
    private static float PackShareRequest(int target, int from, int clicked)
    {
        int c = clicked >= 0 && clicked < (1 << 20) - 1 ? clicked + 1 : 0;
        return c * 16 + from * 4 + target;
    }

    /// <summary>Runs the share command from the chrome: the menu, or one of its entries.</summary>
    private static void RequestShare(int target, int from, int clicked, FrameworkElement? anchor,
                                     Windows.Foundation.Point? at = null)
    {
        _shareAnchor = anchor;
        _shareAt = at;
        Send(Command.Share, PackShareRequest(target, from, clicked));
    }

    /// <summary>
    /// Native, on the UI thread, once the files are gathered. In: { int64 hwnd;
    /// int32 target; int32 from; int32 bytes; int32 reserved; UTF-8 JSON
    /// {"files": [...], "media": [...]} } (chrome_share_args). 0 when handled.
    /// </summary>
    public static int ShareMenu(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < 24) return unchecked((int)0x80070057);
            IntPtr hwnd = checked((IntPtr)Marshal.ReadInt64(arg));
            int target = Marshal.ReadInt32(arg, 8);
            int from = Marshal.ReadInt32(arg, 12);
            int len = Marshal.ReadInt32(arg, 16);
            if (hwnd == IntPtr.Zero || len <= 0 || 24 + len > sizeBytes) return unchecked((int)0x80070057);
            byte[] bytes = new byte[len];
            Marshal.Copy(arg + 24, bytes, 0, len);
            List<string> files, media;
            using (JsonDocument doc = JsonDocument.Parse(bytes))
            {
                files = JArr(doc.RootElement, "files").Select(e => e.GetString() ?? "").Where(s => s.Length > 0).ToList();
                media = JArr(doc.RootElement, "media").Select(e => e.GetString() ?? "").Where(s => s.Length > 0).ToList();
            }
            if (files.Count == 0) return 1;
            (FrameworkElement? anchor, Windows.Foundation.Point? at) = ShareAnchorFor(from);
            _shareAnchor = null;
            _shareAt = null;
            switch (target)
            {
                case ShareTargetSharepoint:
                    ShareWithSharepoint(files, anchor, at);
                    return 0;
                case ShareTargetSystem:
                    return ShowSystemShare(hwnd, media);
                default:
                    return ShowShareFlyout(hwnd, files, media, anchor, at) ? 0 : 1;
            }
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    // The key and the bar anchor at the bar's share button; the icon at
    // itself; a right-click where it was clicked. Fallback: the bar's island.
    private static (FrameworkElement?, Windows.Foundation.Point?) ShareAnchorFor(int from)
    {
        if (from != ShareFromKey && _shareAnchor is { XamlRoot: not null } asked) return (asked, _shareAt);
        if (from == ShareFromGallery && _galleryShare is { XamlRoot: not null } icon) return (icon, null);
        if (_barShareButton is { XamlRoot: not null, Visibility: Visibility.Visible } bar && _barPathRow?.Visibility == Visibility.Visible)
            return (bar, null);
        return (_source?.Content as FrameworkElement, null);
    }

    /// <summary>The share menu: Sharepoint (or its install), then Windows Share.</summary>
    private static bool ShowShareFlyout(IntPtr hwnd, List<string> files, List<string> media,
                                        FrameworkElement? anchor, Windows.Foundation.Point? at)
    {
        if (anchor?.XamlRoot is null) return false;
        var menu = new MenuFlyout { MenuFlyoutPresenterStyle = MenuFlyoutPresenterStyle() };
        foreach (MenuFlyoutItemBase item in ShareEntries(
                     () => ShareWithSharepoint(files, anchor, at),
                     () => ShowSystemShare(hwnd, media),
                     media.Count > 0))
        {
            menu.Items.Add(item);
        }
        menu.Closed += (_, _) => RestoreCanvasFocus();
        if (at is { } p)
        {
            menu.ShouldConstrainToRootBounds = false;
            menu.ShowAt(anchor, new FlyoutShowOptions { Position = p });
        }
        else
        {
            menu.Placement = FlyoutPlacementMode.BottomEdgeAlignedRight;
            menu.ShowAt(anchor);
        }
        return true;
    }

    /// <summary>The menu's entries, the same for the flyout and the right-click submenu.</summary>
    private static IEnumerable<MenuFlyoutItemBase> ShareEntries(Action sharepoint, Action system, bool systemEnabled)
    {
        bool have = SharepointReady || SharepointInstalled;
        MenuFlyoutItem nearby = Item(have ? "Sharepoint" : "Install Sharepoint…", null, sharepoint);
        nearby.Icon = new FontIcon { Glyph = "", FontFamily = SymbolFont };
        ToolTipService.SetToolTip(nearby, "Send to a MediaViewer nearby, over Wi-Fi or Bluetooth");
        AutomationProperties.SetName(nearby, have ? "Send with Sharepoint" : "Install Sharepoint");
        yield return nearby;
        yield return Sep();
        MenuFlyoutItem windows = Item("Windows Share…", null, () => system());
        windows.Icon = new FontIcon { Glyph = "", FontFamily = SymbolFont };
        windows.IsEnabled = systemEnabled;
        ToolTipService.SetToolTip(windows, "Mail, Phone Link, Nearby Share and other apps");
        yield return windows;
    }

    /// <summary>
    /// A right-click's Share submenu, for the stop at <paramref name="clicked"/>
    /// (-1: the file on screen). Picking an entry runs the share command with
    /// that entry; native resolves the files (the selection when the clicked
    /// stop is part of it, else that stop alone).
    /// </summary>
    private static MenuFlyoutSubItem ShareSubItem(int clicked, FrameworkElement anchor, Windows.Foundation.Point? at)
    {
        var sub = new MenuFlyoutSubItem
        {
            Text = "Share",
            Icon = new FontIcon { Glyph = "", FontFamily = SymbolFont },
        };
        foreach (MenuFlyoutItemBase item in ShareEntries(
                     () => RequestShare(ShareTargetSharepoint, ShareFromContext, clicked, anchor, at),
                     () => RequestShare(ShareTargetSystem, ShareFromContext, clicked, anchor, at),
                     true))
        {
            sub.Items.Add(item);
        }
        return sub;
    }

    /// <summary>
    /// A tile's (or the canvas's) right-click menu: the add-ons' entries, then
    /// Share. Never null: Share is always there (issue #444).
    /// </summary>
    private static MenuFlyout TileContextMenuFor(FolderItemVm vm, FrameworkElement anchor, Windows.Foundation.Point? at)
    {
        MenuFlyout menu = SlotContextMenuFor(vm) ?? new MenuFlyout();
        menu.MenuFlyoutPresenterStyle ??= MenuFlyoutPresenterStyle();
        if (menu.Items.Count > 0) menu.Items.Add(Sep());
        menu.Items.Add(ShareSubItem(vm.Index, anchor, at));
        return menu;
    }

    /// <summary>An add-on context entry the Share submenu already carries: Sharepoint's
    /// "Send with Sharepoint…" (its manifest's <c>send_files</c>). Left out of the
    /// right-click menus, so sending goes one way, through the share command.</summary>
    private static bool IsShareRoutedSlotItem(SlotItem item) =>
        item.Addon == SharepointId && item.Id == "send_files";

    // ---- the command bar's share button (beside Home) -------------------------------------

    private static Button BuildBarShareButton()
    {
        Button b = PathIconButton("", () => RequestShare(ShareTargetMenu, ShareFromBar, -1, _barShareButton));
        AutomationProperties.SetName(b, "Share");
        ToolTipService.SetToolTip(b, "Share: Sharepoint, Mail, Phone Link and more" + KeyNote(Command.Share));
        _barShareButton = b;
        UpdateBarShareButton();
        return b;
    }

    /// <summary>Off with nothing to share: no file on screen and no gallery selection.</summary>
    private static void UpdateBarShareButton()
    {
        if (_barShareButton is null) return;
        bool any = Items.Count > 0 && ((_selectedIndex >= 0 && _selectedIndex < Items.Count) || _galleryRange.Count > 0);
        if (_barShareButton.IsEnabled != any) _barShareButton.IsEnabled = any;
    }

    // ---- Windows Share ---------------------------------------------------------------------

    /// <summary>Windows Share over the native window with these media files. 0 when asked for.</summary>
    private static int ShowSystemShare(IntPtr hwnd, List<string> media)
    {
        if (hwnd == IntPtr.Zero || media.Count == 0) return 1;
        _sharePaths = media.ToArray();
        if (_share is null || _shareHwnd != hwnd)
        {
            if (_share is not null) _share.DataRequested -= OnShareRequested;
            _share = DataTransferManagerInterop.GetForWindow(hwnd);
            _shareHwnd = hwnd;
            _share.DataRequested += OnShareRequested;
        }
        DataTransferManagerInterop.ShowShareUIForWindow(hwnd);
        return 0;
    }

    private static async void OnShareRequested(DataTransferManager sender, DataRequestedEventArgs args)
    {
        DataRequest request = args.Request;
        DataRequestDeferral deferral = request.GetDeferral();
        try
        {
            string[] paths = _sharePaths;
            var files = new List<IStorageItem>(paths.Length);
            foreach (string p in paths)
            {
                try { files.Add(await StorageFile.GetFileFromPathAsync(p)); }
                catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
            }
            if (files.Count == 0)
            {
                request.FailWithDisplayText("The file is no longer there.");
                return;
            }
            // The sheet's heading: the one file's name, else a count. Local UI only.
            request.Data.Properties.Title = files.Count == 1 ? files[0].Name : $"{files.Count} files";
            request.Data.SetStorageItems(files, true);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
        }
        finally
        {
            deferral.Complete();
        }
    }
}
