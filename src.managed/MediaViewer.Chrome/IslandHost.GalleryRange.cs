// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The gallery's shift-click range (issues #413, #444; the Mac twin is
// FolderStore.extendRange). Share is the command bar's button (IslandHost.Share.cs).
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
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

public static partial class IslandHost
{
    // Folder indices in the grid's order. Empty: no range.
    private static readonly List<int> _galleryRange = new();

    // The accent ring is the keyboard's tile (docs/design/20 "Craft rules"); the
    // range is its wash. In a contrast theme that wash is transparent
    // (IslandHost.Theme.cs), so there the range keeps the ring.
    private static bool TileRinged(FolderItemVm vm) =>
        (vm.Selected && _folderCursor < 0) || (vm.InRange && Brush(ChromeColour.RangeWash).Color.A == 0);

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
        UpdateBarShareButton();
    }

    private static void ClearGalleryRange()
    {
        if (_galleryRange.Count == 0) return;
        foreach (int i in _galleryRange)
        {
            if (i >= 0 && i < Items.Count) Items[i].InRange = false;
        }
        _galleryRange.Clear();
        UpdateBarShareButton();
    }
}
