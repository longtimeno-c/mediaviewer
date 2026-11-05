// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Media;

namespace MediaViewer.Chrome;

/// <summary>
/// PR 29 (docs/design/20): the Edit workspace — one visible door to the PR 10–14 edits.
/// A fifth panel island at the top of the right column: the header (name, edit
/// count, ✦ Auto with the tools flyout; Undo / Reset / Compare / Save copy) and,
/// under it, the Crop, Trim or Enhance pane. The Adjust, Info and Jobs tabs are
/// the adjust, metadata and Jobs panes, which native places under the header.
/// docs/plans/edit-refresh.md S2: the tabs and Done moved to the icon rail
/// (IslandHost.Rail.cs); the header is as tall as its content and says so
/// (chrome_cmd_edit_strip_height). The Mac twin is EditView.swift.
/// </summary>
/// <remarks>
/// Every control sends a command the keyboard already has, and its tooltip and
/// accessible name say which key, so the pane teaches the keys rather than
/// replacing them. Native owns the workspace (shell/edit_workspace.h) and the
/// crop draft (shell/edit_session.h) and pushes what to show (SetEditView,
/// <c>chrome_edit_args</c>); nothing here edits pixels or reads a file. The
/// buttons never take focus on a click, so the crop and trim keys stay with
/// the canvas.
/// </remarks>
public static partial class IslandHost
{
    // chrome_host.h chrome_edit_args: 12 int/float words, two pointers, two lengths,
    // then the document's app list (pointer, length) and its flag, Enhance, the
    // wand bits, and (S2) the header's chips and the folded inspector.
    internal const int EditArgsSize = 112;

    private static DesktopWindowXamlSource? _editPane;
    private static bool _editPaneVisible;

    // Mirrors chrome_edit_action (chrome_host.h).
    private static class EditActions
    {
        public const int CancelCrop = 0;
        public const int OriginalOff = 1;
        public const int OriginalOn = 2;
        public const int SaveCopy = 3;
        public const int Enhance = 4;  // slice 5: Enhance's sheet, the copy carrying the edit
        public const int Restyle = 5;  // the sheet started on Restyle with words
        // docs/plans/edit-refresh.md S2: the Remove brush on (Adjust shows its
        // Remove section), the red-eye picker on (Adjust), the Enhance tab shown.
        public const int ArmBrush = 6;
        public const int ArmRedEye = 7;
        public const int ShowEnhance = 8;
        // docs/plans/edit-refresh.md S5: Enhance's docked panel on a tool, and its
        // result row (the new file parked in _treePending, as OpenPath's is).
        public const int EnhanceRemove = 9;
        public const int EnhanceRestyle = 10;
        public const int EnhanceDetail = 11;
        public const int EnhanceExtend = 12;
        public const int EnhanceResultShow = 13;
        public const int EnhanceResultKeep = 14;
        // docs/plans/edit-refresh.md S10: Restore faces (Enhance's docked panel on
        // its "faces" tool, else its sheet started on Restore faces).
        public const int RestoreFaces = 15;
    }

    // shell::edit_tab and shell::crop_aspect values (both stable on the wire).
    private const int TabCrop = 0, TabColour = 1, TabInfo = 2, TabTrim = 3, TabJobs = 4, TabEnhance = 5;
    // chrome_host.h kEditChip*.
    private const int ChipLibrary = 1, ChipCloud = 2;
    private const int SubjectNone = 0, SubjectClip = 2;
    private static readonly string[] AspectLabels = { "Free", "Original", "1:1", "4:3", "3:2", "16:9", "5:4" };

    // chrome_host.h kEditTrim*.
    private const int TrimArmedFlag = 1, TrimPreviewingFlag = 2, TrimHasMarkerFlag = 4;

    // Last view native pushed.
    private static bool _editOpen;
    private static int _editTab;
    private static int _editSubject;
    private static bool _editCropActive;
    private static int _editAspect;
    private static bool _editPortrait;
    private static float _editStraighten;
    private static int _editCount;
    private static bool _editOriginal;
    // docs/design/20 "Compare" (shell::compare_kind): 0 no edits, 1 the edits,
    // 2 an Enhance result and its photo, 3 that photo is not beside it, 4 it is
    // being looked for. Compare is enabled on 1 and 2.
    private static int _editCompare;
    private static bool CanCompare => _editCompare == 1 || _editCompare == 2;
    // Compare's tooltip: what it compares, or why it is off.
    private static string CompareTip => _editCompare switch
    {
        1 => "Compare before and after your edits: drag the line on the photo",
        2 => "Compare with the photo this was made from: drag the line on the photo",
        3 => "Nothing to compare: the original photo is not beside this one",
        4 => "Looking for the original photo…",
        _ => "No edits yet to compare",
    };
    private static string CompareTipWithKey
    {
        get
        {
            if (!CanCompare) return CompareTip;
            string? hold = KeyOf(Command.ShowOriginal);
            return hold is null ? CompareTip : $"{CompareTip}  (or hold {hold})";
        }
    }
    private static int _editCropW;
    private static int _editCropH;
    private static int _editTrimFlags;
    private static string _editName = "";
    private static string _editTrimLabel = "";
    private static int _editWand;       // shell/adjust_pane.h kWand*
    private static int _editChips;      // kEditChip*
    private static bool _editCollapsed; // the inspector folded to the rail

    // What the island was built for; anything else is updated in place, so a
    // straighten drag is never interrupted by a rebuild.
    private static string _editShape = "";
    private static bool _updatingEdit;

    private static Button? _editBarButton;
    // A PDF or DOCX (docs/design/20): "Open in <app>" and a ▾ of every app,
    // in place of the Edit button. Names come from native, default first.
    private static bool _editDocument;
    private static bool _editEnhance;  // slice 5: the Enhance add-on is loaded
    private static string[] _openApps = Array.Empty<string>();
    private static StackPanel? _openInBar;
    private static Button? _openInButton;
    private static Button? _openInMore;
    private static MenuFlyout? _openInFlyout;
    private static TextBlock? _editNameText;
    private static TextBlock? _editCountText;
    private static Border? _editCountPill;
    private static Border? _editChip;
    private static TextBlock? _editChipText;
    private static Button? _editUndo;
    private static Button? _editReset;
    private static Button? _editOriginalButton;
    // A disabled Button shows no tooltip: its holder carries it then.
    private static Border? _editCompareHolder;
    // "Before | After" under the header while the split is up (owner call A:
    // only the divider lies over the photo).
    private static Border? _editCompareLabels;
    private static TextBlock? _editCompareBefore;
    private static TextBlock? _editCompareAfter;
    private static Border? _editAuto;
    private static Button? _editAutoMain;
    private static FontIcon? _editAutoIcon;
    private static TextBlock? _editAutoLabel;
    private static Spinner? _editAutoSpinner;
    private static Button? _editAutoMore;
    private static Button? _editSave;
    // Labels hidden below 340 DIP (the icons stay): ApplyEditWidthClass.
    private static readonly List<UIElement> EditCompactHidden = new();
    private static Grid? _editStraightenTurn;
    private static Grid? _editAspectGrid;
    private static TextBlock? _editCropSize;
    private static Button? _editOrient;
    private static Slider? _editStraightenSlider;
    private static TextBlock? _editStraightenText;
    private static TextBlock? _editTrimLabelText;
    private static readonly List<Button> EditTrimSaveButtons = new();
    private static readonly List<Button> EditAspectButtons = new();

    private static void DropEditUi()
    {
        _editShape = "";
        _editNameText = null;
        _editCountText = null;
        _editCountPill = null;
        _editChip = null;
        _editChipText = null;
        _editUndo = null;
        _editReset = null;
        _editOriginalButton = null;
        _editCompareHolder = null;
        _editCompareLabels = null;
        _editCompareBefore = null;
        _editCompareAfter = null;
        _editAuto = null;
        _editAutoMain = null;
        _editAutoIcon = null;
        _editAutoLabel = null;
        _editAutoSpinner = null;
        _editAutoMore = null;
        _editSave = null;
        EditCompactHidden.Clear();
        _editStraightenTurn = null;
        _editAspectGrid = null;
        _enhanceGrid = null;
        DropEnhanceUi();
        _editCropSize = null;
        _editOrient = null;
        _editStraightenSlider = null;
        _editStraightenText = null;
        _editTrimLabelText = null;
        EditTrimSaveButtons.Clear();
        EditAspectButtons.Clear();
    }

    public static int ShowEditPane(IntPtr arg, int sizeBytes) => ShowPanel(
        arg, sizeBytes, _editPane, BuildEditPane,
        onShown: () => _editPaneVisible = true,
        onHidden: () =>
        {
            _editPaneVisible = false;
            DropEditUi();
        });

    /// <summary>
    /// Native pushes the workspace, the crop draft and the trim state whenever
    /// any of them changes, open or not (the bar button follows the item).
    /// In: chrome_edit_args (EditArgsSize bytes; pointers valid for the call).
    /// </summary>
    public static int SetEditView(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < EditArgsSize) return unchecked((int)0x80070057);
            _editOpen = Marshal.ReadInt32(arg, 0) != 0;
            _editTab = Marshal.ReadInt32(arg, 4);
            _editSubject = Marshal.ReadInt32(arg, 8);
            _editCropActive = Marshal.ReadInt32(arg, 12) != 0;
            _editAspect = Math.Clamp(Marshal.ReadInt32(arg, 16), 0, AspectLabels.Length - 1);
            _editPortrait = Marshal.ReadInt32(arg, 20) != 0;
            _editStraighten = BitConverter.Int32BitsToSingle(Marshal.ReadInt32(arg, 24));
            _editCount = Marshal.ReadInt32(arg, 28);
            _editOriginal = Marshal.ReadInt32(arg, 32) != 0;
            _editCropW = Marshal.ReadInt32(arg, 36);
            _editCropH = Marshal.ReadInt32(arg, 40);
            _editTrimFlags = Marshal.ReadInt32(arg, 44);
            static string Utf8(long ptr, int len) =>
                ptr == 0 || len <= 0 ? "" : Marshal.PtrToStringUTF8(checked((IntPtr)ptr), len) ?? "";
            _editName = Utf8(Marshal.ReadInt64(arg, 48), Marshal.ReadInt32(arg, 64));
            _editTrimLabel = Utf8(Marshal.ReadInt64(arg, 56), Marshal.ReadInt32(arg, 68));
            _openApps = Utf8(Marshal.ReadInt64(arg, 72), Marshal.ReadInt32(arg, 80))
                .Split('\n', StringSplitOptions.RemoveEmptyEntries);
            _editDocument = Marshal.ReadInt32(arg, 84) != 0;
            _editEnhance = Marshal.ReadInt32(arg, 88) != 0;
            _editWand = Marshal.ReadInt32(arg, 92);
            _editChips = Marshal.ReadInt32(arg, 96);
            _editCollapsed = Marshal.ReadInt32(arg, 100) != 0;
            _editCompare = Marshal.ReadInt32(arg, 104);
            RenderEditBarButton();
            if (_editPaneVisible) RenderEdit();
            RenderRail();
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    private static bool EditIsClip => _editSubject == SubjectClip;
    private static string EditTitle => EditIsClip ? "Edit video" : "Edit image";

    // ---- the command bar's button ----------------------------------------------

    private static Button BuildEditBarButton()
    {
        // The bar's own TextButton, so it looks and behaves like Open / View
        // (see FlattenButton for why nothing here takes a custom template).
        _editBarButton = TextButton("Edit image", () => Send(Command.EditWorkspace));
        FlattenButton(_editBarButton);  // hidden with no media on the canvas
        RenderEditBarButton();
        return _editBarButton;
    }

    private static void RenderEditBarButton()
    {
        if (_editBarButton is null) return;
        string label = _editOpen ? "Done" : EditTitle;
        SetButtonText(_editBarButton, label);
        // Shown only over media: not on the empty start, not over the gallery.
        _editBarButton.Visibility = _editOpen || (_editSubject != SubjectNone && !_galleryVisible)
            ? Visibility.Visible : Visibility.Collapsed;
        _editBarButton.Background = _editOpen ? Brush(ChromeColour.PressedWash) : Brush(Colors.Transparent);
        string? enter = KeyOf(Command.EditWorkspace);
        string tip = _editOpen ? (enter is null ? "Close the editor  Esc" : $"Close the editor  {enter} or Esc")
                     : WithKey(EditIsClip ? "Edit video: trim, split, clip tools"
                                          : "Edit image: crop, rotate, colour, info", Command.EditWorkspace);
        ToolTipService.SetToolTip(_editBarButton, tip);
        AutomationProperties_SetName(_editBarButton, _editOpen ? "Done editing" : EditTitle);
        RenderOpenInBar();
    }

    // A document's "Open in <app>": the default app (Enter does the same), and a
    // ▾ listing every app Explorer's Open with would offer. Shown where the
    // Edit button would be, only over a document.
    private static StackPanel BuildOpenInBar()
    {
        _openInButton = TextButton("Open in…", () => Send(Command.OpenInApp, _openApps.Length > 0 ? 0 : -1));
        FlattenButton(_openInButton);
        _openInFlyout = new MenuFlyout
        {
            ShouldConstrainToRootBounds = false,
            MenuFlyoutPresenterStyle = MenuFlyoutPresenterStyle(),
        };
        _openInFlyout.Opening += (_, _) => RefreshOpenInMenu();
        Button? more = null;
        more = TextButton("▾", () =>
        {
            if (more is not null) FlyoutBase.ShowAttachedFlyout(more);
        });
        FlattenButton(more);
        AttachBarFlyout(more, _openInFlyout);
        ToolTipService.SetToolTip(more, "Open with another app");
        AutomationProperties_SetName(more, "Open with another app");
        _openInMore = more;
        _openInBar = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 0 };
        _openInBar.Children.Add(_openInButton);
        _openInBar.Children.Add(more);
        RenderOpenInBar();
        return _openInBar;
    }

    private static void RefreshOpenInMenu()
    {
        if (_openInFlyout is null) return;
        _openInFlyout.Items.Clear();
        for (int i = 0; i < _openApps.Length; i++)
        {
            int row = i;
            _openInFlyout.Items.Add(Item("Open in " + _openApps[i], null, () => Send(Command.OpenInApp, row)));
        }
    }

    private static void RenderOpenInBar()
    {
        if (_openInBar is null || _openInButton is null || _openInMore is null) return;
        _openInBar.Visibility = _editDocument && !_editOpen && !_galleryVisible
            ? Visibility.Visible : Visibility.Collapsed;
        string label = _openApps.Length > 0 ? "Open in " + _openApps[0] : "Open in…";
        SetButtonText(_openInButton, label);
        string app = _openApps.Length > 0 ? _openApps[0] : "its app";
        ToolTipService.SetToolTip(_openInButton, WithKey($"Open this document in {app}", Command.EditWorkspace));
        AutomationProperties_SetName(_openInButton, label);
        _openInMore.Visibility = _openApps.Length > 1 ? Visibility.Visible : Visibility.Collapsed;
    }

    // ---- buttons in the chrome's flat style --------------------------------------

    // No custom ControlTemplate here. In these islands a Button whose custom
    // template actually loads fail-fasts in Microsoft.UI.Xaml (0xC000027B) once
    // a bar flyout opens or closes afterwards (seen 2026-09-27; the bar's own
    // FlatButtonTemplate has never loaded, which is why its buttons never hit
    // it). WinUI's own Button template is restyled through its lightweight
    // resources instead: no border, no fill at rest or disabled (a disabled
    // button dims rather than turning grey, so it never reads as "selected"),
    // the Mac's FlatButtonStyle washes on hover and press (the appearance's
    // text colour, as docs/design/25 says), and `Background` as the selected wash.
    private static void FlattenButton(Button b)
    {
        SolidColorBrush clear = Brush(Colors.Transparent);
        foreach (string key in new[]
                 {
                     "ButtonBackgroundDisabled", "ButtonBorderBrush", "ButtonBorderBrushPointerOver",
                     "ButtonBorderBrushPressed", "ButtonBorderBrushDisabled",
                 })
        {
            b.Resources[key] = clear;
        }
        b.Resources["ButtonBackgroundPointerOver"] = Brush(ChromeColour.HoverWash);
        b.Resources["ButtonBackgroundPressed"] = Brush(ChromeColour.PressedWash);
        // Icon content inherits the presenter's foreground: keep it the
        // chrome's title colour in every state, not Windows' own text colour.
        foreach (string key in new[] { "ButtonForegroundPointerOver", "ButtonForegroundPressed" })
            b.Resources[key] = Brush(Title);
        b.Resources["ButtonForegroundDisabled"] = Brush(ChromeColour.Disabled);
        b.CornerRadius = Corner(RadiusButton);
        b.BorderThickness = new Thickness(0);
        // IsEnabledChanged is not raised for a button set disabled before it
        // is in the live tree, so Loaded applies the dim as well.
        void Dim() => b.Opacity = b.IsEnabled ? 1.0 : 0.4;
        Dim();
        b.IsEnabledChanged += (_, _) => Dim();
        b.Loaded += (_, _) => Dim();
    }

    private static void SetSelected(Button b, bool selected)
    {
        // FlatButtonStyle: selected wears the pressed wash.
        b.Background = selected ? Brush(ChromeColour.PressedWash) : Brush(Colors.Transparent);
    }

    private static TextBlock SectionTitle(string text)
    {
        TextBlock t = Text(text, Body, UiFontSize - 2);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetHeadingLevel(
            t, Microsoft.UI.Xaml.Automation.Peers.AutomationHeadingLevel.Level2);
        return t;
    }

    private static TextBlock Hint(string text) => Text(text, Body, UiFontSize - 4);

    private static StackPanel Row(params UIElement[] children)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(4) };
        foreach (UIElement c in children) row.Children.Add(c);
        return row;
    }

    // ---- the island --------------------------------------------------------------

    // The inspector's width class (shell/pane_width.h class_of), from the island's
    // own width: compact < 340, regular < 560, wide < 720, xwide.
    private const int WidthCompact = 0, WidthRegular = 1, WidthWide = 2, WidthXWide = 3;
    private static int _editWidthClass = WidthRegular;

    private static int WidthClassOf(double width) =>
        width < 340 ? WidthCompact : width < 560 ? WidthRegular : width < 720 ? WidthWide : WidthXWide;

    private static UIElement BuildEditPane()
    {
        DropEditUi();
        _editShape = EditShape();
        var root = new Grid
        {
            Background = Brush(PanelBg),
            RequestedTheme = IslandTheme,
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(1, 0, 0, 0),
        };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        FrameworkElement strip = BuildEditStrip();
        root.Children.Add(strip);
        var rule = new Border { Height = 1, Background = Brush(Hairline) };
        Grid.SetRow(rule, 1);
        root.Children.Add(rule);
        if (_editTab == TabCrop || _editTab == TabTrim || _editTab == TabEnhance)
        {
            var pane = new StackPanel { Spacing = Space(16), Padding = Pad(14, 12, 14, 14) };
            if (_editTab == TabTrim) BuildTrimPane(pane);
            else if (_editTab == TabEnhance) BuildEnhanceHub(pane);
            else BuildCropPane(pane);
            var scroll = new ScrollViewer { Content = pane, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
            Grid.SetRow(scroll, 2);
            root.Children.Add(scroll);
        }
        // docs/plans/edit-refresh.md S2: the header is as tall as its content (one
        // row or two, labels or icons, the type scale); native lays the panes
        // under it. Sent when it changes, never per frame.
        strip.SizeChanged += (_, e) =>
        {
            // Only the height matters (a drag of the column's edge changes the width every frame).
            if (Math.Abs(e.NewSize.Height - e.PreviousSize.Height) < 0.5) return;
            Send(Command.EditStripHeight, (float)Math.Ceiling(e.NewSize.Height + 1));
        };
        // The width class: labels or icons, one column or two. Nothing is rebuilt
        // for a width; the same controls change in place.
        root.SizeChanged += (_, e) => ApplyEditWidthClass(e.NewSize.Width);
        AutomationProperties_SetName(root, EditTitle);
        // Issue #224: the island covers the canvas's WM_DROPFILES.
        WireFileDrop(root);
        PaintPaneEdge(root);
        RenderEditInPlace();
        ThemeField.Lay(root);
        return root;
    }

    // What forces a rebuild: the tab, the subject, the crop draft starting or
    // ending, trim arming or previewing (their panes list different buttons).
    // The in/out label and whether a marker exists are updated in place, so
    // Set in / Set out keep keyboard focus on the button pressed (issue #222).
    // The width is not part of it (ApplyEditWidthClass).
    private static string EditShape() =>
        $"{_editTab}|{_editSubject}|{_editCropActive}|{_editTrimFlags & ~TrimHasMarkerFlag}|{_editEnhance}";

    private static void RenderEdit()
    {
        if (_editPane is null) return;
        if (EditShape() != _editShape)
        {
            _editPane.Content = BuildEditPane();
            return;
        }
        RenderEditInPlace();
    }

    // Below 340 DIP the header is icons only; at 560 and up the Crop pane's
    // ratios are one row and straighten sits beside rotate, and the Enhance
    // cards are two columns.
    private static void ApplyEditWidthClass(double width)
    {
        // Everything is built for the current class (BuildEditStrip, the Crop
        // pane and the hub lay themselves out with it), so a drag inside one
        // class costs nothing here.
        int cls = WidthClassOf(width);
        if (cls == _editWidthClass) return;
        _editWidthClass = cls;
        Visibility labels = cls == WidthCompact ? Visibility.Collapsed : Visibility.Visible;
        foreach (UIElement e in EditCompactHidden) e.Visibility = labels;
        LayAspectGrid();
        LayStraightenTurn();
        LayEnhanceGrid();
    }

    private static UIElement Capsule(UIElement child, ChromeColour fill)
    {
        return new Border
        {
            Child = child,
            Padding = Pad(6, 1, 6, 1),
            CornerRadius = new CornerRadius(999),
            Background = Brush(fill),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(0.5),
            VerticalAlignment = VerticalAlignment.Center,
        };
    }

    // A header action: an icon and its name (the name hidden below 340 DIP).
    private static Button HeaderButton(string glyph, string label, string tip, Action click)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(5) };
        row.Children.Add(new FontIcon { Glyph = glyph, FontFamily = IconFont, FontSize = TypeSize(13) });
        TextBlock text = ButtonLabel(label, UiFontSize - 3);
        text.VerticalAlignment = VerticalAlignment.Center;
        row.Children.Add(text);
        EditCompactHidden.Add(text);
        return ChromeButton(row, Pad(8, 5, 8, 5), click, align: HorizontalAlignment.Left, name: label, tip: tip);
    }

    private static FrameworkElement BuildEditStrip()
    {
        var col = new StackPanel { Spacing = Space(8), Padding = Pad(12, 10, 10, 10) };

        // Row 1: the name (truncated in the middle is not a WinUI trimming; the
        // end is), the edit count, a cloud chip, and ✦ Auto with its chevron.
        var head = new Grid { ColumnSpacing = Space(6) };
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _editNameText = Text("", Title, UiFontSize - 1, maxLines: 1);
        _editNameText.TextTrimming = TextTrimming.CharacterEllipsis;
        _editNameText.TextWrapping = TextWrapping.NoWrap;
        _editNameText.VerticalAlignment = VerticalAlignment.Center;
        head.Children.Add(_editNameText);
        _editCountText = Text("", Body, UiFontSize - 5, maxLines: 1);
        _editCountPill = (Border)Capsule(_editCountText, ChromeColour.Surface2);
        Grid.SetColumn(_editCountPill, 1);
        head.Children.Add(_editCountPill);
        _editChipText = Text("", Body, UiFontSize - 5, maxLines: 1);
        _editChip = (Border)Capsule(_editChipText, ChromeColour.Surface2);
        Grid.SetColumn(_editChip, 2);
        head.Children.Add(_editChip);
        if (!EditIsClip)
        {
            UIElement auto = BuildAutoButton();
            Grid.SetColumn((FrameworkElement)auto, 3);
            head.Children.Add(auto);
        }
        col.Children.Add(head);

        if (EditIsClip)
        {
            col.Children.Add(Row(EditButton("Clip tools…", () => Send(Command.ClipToolsFlyout),
                                            tip: WithKey("Rotate, split, remux, save a frame, audio, GIF",
                                                         Command.ClipToolsFlyout))));
        }
        else
        {
            // Row 2: Undo · Reset · Compare, then Save copy… filled with the accent.
            _editUndo = HeaderButton("\uE7A7", "Undo", WithKey("Undo the last edit", Command.UndoEdit),
                                     () => Send(Command.UndoEdit));
            AutomationProperties_SetName(_editUndo, "Undo edit");
            _editReset = HeaderButton("\uE72C", "Reset", WithKey("Reset to the original, exactly", Command.ResetEdits),
                                      () => Send(Command.ResetEdits));
            AutomationProperties_SetName(_editReset, "Reset to original");
            // docs/design/20 "Compare": the before/after split; Y held shows the before alone.
            _editOriginalButton = HeaderButton(RailGlyph.Compare, "Compare", CompareTipWithKey,
                () => Send(Command.EditAction, _editOriginal ? EditActions.OriginalOff : EditActions.OriginalOn));
            AutomationProperties_SetName(_editOriginalButton, "Compare before and after");
            _editCompareHolder = new Border { Background = Brush(Colors.Transparent), Child = _editOriginalButton };
            _editSave = AccentButton("Save copy…", () => Send(Command.EditAction, EditActions.SaveCopy));
            _editSave.Padding = Pad(12, 5, 12, 5);
            if (_editSave.Content is TextBlock saveText) saveText.FontSize = UiFontSize - 2;
            ToolTipService.SetToolTip(_editSave, WithKey("Write a new file with these edits; the original is never changed",
                                                         Command.ExportImage));
            AutomationProperties_SetName(_editSave, "Save copy");
            var actions = new Grid { ColumnSpacing = Space(4) };
            actions.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            actions.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var left = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(2) };
            left.Children.Add(_editUndo);
            left.Children.Add(_editReset);
            left.Children.Add(_editCompareHolder);
            actions.Children.Add(left);
            Grid.SetColumn(_editSave, 1);
            actions.Children.Add(_editSave);
            col.Children.Add(actions);
            col.Children.Add(BuildCompareLabels());
        }
        // Applied once built: the class may already be compact.
        Visibility labels = _editWidthClass == WidthCompact ? Visibility.Collapsed : Visibility.Visible;
        foreach (UIElement e in EditCompactHidden) e.Visibility = labels;
        return col;
    }

    // "Before | After": which side of the line on the photo is which.
    private static Border BuildCompareLabels()
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8) };
        row.Children.Add(new FontIcon
        {
            Glyph = RailGlyph.Compare, FontFamily = IconFont, FontSize = TypeSize(11),
            Foreground = Brush(ChromeColour.Accent), VerticalAlignment = VerticalAlignment.Center,
        });
        _editCompareBefore = Text("", Title, UiFontSize - 4, maxLines: 1);
        _editCompareBefore.VerticalAlignment = VerticalAlignment.Center;
        row.Children.Add(_editCompareBefore);
        row.Children.Add(new Border
        {
            Width = 2, Height = 14, Background = Brush(ChromeColour.Accent),
            VerticalAlignment = VerticalAlignment.Center,
        });
        _editCompareAfter = Text("", Title, UiFontSize - 4, maxLines: 1);
        _editCompareAfter.VerticalAlignment = VerticalAlignment.Center;
        row.Children.Add(_editCompareAfter);
        _editCompareLabels = new Border
        {
            Child = row,
            Padding = Pad(8, 4, 8, 4),
            CornerRadius = Corner(6),
            Background = Brush(ChromeColour.SelectionWash),
            Visibility = Visibility.Collapsed,
        };
        ToolTipService.SetToolTip(_editCompareLabels, "Drag the line on the photo to move between before and after");
        return _editCompareLabels;
    }

    // ✦ Auto (one undo step; filled while it is the last edit, a click then takes
    // it back) and its chevron, which opens the five tools (the Mac's popover).
    private static UIElement BuildAutoButton()
    {
        _editAutoIcon = new FontIcon { Glyph = RailGlyph.Enhance, FontFamily = IconFont, FontSize = TypeSize(12) };
        _editAutoSpinner = new Spinner(12);
        _editAutoSpinner.Root.Visibility = Visibility.Collapsed;
        _editAutoLabel = ButtonLabel("Auto", UiFontSize - 3);
        _editAutoLabel.VerticalAlignment = VerticalAlignment.Center;
        EditCompactHidden.Add(_editAutoLabel);
        var face = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(4) };
        face.Children.Add(_editAutoIcon);
        face.Children.Add(_editAutoSpinner.Root);
        face.Children.Add(_editAutoLabel);
        _editAutoMain = ChromeButton(face, Pad(9, 4, 6, 4), () => Send(Command.AutoEnhance), name: "Auto enhance");
        _editAutoMain.CornerRadius = new CornerRadius(999, 0, 0, 999);
        _editAutoMore = ChromeButton(new FontIcon { Glyph = "\uE70D", FontFamily = IconFont, FontSize = TypeSize(9) },
                                     Pad(6, 4, 8, 4), name: "Enhance tools",
                                     tip: "Enhance tools: Remove, Restyle, Faces, Enhance image, Extend");
        _editAutoMore.CornerRadius = new CornerRadius(0, 999, 999, 0);
        Button more = _editAutoMore;
        _editAutoMore.Click += (_, _) => Guarded(() => OpenEnhanceFlyout(more));
        var pair = new StackPanel { Orientation = Orientation.Horizontal };
        pair.Children.Add(_editAutoMain);
        pair.Children.Add(new Border { Width = 1, Height = 16, Background = Brush(Hairline), VerticalAlignment = VerticalAlignment.Center });
        pair.Children.Add(_editAutoMore);
        _editAuto = new Border
        {
            Child = pair,
            CornerRadius = new CornerRadius(999),
            BorderThickness = new Thickness(1),
            VerticalAlignment = VerticalAlignment.Center,
        };
        return _editAuto;
    }

    private static void RenderAutoButton()
    {
        if (_editAuto is null || _editAutoMain is null || _editAutoMore is null) return;
        bool on = (_editWand & WandAutoOn) != 0;
        bool busy = (_editWand & WandAutoBusy) != 0;
        _editAuto.Background = on ? Brush(ChromeColour.ControlAccent) : Brush(ChromeColour.SelectionWash);
        _editAuto.BorderBrush = on ? Brush(ChromeColour.ControlAccent) : Brush(ChromeColour.Accent);
        SolidColorBrush ink = on ? Brush(ChromeColour.OnAccent) : Brush(ChromeColour.Accent);
        _editAutoMain.Foreground = ink;
        _editAutoMore.Foreground = ink;
        if (_editAutoLabel is not null) _editAutoLabel.Foreground = ink;
        if (_editAutoIcon is not null)
        {
            _editAutoIcon.Glyph = on ? "\uE73E" : RailGlyph.Enhance;  // CheckMark while it is on
            _editAutoIcon.Visibility = busy ? Visibility.Collapsed : Visibility.Visible;
        }
        if (_editAutoSpinner is not null)
        {
            _editAutoSpinner.Root.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
            _editAutoSpinner.Active = busy;
        }
        _editAutoMain.IsEnabled = !busy && _editSubject != SubjectNone;
        string tip = on ? "Auto is on: click to take it back" : "Auto: better light and colour in one click (one undo step)";
        ToolTipService.SetToolTip(_editAutoMain, WithKey(tip, Command.AutoEnhance));
        AutomationProperties_SetName(_editAutoMain, on ? "Auto enhance, on" : "Auto enhance");
    }

    private static void BuildCropPane(StackPanel pane)
    {
        // Start / apply / cancel.
        var crop = new StackPanel { Spacing = Space(6) };
        crop.Children.Add(SectionTitle("Crop"));
        if (_editCropActive)
        {
            _editCropSize = Text("", Title, UiFontSize - 2);
            crop.Children.Add(_editCropSize);
            Button apply = AccentButton("Apply", () => Send(Command.CropCommit));
            apply.Padding = Pad(12, 5, 12, 5);
            ToolTipService.SetToolTip(apply, WithKey("Apply the crop", Command.CropCommit));
            crop.Children.Add(Row(
                apply,
                EditButton("Cancel", () => Send(Command.EditAction, EditActions.CancelCrop),
                           tip: "Drop this crop  Esc")));
        }
        else
        {
            crop.Children.Add(Row(EditButton("Crop and straighten", () => Send(Command.CropMode),
                                             tip: WithKey("Start cropping", Command.CropMode))));
        }
        pane.Children.Add(crop);

        // Aspect presets: two rows of plain buttons (one row from 560 DIP,
        // LayAspectGrid), the chosen one washed and outlined.
        var aspect = new StackPanel { Spacing = Space(6) };
        aspect.Children.Add(SectionTitle("Aspect ratio"));
        var grid = new Grid { ColumnSpacing = Space(4), RowSpacing = Space(4) };
        _editAspectGrid = grid;
        string? cycle = KeyOf(Command.CropAspectCycle);
        string? swap = KeyOf(Command.CropAspectSwap);
        for (int a = 0; a < AspectLabels.Length; a++)
        {
            int preset = a;
            Button b = EditButton(AspectLabels[a],
                () => Send(Command.CropAspectSet, preset + (_editPortrait && HasOrientation(preset) ? 16 : 0)),
                tip: cycle is null ? $"Aspect {AspectLabels[a]}" : $"Aspect {AspectLabels[a]}  {cycle} cycles while cropping",
                stretch: true);
            AutomationProperties_SetName(b, $"Aspect {AspectLabels[a]}");
            b.BorderBrush = Brush(Hairline);
            b.BorderThickness = new Thickness(0.5);
            grid.Children.Add(b);
            EditAspectButtons.Add(b);
        }
        LayAspectGrid();
        aspect.Children.Add(grid);
        _editOrient = EditButton("Landscape",
            () => Send(Command.CropAspectSet, _editAspect + (_editPortrait ? 0 : 16)),
            tip: swap is null ? "Swap portrait / landscape" : $"Swap portrait / landscape  {swap} while cropping");
        aspect.Children.Add(_editOrient);
        aspect.Children.Add(Hint("A next ratio · X swap, while cropping"));
        pane.Children.Add(aspect);

        // Straighten: ±45°, half-degree steps; the value is native's.
        var straighten = new StackPanel { Spacing = Space(6) };
        var head = new Grid();
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        TextBlock st = SectionTitle("Straighten");
        st.VerticalAlignment = VerticalAlignment.Center;
        head.Children.Add(st);
        _editStraightenText = Text("", Body, UiFontSize - 2);
        _editStraightenText.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(_editStraightenText, 1);
        head.Children.Add(_editStraightenText);
        Button level = EditButton("Level", () => Send(Command.CropStraightenSet, 0f), tip: "Back to 0°");
        level.Margin = Pad(6, 0, 0, 0);
        AutomationProperties_SetName(level, "Straighten to zero");
        Grid.SetColumn(level, 2);
        head.Children.Add(level);
        straighten.Children.Add(head);
        _editStraightenSlider = new Slider
        {
            Minimum = -45,
            Maximum = 45,
            StepFrequency = 0.5,
            SmallChange = 0.5,
            LargeChange = 5,
            IsThumbToolTipEnabled = false,
        };
        AutomationProperties_SetName(_editStraightenSlider, "Straighten, degrees");
        _editStraightenSlider.ValueChanged += (_, e) =>
        {
            if (_updatingEdit) return;
            _editStraighten = (float)e.NewValue;
            if (_editStraightenText is not null) _editStraightenText.Text = $"{e.NewValue:+0.0;-0.0;0.0}°";
            Send(Command.CropStraightenSet, (float)e.NewValue);
        };
        straighten.Children.Add(_editStraightenSlider);
        straighten.Children.Add(Hint(", . tilt 0.5° while cropping"));
        // Straighten beside rotate from 560 DIP, under it below (LayStraightenTurn).
        var pair = new Grid { ColumnSpacing = Space(18), RowSpacing = Space(16) };
        _editStraightenTurn = pair;
        pair.Children.Add(straighten);
        pane.Children.Add(pair);

        // Rotate and flip: the PR 10 keys.
        var turn = new StackPanel { Spacing = Space(6) };
        turn.Children.Add(SectionTitle("Rotate and flip"));
        // Icons with their keys in the tooltip, as on the Mac (issue #198):
        // "⟲" and "⟳" are not in CozetteVector.
        turn.Children.Add(Row(
            PaneIconButton(Glyph.Rotate, "Rotate left", KeyOf(Command.RotateCcw), () => Send(Command.RotateCcw),
                           new ScaleTransform { ScaleX = -1 }),
            PaneIconButton(Glyph.Rotate, "Rotate right", KeyOf(Command.RotateCw), () => Send(Command.RotateCw)),
            PaneIconButton(Glyph.FlipArrows, "Flip horizontal", KeyOf(Command.FlipHorizontal),
                           () => Send(Command.FlipHorizontal)),
            PaneIconButton(Glyph.FlipArrows, "Flip vertical", KeyOf(Command.FlipVertical),
                           () => Send(Command.FlipVertical), new RotateTransform { Angle = 90 })));
        turn.Children.Add(Hint("A JPEG with only turns is rewritten losslessly."));
        pair.Children.Add(turn);
        LayStraightenTurn();

        pane.Children.Add(Hint("While cropping: arrows move · Shift+arrows resize · Enter apply · Esc cancel"));
    }

    private static void BuildTrimPane(StackPanel pane)
    {
        bool armed = (_editTrimFlags & TrimArmedFlag) != 0;
        bool previewing = (_editTrimFlags & TrimPreviewingFlag) != 0;
        bool marked = (_editTrimFlags & TrimHasMarkerFlag) != 0;
        var trim = new StackPanel { Spacing = Space(6) };
        trim.Children.Add(SectionTitle("Trim"));
        if (armed)
        {
            _editTrimLabelText = Text("", Title, UiFontSize - 2);
            trim.Children.Add(_editTrimLabelText);
            trim.Children.Add(Row(
                EditButton("Set in", () => Send(Command.TrimIn), tip: WithKey("Set in", Command.TrimIn)),
                EditButton("Set out", () => Send(Command.TrimOut), tip: WithKey("Set out", Command.TrimOut)),
                EditButton(previewing ? "Stop preview" : "Preview", () => Send(Command.TrimPreview),
                           tip: WithKey("Loop the cut", Command.TrimPreview))));
            Button save = EditButton("Save", () => Send(Command.TrimKeyframe),
                                     tip: WithKey("Keyframe cut: instant, no re-encode", Command.TrimKeyframe));
            Button exact = EditButton("Save exact", () => Send(Command.TrimReencode),
                                      tip: WithKey("Frame-accurate re-encode, slower", Command.TrimReencode));
            Button without = EditButton("Copy without in–out", () => Send(Command.TrimRemoveMiddle),
                                        tip: WithKey("A copy without the marked range", Command.TrimRemoveMiddle));
            save.IsEnabled = exact.IsEnabled = without.IsEnabled = marked;
            EditTrimSaveButtons.Add(save);
            EditTrimSaveButtons.Add(exact);
            EditTrimSaveButtons.Add(without);
            trim.Children.Add(Row(save, exact));
            trim.Children.Add(Row(without, EditButton("Clear", () => Send(Command.TrimClear),
                                                      tip: WithKey("Clear in and out", Command.TrimClear))));
            trim.Children.Add(Row(EditButton("Stop trimming", () => Send(Command.TrimMode),
                                             tip: KeyOf(Command.TrimMode))));
        }
        else
        {
            trim.Children.Add(Row(EditButton("Start trimming", () => Send(Command.TrimMode),
                                             tip: WithKey("Set in and out points on the scrub bar", Command.TrimMode))));
        }
        pane.Children.Add(trim);

        var tools = new StackPanel { Spacing = Space(6) };
        tools.Children.Add(SectionTitle("Tools"));
        tools.Children.Add(Row(EditButton("Split at playhead", () => Send(Command.ClipSplit),
                                          tip: KeyOf(Command.ClipSplit))));
        tools.Children.Add(Row(EditButton("More clip tools…", () => Send(Command.ClipToolsFlyout),
                                          tip: KeyOf(Command.ClipToolsFlyout))));
        pane.Children.Add(tools);
        pane.Children.Add(Hint("Space play · , . frame step · J K L shuttle · Q E skip"));
    }

    // The seven ratios: four a row below 560 DIP, all seven in one from it.
    private static void LayAspectGrid()
    {
        if (_editAspectGrid is null) return;
        int columns = _editWidthClass >= WidthWide ? AspectLabels.Length : 4;
        if (_editAspectGrid.ColumnDefinitions.Count != columns)
        {
            _editAspectGrid.ColumnDefinitions.Clear();
            for (int c = 0; c < columns; c++)
                _editAspectGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        }
        int rows = (AspectLabels.Length + columns - 1) / columns;
        while (_editAspectGrid.RowDefinitions.Count < rows) _editAspectGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        while (_editAspectGrid.RowDefinitions.Count > rows) _editAspectGrid.RowDefinitions.RemoveAt(_editAspectGrid.RowDefinitions.Count - 1);
        for (int a = 0; a < EditAspectButtons.Count; a++)
        {
            Grid.SetRow(EditAspectButtons[a], a / columns);
            Grid.SetColumn(EditAspectButtons[a], a % columns);
        }
    }

    // Straighten and Rotate side by side from 560 DIP, stacked below (the same
    // two panels, moved between cells: never rebuilt, so a slider keeps focus).
    private static void LayStraightenTurn()
    {
        if (_editStraightenTurn is null || _editStraightenTurn.Children.Count < 2) return;
        bool side = _editWidthClass >= WidthWide;
        Grid g = _editStraightenTurn;
        int columns = side ? 2 : 1;
        if (g.ColumnDefinitions.Count != columns)
        {
            g.ColumnDefinitions.Clear();
            for (int c = 0; c < columns; c++) g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        }
        int rows = side ? 1 : 2;
        while (g.RowDefinitions.Count < rows) g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        while (g.RowDefinitions.Count > rows) g.RowDefinitions.RemoveAt(g.RowDefinitions.Count - 1);
        var first = (FrameworkElement)g.Children[0];
        var second = (FrameworkElement)g.Children[1];
        Grid.SetRow(first, 0);
        Grid.SetColumn(first, 0);
        Grid.SetRow(second, side ? 0 : 1);
        Grid.SetColumn(second, side ? 1 : 0);
    }

    // Free has none and a square's is itself; Original swaps like the ratios
    // (edit_session.cpp, EditStore.swift hasOrientation).
    private static bool HasOrientation(int aspect) => aspect != 0 && aspect != 2;

    // Everything that moves without a rebuild: labels, washes, enabled states,
    // and the slider (guarded, so native's echo of a drag is not re-sent).
    private static void RenderEditInPlace()
    {
        if (_editNameText is not null)
        {
            _editNameText.Text = _editName.Length > 0 ? _editName : EditTitle;
            ToolTipService.SetToolTip(_editNameText, _editName.Length > 0 ? _editName : null);
            AutomationProperties_SetName(_editNameText, $"{EditTitle}: {_editName}");
        }
        if (_editCountText is not null && _editCountPill is not null)
        {
            string count = _editCount == 1 ? "1 edit" : $"{_editCount} edits";
            _editCountText.Text = _editWidthClass == WidthCompact ? $"{_editCount}" : count;
            _editCountPill.Visibility = _editCount > 0 && !EditIsClip ? Visibility.Visible : Visibility.Collapsed;
            ToolTipService.SetToolTip(_editCountPill, count + " on this photo; the original is never changed");
            AutomationProperties_SetName(_editCountPill, count);
        }
        if (_editChip is not null && _editChipText is not null)
        {
            bool cloud = (_editChips & ChipCloud) != 0;
            _editChip.Visibility = cloud ? Visibility.Visible : Visibility.Collapsed;
            _editChipText.Text = "Cloud";
            ToolTipService.SetToolTip(_editChip, "Downloading the file from the cloud…");
        }
        RenderAutoButton();
        if (_editTab == TabEnhance) RenderEnhanceInPlace();
        bool canEdit = _editSubject != SubjectNone;
        if (_editUndo is not null) _editUndo.IsEnabled = _editCount > 0;
        if (_editReset is not null) _editReset.IsEnabled = _editCount > 0 || _editCropActive;
        if (_editOriginalButton is not null)
        {
            SetSelected(_editOriginalButton, _editOriginal);
            _editOriginalButton.IsEnabled = canEdit && CanCompare;
            ToolTipService.SetToolTip(_editOriginalButton, CompareTipWithKey);
            if (_editCompareHolder is not null)
                ToolTipService.SetToolTip(_editCompareHolder, _editOriginalButton.IsEnabled ? null : CompareTip);
        }
        if (_editCompareLabels is not null && _editCompareBefore is not null && _editCompareAfter is not null)
        {
            bool shown = _editOriginal && CanCompare;
            _editCompareLabels.Visibility = shown ? Visibility.Visible : Visibility.Collapsed;
            _editCompareBefore.Text = _editCompare == 2 ? "Before: the original photo" : "Before: no edits";
            _editCompareAfter.Text = _editCompare == 2 ? "After: Enhance" : "After: your edits";
            AutomationProperties_SetName(_editCompareLabels,
                $"{_editCompareBefore.Text}, left of the line. {_editCompareAfter.Text}, right of the line.");
        }
        if (_editCropSize is not null)
        {
            _editCropSize.Text = _editCropW > 0 ? $"{_editCropW} × {_editCropH} px" : "";
            AutomationProperties_SetName(_editCropSize, $"Crop size {_editCropW} by {_editCropH} pixels");
        }
        for (int i = 0; i < EditAspectButtons.Count; i++)
        {
            SetSelected(EditAspectButtons[i], i == _editAspect);
            EditAspectButtons[i].BorderBrush = i == _editAspect ? Brush(ChromeColour.Accent) : Brush(Hairline);
            EditAspectButtons[i].BorderThickness = new Thickness(i == _editAspect ? 1.5 : 0.5);
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetItemStatus(
                EditAspectButtons[i], i == _editAspect ? "Selected" : "");
        }
        if (_editOrient is not null)
        {
            SetButtonText(_editOrient, _editPortrait ? "Portrait" : "Landscape");
            _editOrient.IsEnabled = HasOrientation(_editAspect);
        }
        if (_editTrimLabelText is not null)
        {
            _editTrimLabelText.Text = _editTrimLabel;
            _editTrimLabelText.Visibility = _editTrimLabel.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        }
        bool trimMarked = (_editTrimFlags & TrimHasMarkerFlag) != 0;
        foreach (Button b in EditTrimSaveButtons) b.IsEnabled = trimMarked;
        if (_editStraightenText is not null) _editStraightenText.Text = $"{_editStraighten:+0.0;-0.0;0.0}°";
        if (_editStraightenSlider is not null && Math.Abs(_editStraightenSlider.Value - _editStraighten) > 1e-3)
        {
            _updatingEdit = true;
            try
            {
                _editStraightenSlider.Value = _editStraighten;
            }
            finally
            {
                _updatingEdit = false;
            }
        }
    }
}
