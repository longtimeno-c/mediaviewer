// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

/// <summary>
/// docs/plans/edit-refresh.md S5 (owner call B): Enhance's docked screen
/// (manifest place "edit", contribution API 12) drawn in the Edit workspace's
/// Enhance tab, under the card of the tool it is on (its tree's root is
/// "tool.&lt;remove|restyle|detail|extend|faces&gt;", faces under the Faces card): the tool's knobs, the preview,
/// the glowing progress with the time left, Cancel, and the result row. Native
/// opens it (main.cpp enhance_tool, through mv_slots_screen_command) and closes
/// it with Edit or another photo; it is never a window (SyncScreenWindows skips
/// place "edit"). The row's Show and Keep editing are the viewer's
/// (TakeScreenButton); Move to Recycle Bin is Enhance's (the host table's
/// recycle_file). The Mac twin is DockedToolPanel in EnhanceHubView.swift.
/// </summary>
/// <remarks>Only Button, TextBlock, Border, Grid, StackPanel and ContentControl
/// (tools/check-winui-controls.ps1); the tree itself is the screens' renderer.</remarks>
public static partial class IslandHost
{
    // The docked screen as the core last listed it (a clone: the list's document
    // is disposed), the revision and tool drawn, and the new file the result
    // row's thumbnail names.
    private static JsonElement? _enhanceDock;
    private static long _enhanceDockRevision = -1;
    private static string _enhanceDockTool = "";
    private static string _enhanceDockResult = "";
    // Each dockable card's place for it: the panel (hairline and close) and the tree's host.
    private static readonly Dictionary<EnhanceToolKind, (StackPanel Panel, ContentControl Body)> EnhanceDockHosts = new();

    private static string? EnhancePhotoOf(EnhanceToolKind kind) => kind switch
    {
        EnhanceToolKind.Remove => "remove",
        EnhanceToolKind.Restyle => "restyle",
        EnhanceToolKind.Detail => "detail",
        EnhanceToolKind.Extend => "extend",
        EnhanceToolKind.RestoreFaces => "faces",
        _ => null,
    };

    // The docked tree a card draws under it: its own tool's, and for the Faces
    // card Restore faces' (S10: its second button), Open closed eyes staying native's.
    private static string? EnhanceDockWordOf(EnhanceToolKind kind) =>
        kind == EnhanceToolKind.Faces ? "faces" : EnhancePhotoOf(kind);

    // The card the docked screen is on, if any.
    private static bool EnhanceDockedOn(EnhanceToolKind kind) =>
        _enhanceDock is not null && _enhanceDockTool.Length > 0 && EnhanceDockWordOf(kind) == _enhanceDockTool;

    /// <summary>Under a card: the docked panel's place, collapsed until the screen is on that tool.</summary>
    private static StackPanel BuildEnhanceDockHost(EnhanceToolKind kind)
    {
        var panel = new StackPanel { Spacing = Space(10), Visibility = Visibility.Collapsed, Margin = Pad(0, 4, 0, 0) };
        var head = new Grid { ColumnSpacing = Space(6) };
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.Children.Add(new Border { Height = 1, Background = Brush(Hairline), VerticalAlignment = VerticalAlignment.Center });
        Button close = ChromeButton(new FontIcon { Glyph = "", FontFamily = IconFont, FontSize = TypeSize(10) },
            CompactButtonPadding, () => SlotNative.CloseScreen("enhance", "tool"),
            name: "Close the tool panel", tip: "Close (a run in progress stops)");
        Grid.SetColumn(close, 1);
        head.Children.Add(close);
        panel.Children.Add(head);
        var body = new ContentControl { HorizontalContentAlignment = HorizontalAlignment.Stretch, IsTabStop = false };
        panel.Children.Add(body);
        EnhanceDockHosts[kind] = (panel, body);
        return panel;
    }

    // SyncScreenWindows: the core's list. The docked screen is drawn when its
    // tree's revision moves; otherwise only its preview's pixels can have moved.
    private static void SyncEnhanceDock(JsonElement list)
    {
        JsonElement? found = null;
        foreach (JsonElement s in list.EnumerateArray())
        {
            if (JStr(s, "place") != "edit" || JStr(s, "addon") != "enhance") continue;
            found = s.Clone();
            break;
        }
        bool had = _enhanceDock is not null;
        _enhanceDock = found;
        if (found is null)
        {
            if (!had) return;
            ForgetSurfaces(new ScreenAt("enhance", "tool"));
            _enhanceDockRevision = -1;
            _enhanceDockTool = "";
            _enhanceDockResult = "";
            DrawEnhanceDock(force: true);
            return;
        }
        DrawEnhanceDock(force: false);
    }

    // Draws the docked tree under its card (BuildEnhanceHub asks again, forced,
    // when the tab is rebuilt), and clears every other card's place.
    private static void DrawEnhanceDock(bool force)
    {
        if (_enhanceDock is not { } s)
        {
            foreach ((StackPanel panel, ContentControl body) in EnhanceDockHosts.Values)
            {
                body.Content = null;
                panel.Visibility = Visibility.Collapsed;
            }
            if (_editTab == TabEnhance) RenderEnhanceInPlace();
            return;
        }
        var at = new ScreenAt(JStr(s, "addon"), JStr(s, "id"));
        long revision = (long)JNum(s, "revision", 0);
        if (!s.TryGetProperty("tree", out JsonElement tree) || tree.ValueKind != JsonValueKind.Object) return;
        string root = JStr(tree, "id");
        string tool = root.StartsWith("tool.", StringComparison.Ordinal) ? root[5..] : "";
        if (!force && revision == _enhanceDockRevision && tool == _enhanceDockTool)
        {
            RefreshSurfaces(at);
            return;
        }
        if (ScreenPressed.Contains(at)) return;  // #511: the release catches up
        _enhanceDockResult = JStr(FindScreenNode(tree, "result.thumb") ?? default, "path");
        foreach ((EnhanceToolKind kind, (StackPanel panel, ContentControl body)) in EnhanceDockHosts)
        {
            if (EnhanceDockWordOf(kind) != tool)
            {
                body.Content = null;
                panel.Visibility = Visibility.Collapsed;
                continue;
            }
            ScreenPlace? kept = NoteScreenPlace(body.Content as UIElement);
            FrameworkElement content = BuildSettingsScreenContent(tree, at);
            if (kept is { } k) RestoreScreenPlace(content, k);
            body.Content = content;
            panel.Visibility = Visibility.Visible;
        }
        _enhanceDockRevision = revision;
        _enhanceDockTool = tool;
        if (_editTab == TabEnhance) RenderEnhanceInPlace();
    }

    private static JsonElement? FindScreenNode(JsonElement node, string id)
    {
        if (node.ValueKind != JsonValueKind.Object) return null;
        if (JStr(node, "id") == id) return node;
        foreach (JsonElement child in JArr(node, "children"))
        {
            if (FindScreenNode(child, id) is { } hit) return hit;
        }
        return null;
    }

    /// <summary>
    /// The screens' renderer asks before a button tells its add-on: true when
    /// the host took the click. Enhance's docked row: Show and Keep editing
    /// navigate (the viewer's). Remove has no run button of its own in the
    /// panel: the card's Remove (or Return in the brush) hands the strokes over.
    /// </summary>
    private static bool TakeScreenButton(ScreenAt at, string id)
    {
        if (at.Addon != "enhance" || at.Screen != "tool") return false;
        switch (id)
        {
            case "result.show":
            case "result.keep":
                if (_enhanceDockResult.Length == 0) return true;
                _treePending = _enhanceDockResult;
                Send(Command.EditAction, id == "result.keep" ? EditActions.EnhanceResultKeep : EditActions.EnhanceResultShow);
                return true;
            default:
                return false;
        }
    }
}
