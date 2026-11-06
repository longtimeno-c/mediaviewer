// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;

namespace MediaViewer.Chrome;

/// <summary>
/// docs/plans/edit-refresh.md S2 (§3.1): the Edit workspace's Enhance tab (tab 5)
/// and the header's ✦ flyout. Five tools, one card each: Remove objects,
/// Restyle, Faces, Enhance image, Extend edges. A card says what the tool does,
/// whether it can run now (its chip) and has one primary button. They launch
/// what exists: the Remove brush, Enhance's own sheet started on the right
/// choice (its "open" and "restyle" commands), Extend's frame on the canvas (S6)
/// and the Faces tool (S8: a box per face on the photo and the Faces
/// panel here, IslandHost.Faces.cs). Every result is a new file; the original
/// never changes. S5 (owner call B): Remove, Restyle, Enhance image and Extend
/// run inside the pane, Enhance's docked screen drawn under the card
/// (IslandHost.EnhanceDock.cs); Extend's Fill runs there on the frame. The Mac
/// twin is EnhanceHubView.swift.
/// </summary>
/// <remarks>
/// Only Button, TextBlock, Border, Grid and StackPanel, and a plain Flyout of
/// buttons for the ✦ chevron (tools/check-winui-controls.ps1). The Windows
/// brush is the adjust pane's pick until S3/S4a: "Start brushing" shows Adjust
/// with the brush on.
/// </remarks>
public static partial class IslandHost
{
    // RestoreFaces (S10) is the Faces card's second button and a flyout row of
    // its own, not a sixth card.
    private enum EnhanceToolKind { Remove, Restyle, Faces, Detail, Extend, RestoreFaces }

    private sealed record EnhanceToolInfo(EnhanceToolKind Kind, string Title, string Purpose, string Glyph);

    private static readonly EnhanceToolInfo[] EnhanceTools =
    {
        new(EnhanceToolKind.Remove, "Remove objects",
            "Brush over something to take it out.", RailGlyph.Remove),
        new(EnhanceToolKind.Restyle, "Restyle",
            "Describe a new look: a snowy day, golden hour.", ""),  // Color
        new(EnhanceToolKind.Faces, "Faces",
            "Open closed eyes, or a better face of the same person.", RailGlyph.Faces),
        new(EnhanceToolKind.Detail, "Enhance image",
            "Sharpen, clean up noise or blur, or enlarge.", "\uE71E"),  // Zoom: more detail, never a sparkle
        new(EnhanceToolKind.Extend, "Extend edges",
            "Add more picture around the edges.", RailGlyph.Extend),
    };

    // S10: Restore faces, in the flyout after Faces.
    private static readonly EnhanceToolInfo RestoreFacesTool = new(EnhanceToolKind.RestoreFaces, "Restore faces",
        "Sharper, clearer faces, each checked by People to still look like the same person. Saves a new file.",
        RailGlyph.Faces);

    // shell/adjust_pane.h kWand*: what chrome_edit_args.wand says.
    private const int WandBrushOn = 1, WandBrushStrokes = 2, WandBrushFull = 4, WandRestyle = 8, WandEyesBusy = 16,
                      WandExtend = 32, WandDetail = 64, WandFacesReady = 128, WandAutoOn = 256, WandAutoBusy = 512,
                      WandRestoreFaces = 1024;

    private readonly record struct EnhanceToolState(string Chip, bool Ready, bool Busy, string Action, bool Enabled,
                                                    bool NeedsAddon);

    private static EnhanceToolState EnhanceStateOf(EnhanceToolKind kind)
    {
        bool brushOn = (_editWand & WandBrushOn) != 0, brushed = (_editWand & WandBrushStrokes) != 0;
        if (!_editEnhance && kind != EnhanceToolKind.Faces)
            return new("Needs Enhance", false, false, "Get Enhance…", true, true);
        return kind switch
        {
            EnhanceToolKind.Remove when brushOn && brushed =>
                new((_editWand & WandBrushFull) != 0 ? "Brush full" : "Brushed", true, false, "Remove", true, false),
            EnhanceToolKind.Remove when brushOn => new("Brushing", true, false, "Done brushing", true, false),
            EnhanceToolKind.Remove => new("Ready", true, false, "Start brushing", true, false),
            // One-click: offered whenever the add-on's cards have the tool (never a
            // palette command: the owner's "Not offered", 2026-11-05).
            EnhanceToolKind.Restyle or EnhanceToolKind.Detail => EnhanceOffered(kind),
            EnhanceToolKind.Faces when (_editWand & WandEyesBusy) != 0 =>
                new("Working…", false, true, "Choose faces", false, false),
            // S8: the card arms the Faces tool; the panel takes over while it is.
            EnhanceToolKind.Faces when _facesArmed => new("Choosing", true, false, "Done", true, false),
            EnhanceToolKind.Faces => (_editWand & WandFacesReady) != 0
                ? new("Ready", true, false, "Choose faces", true, false)
                : new("Needs People", false, false, "Choose faces", true, false),
            EnhanceToolKind.RestoreFaces when !EnhanceDockedOn(kind) => EnhanceOffered(kind),
            EnhanceToolKind.RestoreFaces => (_editWand & WandFacesReady) != 0
                ? new("Ready", true, false, "Restore faces", true, false)
                : new("Needs People", false, false, "Restore faces", true, false),
            _ => new("Ready", true, false, "Set the new frame", (_editWand & WandExtend) != 0, false),
        };
    }

    // A tool the cards draw: ready when the add-on's tree has its card, else why
    // not (the cards still opening, an Enhance to update, or one without it).
    private static EnhanceToolState EnhanceOffered(EnhanceToolKind kind)
    {
        string title = kind switch
        {
            EnhanceToolKind.Restyle => "Restyle",
            EnhanceToolKind.Detail => "Enhance image",
            _ => "Restore faces",
        };
        if (EnhanceDockedOn(kind))
        {
            bool working = EnhanceRunWorking && EnhanceDockWordOf(kind) == _enhanceRunTool;
            return new(working ? "Working…" : "Ready", true, working, title, true, false);
        }
        if (_enhanceCardsOutdated) return new("Update Enhance", false, false, "Update Enhance…", true, true);
        if (_enhanceDock is null) return new("Opening…", false, true, title, false, false);
        return new("Not in this Enhance", false, false, title, false, false);
    }

    private static void RunEnhanceTool(EnhanceToolKind kind)
    {
        bool brushOn = (_editWand & WandBrushOn) != 0, brushed = (_editWand & WandBrushStrokes) != 0;
        bool cardsTool = kind is EnhanceToolKind.Restyle or EnhanceToolKind.Detail or EnhanceToolKind.RestoreFaces;
        if ((!_editEnhance && kind != EnhanceToolKind.Faces) || (cardsTool && _enhanceCardsOutdated))
        {
            OpenEnhanceSettings();
            return;
        }
        switch (kind)
        {
            case EnhanceToolKind.Remove:
                // One-click: Remove hands the strokes over and runs (the glow, then
                // the new photo); the tool bar's Remove does the same.
                if (brushOn && brushed) Send(Command.AdjustBrush, 2);
                else if (brushOn) Send(Command.AdjustPick, 100f);      // the brush down (pick 0)
                else Send(Command.EditAction, EditActions.ArmBrush);
                break;
            // Restyle needs its words: the tab, where its card takes them.
            case EnhanceToolKind.Restyle: Send(Command.EditAction, EditActions.ShowEnhance); break;
            // The flyout's Restore faces and Enhance image run at once with the
            // cards' choices (native's enhance_tool, downloading first).
            case EnhanceToolKind.RestoreFaces: Send(Command.EditAction, EditActions.RestoreFaces); break;
            // S8: the Faces tool (a second press puts it down).
            case EnhanceToolKind.Faces: Send(Command.CanvasTool, CanvasTools.Faces * 1000); break;
            case EnhanceToolKind.Detail: Send(Command.EditAction, EditActions.EnhanceDetail); break;
            // S6: Extend's frame on the canvas (canvas_tool 5); Fill on the context bar runs it.
            case EnhanceToolKind.Extend: Send(Command.CanvasTool, CanvasTools.Extend * 1000); break;
        }
    }

    // Settings → Add-ons → Enhance, opened if it is not up (OpenAddonUpdates' route).
    private static void OpenEnhanceSettings()
    {
        const string page = "enhance";
        if (!_settingsVisible)
        {
            _addonPageWanted = page;
            _addonListWanted = false;
            Send(Command.OpenSettings);
            return;
        }
        _showGeneralSettings?.Invoke();
        OpenAddonPage(page);
    }

    // ---- the tab ------------------------------------------------------------------

    private static readonly List<(EnhanceToolKind Kind, TextBlock Chip, Border ChipPill, Button Action, Border Card)>
        EnhanceCards = new();
    private static TextBlock? _enhanceBrushNote;
    private static Button? _enhanceUndoStroke;
    private static Button? _enhanceClearStrokes;
    private static TextBlock? _enhanceRestoreFacesHead;  // S10: the Faces card's Restore faces heading

    private static void DropEnhanceUi()
    {
        EnhanceCards.Clear();
        EnhanceDockHosts.Clear();
        _enhanceRestoreFacesHead = null;
        _enhanceBrushNote = null;
        _enhanceUndoStroke = null;
        _enhanceClearStrokes = null;
        _enhanceLevelChip = null;
        _enhanceLevelText = null;
        ReleaseEnhanceLevel();
        _facesHost = null;
    }

    private static void BuildEnhanceHub(StackPanel pane)
    {
        DropEnhanceUi();
        var head = new StackPanel { Spacing = Space(4) };
        var titleRow = new Grid { ColumnSpacing = Space(8) };
        titleRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        titleRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        TextBlock title = Text("Enhance", Title, UiFontSize + 1);
        title.VerticalAlignment = VerticalAlignment.Center;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetHeadingLevel(
            title, Microsoft.UI.Xaml.Automation.Peers.AutomationHeadingLevel.Level2);
        titleRow.Children.Add(title);
        if (_editEnhance)
        {
            Button chip = BuildEnhanceLevelChip();
            Grid.SetColumn(chip, 1);
            titleRow.Children.Add(chip);
            OpenEnhanceLevel();
        }
        head.Children.Add(titleRow);
        // "The original is not changed" is the result's to say, once (docs/design/20 "Craft rules").
        pane.Children.Add(head);
        if (!_editEnhance)
        {
            var banner = new StackPanel { Spacing = Space(6) };
            banner.Children.Add(Text("Add the Enhance add-on", Title, UiFontSize - 1));
            banner.Children.Add(Hint("Remove, Restyle, Enhance image and Extend run on this PC with models it " +
                                     "downloads when you first use them."));
            Button open = AccentButton("Open Add-ons…", OpenEnhanceSettings);
            open.HorizontalAlignment = HorizontalAlignment.Left;
            banner.Children.Add(open);
            pane.Children.Add(new Border
            {
                Child = banner,
                Padding = Pad(12),
                CornerRadius = Corner(10),
                Background = Brush(ChromeColour.SelectionWash),
            });
        }
        // S8: the Faces panel, first while the Faces tool is armed.
        _facesHost = new StackPanel { Visibility = Visibility.Collapsed };
        pane.Children.Add(_facesHost);
        RenderFaces();
        // Two columns from 560 DIP (ApplyEditWidthClass reparents nothing here:
        // the hub is rebuilt with the tab, and its columns are its own).
        var grid = new Grid { ColumnSpacing = Space(10), RowSpacing = Space(10) };
        _enhanceGrid = grid;
        foreach (EnhanceToolInfo tool in EnhanceTools) grid.Children.Add(BuildEnhanceCard(tool));
        LayEnhanceGrid();
        pane.Children.Add(grid);
        DrawEnhanceDock(force: true);  // one-click: the cards' stacks, when the screen is open
        RenderEnhanceInPlace();
    }

    private static Grid? _enhanceGrid;

    // One column below 560 DIP, two from it (the inspector's wide class).
    private static void LayEnhanceGrid()
    {
        if (_enhanceGrid is null) return;
        int columns = _editWidthClass >= WidthWide ? 2 : 1;
        if (_enhanceGrid.ColumnDefinitions.Count != columns)
        {
            _enhanceGrid.ColumnDefinitions.Clear();
            for (int c = 0; c < columns; c++)
                _enhanceGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        }
        int rows = (EnhanceTools.Length + columns - 1) / columns;
        while (_enhanceGrid.RowDefinitions.Count < rows) _enhanceGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        while (_enhanceGrid.RowDefinitions.Count > rows) _enhanceGrid.RowDefinitions.RemoveAt(_enhanceGrid.RowDefinitions.Count - 1);
        for (int i = 0; i < _enhanceGrid.Children.Count; i++)
        {
            if (_enhanceGrid.Children[i] is not FrameworkElement card) continue;
            Grid.SetColumn(card, i % columns);
            Grid.SetRow(card, i / columns);
        }
    }

    private static Border BuildEnhanceCard(EnhanceToolInfo tool)
    {
        var col = new StackPanel { Spacing = Space(8) };
        // docs/design/20 "Craft rules": icon and title, one short line, the
        // controls, one primary button; a chip only when something is needed.
        var head = new Grid { ColumnSpacing = Space(8) };
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.Children.Add(new FontIcon
        {
            Glyph = tool.Glyph, FontFamily = IconFont, FontSize = TypeSize(15), Width = 20,
            Foreground = Brush(ChromeColour.Accent), VerticalAlignment = VerticalAlignment.Center,
        });
        TextBlock name = Text(tool.Title, Title, UiFontSize - 1, maxLines: 1);
        name.TextTrimming = TextTrimming.CharacterEllipsis;
        name.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(name, 1);
        head.Children.Add(name);
        TextBlock chipText = Text("", Body, UiFontSize - 5, maxLines: 1);
        // What is needed, in words on a quiet fill: no dot, no outline.
        var chip = new Border
        {
            Child = chipText,
            Padding = Pad(7, 2, 7, 2),
            CornerRadius = new CornerRadius(999),
            Background = Brush(ChromeColour.Surface2),
            VerticalAlignment = VerticalAlignment.Center,
        };
        Grid.SetColumn(chip, 2);
        head.Children.Add(chip);
        col.Children.Add(head);
        col.Children.Add(Hint(tool.Purpose));

        EnhanceToolKind kind = tool.Kind;
        Button action = AccentButton(" ", () => RunEnhanceTool(kind));
        action.Padding = Pad(12, 5, 12, 5);
        action.HorizontalAlignment = HorizontalAlignment.Left;
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(6) };
        actions.Children.Add(action);
        if (kind == EnhanceToolKind.Remove)
        {
            _enhanceUndoStroke = EditButton("Undo stroke", () => Send(Command.AdjustBrush, 0));
            _enhanceClearStrokes = EditButton("Clear", () => Send(Command.AdjustBrush, 1));
            actions.Children.Add(_enhanceUndoStroke);
            actions.Children.Add(_enhanceClearStrokes);
        }
        col.Children.Add(actions);
        if (kind == EnhanceToolKind.Faces)
        {
            // S10: Enhance's face model under Choose faces: its Look, Strength, the
            // time a face takes and its one button (the add-on's card stack below).
            _enhanceRestoreFacesHead = Text("Restore faces", Body, UiFontSize - 3, bold: true, maxLines: 1);
            _enhanceRestoreFacesHead.Margin = Pad(0, 4, 0, 0);
            ToolTipService.SetToolTip(_enhanceRestoreFacesHead, RestoreFacesTool.Purpose);
            col.Children.Add(_enhanceRestoreFacesHead);
        }
        if (kind == EnhanceToolKind.Extend && _editEnhance)
            col.Children.Add(Hint("Drag the frame's edges past the photo, then Fill (Enter)."));
        if (kind == EnhanceToolKind.Remove)
        {
            _enhanceBrushNote = Hint("Brush full: Remove, then brush more.");
            col.Children.Add(_enhanceBrushNote);
        }
        if (EnhanceDockWordOf(kind) is not null) col.Children.Add(BuildEnhanceDockHost(kind));
        // One surface, no border; the active card lifts (RenderEnhanceInPlace).
        var card = new Border
        {
            Child = col,
            Padding = Pad(12),
            CornerRadius = Corner(10),
            Background = Brush(ChromeColour.Surface2),
        };
        // No hover wash on a card with choices on it: it is a page, not a button.
        card.PointerEntered += (_, _) =>
        {
            if (!EnhanceDockedOn(kind) && !EnhanceCardLifted(kind)) card.Background = Brush(ChromeColour.HoverWash);
        };
        card.PointerExited += (_, _) => card.Background = EnhanceCardLifted(kind) ? EnhanceLiftBrush() : Brush(ChromeColour.Surface2);
        AutomationProperties_SetName(card, tool.Title);
        EnhanceCards.Add((kind, chipText, chip, action, card));
        return card;
    }

    // The chips, the buttons' words and enabled states, the armed outline.
    private static void RenderEnhanceInPlace()
    {
        foreach ((EnhanceToolKind kind, TextBlock chip, Border pill, Button action, Border card) in EnhanceCards)
        {
            EnhanceToolState st = EnhanceStateOf(kind);
            chip.Text = st.Chip;
            // Only when something is needed (Enhance, People) or under way: "Ready" is no chip.
            pill.Visibility = !st.Ready || st.Busy ? Visibility.Visible : Visibility.Collapsed;
            if (action.Content is TextBlock t) t.Text = st.Action;
            AutomationProperties_SetName(action, st.Action);
            action.IsEnabled = st.Enabled;
            // One-click: Restyle, Enhance image (and the Faces card's Restore faces)
            // run from the add-on's own button inside the card; the card's own
            // button is the brush's, the frame's, the Faces tool's, or Get / Update Enhance.
            bool cardsTool = kind is EnhanceToolKind.Restyle or EnhanceToolKind.Detail;
            action.Visibility = cardsTool && !st.NeedsAddon ? Visibility.Collapsed : Visibility.Visible;
            // The active card lifts to a lighter step of the surface; a ring is
            // keyboard focus's (docs/design/20 "Craft rules").
            card.Background = EnhanceCardLifted(kind) ? EnhanceLiftBrush() : Brush(ChromeColour.Surface2);
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetItemStatus(card, st.Chip);
        }
        bool brushOn = (_editWand & WandBrushOn) != 0, brushed = (_editWand & WandBrushStrokes) != 0;
        if (_enhanceUndoStroke is not null)
        {
            _enhanceUndoStroke.Visibility = brushOn ? Visibility.Visible : Visibility.Collapsed;
            _enhanceUndoStroke.IsEnabled = brushed;
        }
        if (_enhanceClearStrokes is not null)
        {
            _enhanceClearStrokes.Visibility = brushOn ? Visibility.Visible : Visibility.Collapsed;
            _enhanceClearStrokes.IsEnabled = brushed;
        }
        if (_enhanceRestoreFacesHead is not null)
        {
            // Shown while the cards have Restore faces (its stack follows it).
            _enhanceRestoreFacesHead.Visibility = _editEnhance && EnhanceDockedOn(EnhanceToolKind.Faces)
                ? Visibility.Visible : Visibility.Collapsed;
        }
        if (_enhanceBrushNote is not null)
            _enhanceBrushNote.Visibility = (_editWand & WandBrushFull) != 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    // Brushing, choosing faces, or running: the card the work is on.
    private static bool EnhanceCardLifted(EnhanceToolKind kind) =>
        (kind == EnhanceToolKind.Remove && (_editWand & WandBrushOn) != 0) ||
        (kind == EnhanceToolKind.Faces && _facesArmed) ||
        (EnhanceRunWorking && EnhanceDockWordOf(kind) == _enhanceRunTool);

    // The Mac's MVTheme.raised: the title colour at 0.12, a solid step.
    private static SolidColorBrush EnhanceLiftBrush()
    {
        Windows.UI.Color t = Brush(Title).Color;
        return new SolidColorBrush(Windows.UI.Color.FromArgb(31, t.R, t.G, t.B));
    }

    // ---- the performance level's chip (docs/plans/edit-refresh.md S9, §3.3) ---------
    // "Default level" in the tab's header: the level Enhance runs at, as its
    // settings page names it (the "tierlevel" node). That page's screen is open
    // while it shows in Settings; this tab opens it too while it shows and keeps
    // the last text read between (the level changes only in Settings, which the
    // list carries here). A click opens Settings → Add-ons → Enhance, whose
    // first card is Performance. The Mac twin is EnhanceLevel in EnhanceHubView.swift.

    private const string EnhanceAddonId = "enhance";
    private static string _enhanceLevel = "";
    private static Button? _enhanceLevelChip;
    private static TextBlock? _enhanceLevelText;
    private static bool _enhanceLevelOpen;  // this tab opened Enhance's settings screen

    private static Button BuildEnhanceLevelChip()
    {
        _enhanceLevelText = Text(_enhanceLevel, Body, UiFontSize - 5, maxLines: 1);
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(4) };
        row.Children.Add(new FontIcon
        {
            Glyph = "\uEC4A", FontFamily = IconFont, FontSize = TypeSize(11),  // Speed high
            Foreground = Brush(ChromeColour.Accent), VerticalAlignment = VerticalAlignment.Center,
        });
        row.Children.Add(_enhanceLevelText);
        var pill = new Border
        {
            Child = row,
            Padding = Pad(7, 2, 7, 2),
            CornerRadius = new CornerRadius(999),
            Background = Brush(ChromeColour.Surface2),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(0.5),
        };
        _enhanceLevelChip = ChromeButton(pill, Pad(0), OpenEnhanceSettings,
            tip: "Enhance's performance level. Change it in Settings → Add-ons → Enhance.");
        RenderEnhanceLevel();
        return _enhanceLevelChip;
    }

    private static void RenderEnhanceLevel()
    {
        if (_enhanceLevelChip is null || _enhanceLevelText is null) return;
        _enhanceLevelText.Text = _enhanceLevel;
        _enhanceLevelChip.Visibility = _enhanceLevel.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        AutomationProperties_SetName(_enhanceLevelChip, $"Performance level: {_enhanceLevel}. Opens Enhance's settings");
    }

    private static ScreenAt? EnhanceSettingsAt() =>
        SlotSettingsScreens.TryGetValue(EnhanceAddonId, out string? id) ? new ScreenAt(EnhanceAddonId, id) : null;

    private static void OpenEnhanceLevel()
    {
        if (EnhanceSettingsAt() is not { } at) return;
        // A no-op in the core while it is open (Settings showing it, or a rebuild).
        SlotNative.OpenScreen(at.Addon, at.Screen, "", SlotTheme(), 0, 0, (float)SlotScale());
        _enhanceLevelOpen = true;
    }

    // The tab's UI dropped: closed a hop later unless the tab was built again
    // (a rebuild drops and builds in one go), or Settings is showing it.
    private static void ReleaseEnhanceLevel()
    {
        if (!_enhanceLevelOpen) return;
        DispatcherQueueControllerTryEnqueue(() =>
        {
            if (!_enhanceLevelOpen || _enhanceLevelChip is not null) return;
            _enhanceLevelOpen = false;
            if (EnhanceSettingsAt() is { } at && _settingsScreenAt != at) SlotNative.CloseScreen(at.Addon, at.Screen);
        });
    }

    // SyncScreenWindows: the level Enhance's settings screen names, kept.
    private static void NoteEnhanceLevel(JsonElement list)
    {
        if (EnhanceSettingsAt() is not { } at) return;
        foreach (JsonElement s in list.EnumerateArray())
        {
            if (JStr(s, "addon") != at.Addon || JStr(s, "id") != at.Screen) continue;
            if (s.TryGetProperty("tree", out JsonElement tree) && FindTreeNode(tree, "tierlevel") is { } n)
            {
                string text = JStr(n, "text");
                if (text.Length > 0 && text != _enhanceLevel)
                {
                    _enhanceLevel = text;
                    RenderEnhanceLevel();
                }
            }
            return;
        }
    }

    private static JsonElement? FindTreeNode(JsonElement node, string id)
    {
        if (node.ValueKind != JsonValueKind.Object) return null;
        if (JStr(node, "id") == id) return node;
        foreach (JsonElement child in JArr(node, "children"))
        {
            if (FindTreeNode(child, id) is { } hit) return hit;
        }
        return null;
    }
}
