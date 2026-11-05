// SPDX-License-Identifier: GPL-3.0-or-later
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
/// choice (its "open" and "restyle" commands; Extend through the sheet's photo
/// choice) and Open closed eyes. Every result is a new file; the original
/// never changes. Running inside the pane is S5's. The Mac twin is EnhanceHubView.swift.
/// </summary>
/// <remarks>
/// Only Button, TextBlock, Border, Grid and StackPanel, and a plain Flyout of
/// buttons for the ✦ chevron (tools/check-winui-controls.ps1). The Windows
/// brush is the adjust pane's pick until S3/S4a: "Start brushing" shows Adjust
/// with the brush on.
/// </remarks>
public static partial class IslandHost
{
    private enum EnhanceToolKind { Remove, Restyle, Faces, Detail, Extend }

    private sealed record EnhanceToolInfo(EnhanceToolKind Kind, string Title, string Purpose, string Glyph);

    private static readonly EnhanceToolInfo[] EnhanceTools =
    {
        new(EnhanceToolKind.Remove, "Remove objects",
            "Brush over what to take out; it is filled in from around it.", RailGlyph.Remove),
        new(EnhanceToolKind.Restyle, "Restyle",
            "Describe a new look in words: a snowy day, golden hour.", ""),  // Color
        new(EnhanceToolKind.Faces, "Faces",
            "Open closed eyes from another photo of the same person.", RailGlyph.Faces),
        new(EnhanceToolKind.Detail, "Enhance image",
            "Sharpen and restore, remove noise or blur, or enlarge, with a model on this PC.", RailGlyph.Enhance),
        new(EnhanceToolKind.Extend, "Extend edges",
            "More picture around the edges, to a wider or taller frame.", RailGlyph.Extend),
    };

    // shell/adjust_pane.h kWand*: what chrome_edit_args.wand says.
    private const int WandBrushOn = 1, WandBrushStrokes = 2, WandBrushFull = 4, WandRestyle = 8, WandEyesBusy = 16,
                      WandExtend = 32, WandDetail = 64, WandFacesReady = 128, WandAutoOn = 256, WandAutoBusy = 512;

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
            EnhanceToolKind.Restyle => (_editWand & WandRestyle) != 0
                ? new("Ready", true, false, "Restyle…", true, false)
                : new("Not offered", false, false, "Restyle…", false, false),
            EnhanceToolKind.Faces when (_editWand & WandEyesBusy) != 0 =>
                new("Working…", false, true, "Open closed eyes", false, false),
            EnhanceToolKind.Faces => (_editWand & WandFacesReady) != 0
                ? new("Ready", true, false, "Open closed eyes", true, false)
                : new("Needs People", false, false, "Open closed eyes", true, false),
            EnhanceToolKind.Detail => new("Ready", true, false, "Enhance…", (_editWand & WandDetail) != 0, false),
            _ => new("Ready", true, false, "Extend…", (_editWand & WandExtend) != 0, false),
        };
    }

    private static void RunEnhanceTool(EnhanceToolKind kind)
    {
        bool brushOn = (_editWand & WandBrushOn) != 0, brushed = (_editWand & WandBrushStrokes) != 0;
        if (!_editEnhance && kind != EnhanceToolKind.Faces)
        {
            OpenEnhanceSettings();
            return;
        }
        switch (kind)
        {
            case EnhanceToolKind.Remove:
                if (brushOn && brushed) Send(Command.AdjustBrush, 2);   // Enhance's sheet takes what is brushed
                else if (brushOn) Send(Command.AdjustPick, 100f);      // the brush down (pick 0)
                else Send(Command.EditAction, EditActions.ArmBrush);
                break;
            case EnhanceToolKind.Restyle: Send(Command.EditAction, EditActions.Restyle); break;
            case EnhanceToolKind.Faces: Send(Command.AdjustOpenEyes); break;
            case EnhanceToolKind.Detail:
            case EnhanceToolKind.Extend: Send(Command.EditAction, EditActions.Enhance); break;
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

    private static readonly List<(EnhanceToolKind Kind, TextBlock Chip, Border ChipDot, Button Action, Border Card)>
        EnhanceCards = new();
    private static TextBlock? _enhanceBrushNote;
    private static Button? _enhanceUndoStroke;
    private static Button? _enhanceClearStrokes;

    private static void DropEnhanceUi()
    {
        EnhanceCards.Clear();
        _enhanceBrushNote = null;
        _enhanceUndoStroke = null;
        _enhanceClearStrokes = null;
    }

    private static void BuildEnhanceHub(StackPanel pane)
    {
        DropEnhanceUi();
        var head = new StackPanel { Spacing = Space(4) };
        TextBlock title = Text("Enhance", Title, UiFontSize + 1);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetHeadingLevel(
            title, Microsoft.UI.Xaml.Automation.Peers.AutomationHeadingLevel.Level2);
        head.Children.Add(title);
        head.Children.Add(Hint("Each tool writes a new file; the original is never changed."));
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
        // Two columns from 560 DIP (ApplyEditWidthClass reparents nothing here:
        // the hub is rebuilt with the tab, and its columns are its own).
        var grid = new Grid { ColumnSpacing = Space(10), RowSpacing = Space(10) };
        _enhanceGrid = grid;
        foreach (EnhanceToolInfo tool in EnhanceTools) grid.Children.Add(BuildEnhanceCard(tool));
        LayEnhanceGrid();
        pane.Children.Add(grid);
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
        var head = new Grid { ColumnSpacing = Space(10) };
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.Children.Add(new Border
        {
            Width = 30,
            Height = 30,
            CornerRadius = Corner(8),
            Background = Brush(ChromeColour.SelectionWash),
            Child = new FontIcon
            {
                Glyph = tool.Glyph, FontFamily = IconFont, FontSize = TypeSize(15),
                Foreground = Brush(ChromeColour.Accent),
            },
        });
        TextBlock name = Text(tool.Title, Title, UiFontSize - 1, maxLines: 1);
        name.TextTrimming = TextTrimming.CharacterEllipsis;
        name.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(name, 1);
        head.Children.Add(name);
        var dot = new Border { Width = 6, Height = 6, CornerRadius = new CornerRadius(3), VerticalAlignment = VerticalAlignment.Center };
        TextBlock chipText = Text("", Body, UiFontSize - 5, maxLines: 1);
        var chipRow = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(4) };
        chipRow.Children.Add(dot);
        chipRow.Children.Add(chipText);
        var chip = new Border
        {
            Child = chipRow,
            Padding = Pad(7, 2, 7, 2),
            CornerRadius = new CornerRadius(999),
            Background = Brush(ChromeColour.Surface2),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(0.5),
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
        if (kind == EnhanceToolKind.Extend && _editEnhance)
            col.Children.Add(Hint("In the sheet, choose Extend the picture and a frame."));
        if (kind == EnhanceToolKind.Remove)
        {
            _enhanceBrushNote = Hint("Brush full: Remove, then brush more.");
            col.Children.Add(_enhanceBrushNote);
        }
        var card = new Border
        {
            Child = col,
            Padding = Pad(12),
            CornerRadius = Corner(10),
            Background = Brush(ChromeColour.Surface2),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(0.5),
        };
        card.PointerEntered += (_, _) => card.Background = Brush(ChromeColour.HoverWash);
        card.PointerExited += (_, _) => card.Background = Brush(ChromeColour.Surface2);
        AutomationProperties_SetName(card, tool.Title);
        EnhanceCards.Add((kind, chipText, dot, action, card));
        return card;
    }

    // The chips, the buttons' words and enabled states, the armed outline.
    private static void RenderEnhanceInPlace()
    {
        foreach ((EnhanceToolKind kind, TextBlock chip, Border dot, Button action, Border card) in EnhanceCards)
        {
            EnhanceToolState st = EnhanceStateOf(kind);
            chip.Text = st.Chip;
            chip.Foreground = st.Ready ? Brush(Title) : Brush(Body);
            dot.Background = st.Ready ? new SolidColorBrush(ColorHelper.FromArgb(255, 70, 190, 90)) : Brush(Body);
            if (action.Content is TextBlock t) t.Text = st.Action;
            AutomationProperties_SetName(action, st.Action);
            action.IsEnabled = st.Enabled;
            bool armed = kind == EnhanceToolKind.Remove && (_editWand & WandBrushOn) != 0;
            card.BorderBrush = armed ? Brush(ChromeColour.Accent) : Brush(Hairline);
            card.BorderThickness = new Thickness(armed ? 1.5 : 0.5);
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
        if (_enhanceBrushNote is not null)
            _enhanceBrushNote.Visibility = (_editWand & WandBrushFull) != 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    // ---- the header's ✦ chevron: the five tools in a flyout ----------------------
    // This closes the Windows wand twin (the Mac's popover). A plain Flyout of
    // buttons, built fresh per open as the add-on alert's is.

    private static Flyout? _enhanceFlyout;

    private static void OpenEnhanceFlyout(FrameworkElement anchor)
    {
        try
        {
            _enhanceFlyout?.Hide();
            var rows = new StackPanel { Spacing = Space(2), Width = 300 };
            rows.Children.Add(Hint("Enhance tools"));
            foreach (EnhanceToolInfo tool in EnhanceTools)
            {
                EnhanceToolState st = EnhanceStateOf(tool.Kind);
                var line = new Grid { ColumnSpacing = Space(10) };
                line.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                line.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                line.Children.Add(new Border
                {
                    Width = 26, Height = 26, CornerRadius = Corner(6),
                    Background = Brush(ChromeColour.SelectionWash),
                    Child = new FontIcon
                    {
                        Glyph = tool.Glyph, FontFamily = IconFont, FontSize = TypeSize(14),
                        Foreground = Brush(ChromeColour.Accent),
                    },
                });
                var words = new StackPanel();
                words.Children.Add(Text(tool.Title, Title, UiFontSize - 2, maxLines: 1));
                words.Children.Add(Text(st.Ready ? st.Action : st.Chip, Body, UiFontSize - 5, maxLines: 1));
                Grid.SetColumn(words, 1);
                line.Children.Add(words);
                EnhanceToolKind kind = tool.Kind;
                Button row = ChromeButton(line, Pad(6, 4, 6, 4), () =>
                {
                    _enhanceFlyout?.Hide();
                    RunEnhanceTool(kind);
                }, align: HorizontalAlignment.Stretch, contentAlign: HorizontalAlignment.Left,
                    name: tool.Title, tip: tool.Purpose, enabled: st.Enabled);
                rows.Children.Add(row);
            }
            rows.Children.Add(new Border { Height = 1, Background = Brush(Hairline), Margin = Pad(0, 4, 0, 4) });
            rows.Children.Add(ChromeButton(ButtonLabel("Show the Enhance tab", UiFontSize - 2), CompactButtonPadding, () =>
            {
                _enhanceFlyout?.Hide();
                Send(Command.EditAction, EditActions.ShowEnhance);
            }, align: HorizontalAlignment.Stretch, contentAlign: HorizontalAlignment.Left));
            _enhanceFlyout = new Flyout
            {
                ShouldConstrainToRootBounds = false,
                FlyoutPresenterStyle = FlyoutPresenterStyle(),
                Content = rows,
                Placement = FlyoutPlacementMode.BottomEdgeAlignedRight,
            };
            _enhanceFlyout.Closed += (_, _) => RestoreCanvasFocus();
            _enhanceFlyout.ShowAt(anchor);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
        }
    }
}
