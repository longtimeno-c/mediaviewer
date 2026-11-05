// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Media;

namespace MediaViewer.Chrome;

/// <summary>
/// docs/plans/edit-refresh.md S2: the Edit workspace's 44 DIP icon rail, its own
/// island at the window's right edge while the workspace is open (native
/// places it, layout_panels). Top: the tabs (Crop, Adjust, Enhance, Info; Trim
/// and Jobs on a clip). Then a hairline and Red-eye, the one canvas tool with
/// no card (2026-11-05, docs/design/20 "Craft rules": Remove, Faces and Extend
/// are the Enhance tab's cards). At the foot: Undo and Compare while the panel
/// is folded (open, the header has them), and Done. The Mac twin is EditRailView.swift.
/// </summary>
/// <remarks>
/// A tab icon sends <c>chrome_cmd_edit_tab</c>, which native reads as a rail
/// click (shell/edit_workspace.h rail_pick): another tab selects it, the active
/// one folds the inspector to the rail, a folded rail unfolds. The icons never
/// take focus on a click, so the canvas keeps its keys. Also here: Notify,
/// native's one-number notifications, of which the column edge's state lights
/// the left border of the pane on top (the Mac's MvPaneEdgeView).
/// </remarks>
public static partial class IslandHost
{
    private static DesktopWindowXamlSource? _editRail;
    private static bool _editRailVisible;
    private static string _railShape = "";
    private static int _paneEdgeState;

    // Segoe Fluent Icons / MDL2 Assets.
    private static class RailGlyph
    {
        public const string Crop = "";
        public const string Adjust = "";    // Equalizer
        public const string Enhance = "";   // Processing (the strip's Enhance icon before)
        public const string Info = "";
        public const string Trim = "";
        public const string Jobs = "";      // BulletedList
        public const string Remove = "";    // EraseTool
        public const string RedEye = "";
        public const string Faces = "";     // Contact
        public const string Extend = "";    // FullScreen: arrows out to the corners
        public const string Undo = "";
        public const string Compare = "";   // DockLeft: a split frame
        public const string Done = "";      // Accept
    }

    private sealed record RailItem(Button Button, Border Bar);
    private static readonly List<(RailItem Item, int Tab)> RailTabs = new();
    private static RailItem? _railCompare;
    private static Button? _railUndo;

    private static void DropRailUi()
    {
        _railShape = "";
        RailTabs.Clear();
        _railCompare = null;
        _railUndo = null;
    }

    public static int ShowEditRail(IntPtr arg, int sizeBytes) => ShowPanel(
        arg, sizeBytes, _editRail, BuildEditRail,
        onShown: () => _editRailVisible = true,
        onHidden: () =>
        {
            _editRailVisible = false;
            DropRailUi();
        });

    // The rail's icons differ between a still and a clip; everything else
    // (selection, enabled, the brush) changes in place.
    private static string RailShape() => $"{_editSubject}|{_editEnhance}";

    private static void RenderRail()
    {
        if (_editRail is null || !_editRailVisible) return;
        if (RailShape() != _railShape)
        {
            _editRail.Content = BuildEditRail();
            return;
        }
        RenderRailInPlace();
    }

    private static UIElement BuildEditRail()
    {
        DropRailUi();
        _railShape = RailShape();
        var root = new Grid
        {
            Background = Brush(PanelBg),
            RequestedTheme = IslandTheme,
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(1, 0, 0, 0),
            Padding = Pad(0, 8, 0, 8),
        };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        var top = new StackPanel { Spacing = Space(2), HorizontalAlignment = HorizontalAlignment.Center };
        (int Tab, string Glyph, string Label, int KeyedBy)[] tabs = EditIsClip
            ? new[] { (TabTrim, RailGlyph.Trim, "Trim", Command.TrimMode), (TabJobs, RailGlyph.Jobs, "Jobs", Command.JobsPane) }
            : new[]
            {
                (TabCrop, RailGlyph.Crop, "Crop", Command.CropMode),
                (TabColour, RailGlyph.Adjust, "Adjust", Command.AdjustPane),
                (TabEnhance, RailGlyph.Enhance, "Enhance", -1),
                (TabInfo, RailGlyph.Info, "Info", Command.MetadataPane),
            };
        foreach ((int tab, string glyph, string label, int keyedBy) in tabs)
        {
            int t = tab;
            string tip = keyedBy >= 0 ? WithKey(label, keyedBy) : label;
            RailItem item = RailButton(glyph, label, tip, () => Send(Command.EditTab, t));
            RailTabs.Add((item, tab));
            top.Children.Add(Holder(item));
        }
        if (!EditIsClip)
        {
            top.Children.Add(new Border
            {
                Height = 1, Width = 24, Background = Brush(Hairline), Margin = Pad(0, 6, 0, 6),
                HorizontalAlignment = HorizontalAlignment.Center,
            });
            top.Children.Add(Holder(RailButton(RailGlyph.RedEye, "Red-eye", "Red-eye: click each red pupil on the photo",
                () => Send(Command.EditAction, EditActions.ArmRedEye))));
        }
        root.Children.Add(top);

        var foot = new StackPanel { Spacing = Space(2), HorizontalAlignment = HorizontalAlignment.Center };
        if (!EditIsClip)
        {
            RailItem undo = RailButton(RailGlyph.Undo, "Undo edit", WithKey("Undo the last edit", Command.UndoEdit),
                                       () => Send(Command.UndoEdit));
            _railUndo = undo.Button;
            foot.Children.Add(Holder(undo));
            // docs/design/20 "Compare": the before/after split (Y held: the before alone).
            _railCompare = RailButton(RailGlyph.Compare, "Compare before and after", CompareTipWithKey,
                () => Send(Command.EditAction, _editOriginal ? EditActions.OriginalOff : EditActions.OriginalOn));
            if (Holder(_railCompare) is Panel compareHolder) compareHolder.Background = Brush(Colors.Transparent);
            foot.Children.Add(Holder(_railCompare));
        }
        string? enter = KeyOf(Command.EditWorkspace);
        RailItem done = RailButton(RailGlyph.Done, "Done",
            enter is null ? "Done: close the editor  Esc" : $"Done: close the editor  {enter} or Esc",
            () => Send(Command.EditWorkspace));
        done.Button.Foreground = Brush(Title);
        foot.Children.Add(Holder(done));
        Grid.SetRow(foot, 2);
        root.Children.Add(foot);

        AutomationProperties_SetName(root, "Edit tools");
        WireFileDrop(root);
        RenderRailInPlace();
        ThemeField.Lay(root);
        return root;
    }

    private static UIElement Holder(RailItem item) => item.Bar.Parent as UIElement ?? item.Button;

    // One 32 DIP icon: a wash on hover (FlattenButton), the accent and a 2 DIP
    // bar on its leading edge while selected; the tooltip and the screen
    // reader carry the name and the live key.
    private static RailItem RailButton(string glyph, string name, string tip, Action click)
    {
        var icon = new FontIcon { Glyph = glyph, FontFamily = IconFont, FontSize = TypeSize(16) };
        var b = new Button
        {
            Content = icon,
            Width = 32,
            Height = 32,
            Padding = new Thickness(0),
            Foreground = Brush(Body),
            Background = Brush(Colors.Transparent),
            HorizontalAlignment = HorizontalAlignment.Center,
            HorizontalContentAlignment = HorizontalAlignment.Center,
            VerticalContentAlignment = VerticalAlignment.Center,
            AllowFocusOnInteraction = false,
        };
        FlattenButton(b);
        b.CornerRadius = Corner(7);
        ToolTipService.SetToolTip(b, tip);
        AutomationProperties_SetName(b, name);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetHelpText(b, tip);
        b.Click += (_, _) => Guarded(click);
        var bar = new Border
        {
            Width = 2.5,
            Height = 16,
            CornerRadius = new CornerRadius(1.25),
            Background = Brush(ChromeColour.Accent),
            HorizontalAlignment = HorizontalAlignment.Left,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(1, 0, 0, 0),
            Visibility = Visibility.Collapsed,
            IsHitTestVisible = false,
        };
        // The bar sits at the rail's own left edge, beside the icon.
        var holder = new Grid { Width = 44, Height = 34 };
        holder.Children.Add(b);
        holder.Children.Add(bar);
        return new RailItem(b, bar);
    }

    private static void SetRailSelected(RailItem item, bool selected, bool marked)
    {
        item.Button.Background = selected ? Brush(ChromeColour.SelectionWash) : Brush(Colors.Transparent);
        item.Button.Foreground = selected ? Brush(ChromeColour.Accent) : Brush(Body);
        item.Bar.Visibility = selected || marked ? Visibility.Visible : Visibility.Collapsed;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetItemStatus(item.Button, selected ? "Selected" : "");
    }

    private static void RenderRailInPlace()
    {
        foreach ((RailItem item, int tab) in RailTabs)
        {
            bool active = tab == _editTab;
            SetRailSelected(item, active && !_editCollapsed, active);
            string label = AutomationNameOf(item.Button);
            ToolTipService.SetToolTip(item.Button, _editCollapsed ? $"Show {label}"
                                      : active ? $"{label}: click again to hide the panel" : WithTabKey(label, tab));
        }
        if (_railCompare is not null)
        {
            SetRailSelected(_railCompare, _editOriginal, false);
            // A disabled Button shows no tooltip: its holder says why then.
            _railCompare.Button.IsEnabled = CanCompare;
            ToolTipService.SetToolTip(_railCompare.Button, CompareTipWithKey);
            ToolTipService.SetToolTip(Holder(_railCompare), CanCompare ? null : CompareTip);
            // Folded only: open, the header carries Compare (docs/design/20 "Craft rules").
            Holder(_railCompare).Visibility = _editCollapsed ? Visibility.Visible : Visibility.Collapsed;
        }
        if (_railUndo is not null)
        {
            _railUndo.IsEnabled = _editCount > 0;
            if (_railUndo.Parent is UIElement holder)
                holder.Visibility = _editCollapsed ? Visibility.Visible : Visibility.Collapsed;
        }
    }

    private static string AutomationNameOf(Button b) =>
        Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(b) ?? "";

    private static string WithTabKey(string label, int tab) => tab switch
    {
        TabCrop => WithKey(label, Command.CropMode),
        TabColour => WithKey(label, Command.AdjustPane),
        TabInfo => WithKey(label, Command.MetadataPane),
        TabTrim => WithKey(label, Command.TrimMode),
        TabJobs => WithKey(label, Command.JobsPane),
        _ => label,
    };

    /// <summary>
    /// docs/plans/edit-refresh.md S2: a notification from native with one number
    /// (chrome_host.h chrome_notify_args: int32 id, float value). Today the
    /// column edge's state (Command.PaneEdgeState: 0 idle, 1 hot, 2 drag).
    /// </summary>
    public static int Notify(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < 8) return unchecked((int)0x80070057);
            int id = Marshal.ReadInt32(arg, 0);
            float value = BitConverter.Int32BitsToSingle(Marshal.ReadInt32(arg, 4));
            if (id == Command.PaneEdgeState) ApplyPaneEdge((int)Math.Round(value));
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    // The pane on top of the column: its left border is the accent at 2 DIP
    // while the edge is hot or dragged, the hairline otherwise.
    private static void ApplyPaneEdge(int state)
    {
        _paneEdgeState = state;
        foreach (DesktopWindowXamlSource? source in new[] { _editPane, _adjustPane, _metaPane, _jobsPane })
            PaintPaneEdge(source?.Content as Grid);
    }

    private static void PaintPaneEdge(Grid? root)
    {
        if (root is null) return;
        bool lit = _paneEdgeState != 0;
        root.BorderBrush = lit ? Brush(ChromeColour.Accent) : Brush(Hairline);
        root.BorderThickness = new Thickness(lit ? 2 : 1, 0, 0, 0);
    }
}
