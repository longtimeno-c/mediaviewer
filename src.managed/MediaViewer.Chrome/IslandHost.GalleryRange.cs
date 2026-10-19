// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The gallery's shift-click range and its share icon (issues #413, #444; the
// Mac twin is FolderStore.extendRange and GalleryShareButton).
//
// Shift-click selects every image from the selected cell (the anchor) to the
// clicked one, in the order the grid shows them (a file search's filter
// counts), and the grid stays open. Arrows still move one cell, and marks stay
// what Insert / Shift+Space edit. Native holds the range as paths
// (chrome_cmd_gallery_range): while it is there, copy, F7/F8, Delete, copy path,
// Share and drag-out act on it. It ends with another selection, a relist, or
// the grid closing, on both sides.
using System.Text.Json;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

public static partial class IslandHost
{
    // Folder indices in the grid's order. Empty: no range.
    private static readonly List<int> _galleryRange = new();

    private static bool ShiftDown() =>
        InputKeyboardSource.GetKeyStateForCurrentThread(Windows.System.VirtualKey.Shift)
            .HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);

    private static void ExtendGalleryRange(int index)
    {
        IList<FolderItemVm> shown = GalleryItems;
        int from = -1, to = -1;
        for (int i = 0; i < shown.Count; ++i)
        {
            if (shown[i].Index == _selectedIndex) from = i;
            if (shown[i].Index == index) to = i;
        }
        if (to < 0) return;
        var range = new List<int>();
        if (from < 0)
        {
            range.Add(index);  // nothing selected among those shown: this cell alone
        }
        else
        {
            for (int i = Math.Min(from, to); i <= Math.Max(from, to); ++i) range.Add(shown[i].Index);
        }
        ClearGalleryRange();
        _galleryRange.AddRange(range);
        foreach (int i in range)
        {
            if (i >= 0 && i < Items.Count) Items[i].InRange = true;
        }
        // Native pulls the indices (TakeTreePath: a command carries one number);
        // the argument is the anchor, which native checks is still selected.
        _treePending = string.Join(",", range);
        Send(Command.GalleryRange, _selectedIndex);
        UpdateGalleryShareButton();
    }

    private static void ClearGalleryRange()
    {
        if (_galleryRange.Count == 0) return;
        foreach (int i in _galleryRange)
        {
            if (i >= 0 && i < Items.Count) Items[i].InRange = false;
        }
        _galleryRange.Clear();
        UpdateGalleryShareButton();
    }

    private static Button BuildGalleryShareButton()
    {
        var b = new Button
        {
            Content = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                Spacing = 6,
                Children =
                {
                    new FontIcon { Glyph = "\uE72D", FontFamily = SymbolFont, FontSize = 13, Foreground = Brush(Title) },
                    Text("Share", Title, UiFontSize - 2),
                },
            },
            Padding = new Thickness(8, 3, 8, 3),
            Background = Brush(Hairline),
            BorderThickness = new Thickness(0),
            CornerRadius = new CornerRadius(4),
            IsTabStop = true,
        };
        // Issue #444: the share menu (Driftwire, then Windows Share) for the
        // selection, through the one share command.
        ToolTipService.SetToolTip(b, "Share the selection: Driftwire, Mail, Phone Link and more");
        b.Click += (_, _) =>
        {
            try { RequestShare(ShareTargetMenu, ShareFromGallery, -1, b); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        };
        UpdateGalleryShareButton(b);
        return b;
    }

    private static void UpdateGalleryShareButton(Button? button = null)
    {
        Button? b = button ?? _galleryShare;
        if (b is null) return;
        int n = _galleryRange.Count;
        UpdateBarShareButton();
        AutomationProperties.SetName(b, n > 1 ? $"Share {n} files" : "Share the selection");
        if (b.Content is StackPanel { Children.Count: 2 } row && row.Children[1] is TextBlock t)
        {
            t.Text = n > 1 ? $"Share {n}" : "Share";
        }
        b.Visibility = Items.Count > 0 && (_selectedIndex >= 0 || n > 0) ? Visibility.Visible : Visibility.Collapsed;
    }
}
