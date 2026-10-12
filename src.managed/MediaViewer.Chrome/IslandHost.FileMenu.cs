// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

/// <summary>
/// Issue #220: the file and navigation commands the Mac's File, View and Go
/// menus carry (main_mac.mm), on the Windows command bar's Open and View
/// flyouts so the mouse reaches them. Each item posts the command-table id its
/// key posts; native runs it through the same switch (docs/design/16).
/// </summary>
public static partial class IslandHost
{
    /// <summary>Open: Mark / Unmark, Copy / Move Marked To…, Recycle Bin, Share, Copy Path, Copy Edited Image.</summary>
    private static void AddFileCommandItems(MenuFlyout flyout)
    {
        bool any = _selectedIndex >= 0 && _selectedIndex < Items.Count;
        flyout.Items.Add(Sep());
        flyout.Items.Add(FileItem("Mark / Unmark", Command.ToggleMark, any));
        flyout.Items.Add(FileItem("Copy Marked To…", Command.CopyToPick, any));
        flyout.Items.Add(FileItem("Move Marked To…", Command.MoveToPick, any));
        flyout.Items.Add(FileItem("Move to Recycle Bin", Command.DeleteToRecycleBin, any));
        flyout.Items.Add(Sep());
        flyout.Items.Add(FileItem("Share…", Command.Share, any));
        flyout.Items.Add(FileItem("Copy Path", Command.CopyPath, any));
        flyout.Items.Add(FileItem("Copy Edited Image", Command.CopyFlattened, any));
    }

    /// <summary>View: Start Slideshow and a Go submenu (Next, Previous, First, Last).</summary>
    private static void AddGoItems(MenuFlyout flyout)
    {
        flyout.Items.Add(Item("Start Slideshow", KeyOf(Command.SlideshowStart), () => SendFromMenu(Command.SlideshowStart)));
        var go = new MenuFlyoutSubItem { Text = "Go" };
        go.Items.Add(Item("Next", KeyOf(Command.Next), () => SendFromMenu(Command.Next)));
        go.Items.Add(Item("Previous", KeyOf(Command.Prev), () => SendFromMenu(Command.Prev)));
        go.Items.Add(Item("First", KeyOf(Command.First), () => SendFromMenu(Command.First)));
        go.Items.Add(Item("Last", KeyOf(Command.Last), () => SendFromMenu(Command.Last)));
        flyout.Items.Add(go);
    }

    private static MenuFlyoutItem FileItem(string text, int command, bool enabled)
    {
        // The shortcut column is the live table's (issue #197), like the rest of the bar.
        MenuFlyoutItem item = Item(text, KeyOf(command), () => SendFromMenu(command));
        item.IsEnabled = enabled;
        return item;
    }

    // As Open's items: the command runs as if its key were pressed on the
    // canvas, and the keyboard goes back there.
    private static void SendFromMenu(int command)
    {
        Send(command);
        RestoreCanvasFocus();
    }
}
